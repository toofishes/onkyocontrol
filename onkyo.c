/*
 *  onkyo.c - Onkyo receiver communication program
 *
 *  Copyright (c) 2008 Dan McGee <dpmcgee@gmail.com>
 *
 *  Originally based on:
 *  miniterm.c - Sven Goldt <goldt@math.tu-berlin.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>

#include "onkyo.h"

/* struct containing serial port descriptor and config info */
struct serialdev {
	int fd;
	struct termios oldtio;
	struct serialdev *next;
};

/* an enum useful for keeping track of two paired file descriptors */
enum pipehalfs { READ = 0, WRITE = 1 };

/* our list of serial devices we know about */
static struct serialdev *serialdevs = NULL;
/* our list of listening sockets/descriptors we process commands on */
static int *listeners[2];

/* pipe used for async-safe signal handling in our select */
static int signalpipe[2] = { -1, -1 };

/* define here so we can add the noreturn attribute */
static void cleanup(int ret) __attribute__ ((noreturn));

static void cleanup(int ret)
{
	free_commands();
	while(serialdevs) {
		struct serialdev *next = serialdevs->next;
		/* reset and close our serial devices */
		tcsetattr(serialdevs->fd, TCSANOW, &(serialdevs->oldtio));
		close(serialdevs->fd);
		free(serialdevs);
		serialdevs = next;
	}
	if(signalpipe[WRITE] > -1) {
		close(signalpipe[WRITE]);
		signalpipe[WRITE] = -1;
	}
	if(signalpipe[READ] > -1) {
		close(signalpipe[READ]);
		signalpipe[READ] = -1;
	}
	exit(ret);
}

/**
 * Handle a signal in an async-safe fashion. The signal is written
 * to a pipe monitored in our main select() loop and will be handled
 * just as the other file descriptors there are.
 * @param signo the signal number
 */
static void pipehandler(int signo)
{
	/* write is async safe. write the signal number to the pipe. */
	write(signalpipe[WRITE], &signo, sizeof(int));
}

/**
 * Handler for signals called when a signal was detected in our select()
 * loop. This ensures we can safely handle the signal and not have weird
 * interactions with interrupted system calls and other such fun. This
 * should never be called from within the signal handler set via sigaction.
 * @param signo the signal number
 */
static void realhandler(int signo)
{
	if(signo == SIGINT) {
		fprintf(stderr, "\ninterrupt signal received\n");
		cleanup(EXIT_SUCCESS);
	}
}

/**
 * Open the serial device at the given path for use as a destination
 * receiver.
 * @param path the path to the serial device, e.g. "/dev/ttyS0"
 * @return the serial device struct consisting of a fd and the old settings
 */
static struct serialdev *open_serial_device(const char *path)
{
	struct termios newtio;
	struct serialdev *dev = NULL;

	dev = calloc(1, sizeof(struct serialdev));
	/* Open serial device for reading and writing, but not as controlling
	 * TTY because we don't want to get killed if linenoise sends CTRL-C.
	 */
	dev->fd = open(path, O_RDWR | O_NOCTTY);
	if (dev->fd < 0) {
		perror(path);
		free(dev);
		return(NULL);
	}

	/* save current serial port settings */
	tcgetattr(dev->fd, &(dev->oldtio));

	/* Set:
	 * B9600 - 9600 baud
	 * No flow control
	 * CS8 - 8n1 (8 bits, no parity, 1 stop bit)
	 * Don't hangup automatically
	 * CLOCAL - ignore modem status
	 * CREAD - enable receiving characters
	 */
	newtio.c_cflag = B9600 | CS8 | CLOCAL | CREAD;
	/* ignore bytes with parity errors and make terminal raw and dumb */
	newtio.c_iflag = IGNPAR;
	/* raw output mode */
	newtio.c_oflag = 0;
	/* canonical input mode- end read at a line descriptor */
	newtio.c_lflag = ICANON;
	/* add the Onkyo-used EOF char to allow canonical read */
	newtio.c_cc[VEOL] = END_RECV_CHAR;

	/* clean the line and activate the settings */
	tcflush(dev->fd, TCIOFLUSH);
	tcsetattr(dev->fd, TCSAFLUSH, &newtio);

	return(dev);
}

/**
 * Process input from our input file descriptor and chop it into commands.
 * @param inputfd the fd to process input from
 * @param outputfd the fd used for any eventual output
 * @param serialfd the fd used for sending commands to the receiver
 * @return 0 on success, -1 on end of (input) file, and -2 on attempted
 * buffer overflow
 */
static int process_input(int inputfd, int outputfd, int serialfd) {
	/* static vars used to buffer our input into line-oriented commands */
	static char inputbuf[BUF_SIZE];
	static char *pos = inputbuf;
	static char * const end_pos = &inputbuf[BUF_SIZE - 1];

	int ret = 0;
	int count;

	/*
	 * Picture time! Let's get overly verbose since we don't do this
	 * stuff too often.
	 *
	 *           ( read #1 ) ( read #2 ... )
	 * inputbuf: [v] [o] [l] [u] [m] [e] [\n] [p] [ ] ... [ ] [ ]
	 *                pos-----^                    end_pos-----^
	 * count: 5
	 *
	 * Basically we will perform a read, getting as many chars as are
	 * available. If we didn't get a newline on the first read, then
	 * pos will be located at the end of the previous read. If we find
	 * a newline, we will substring from start to pos (excluding newline)
	 * and pass that off to our command parser. We will then memmove
	 * anything remaining in our buffer to the beginning and continue
	 * the cycle.
	 */

	count = read(inputfd, pos, end_pos - pos + 1);
	/* TODO handle EINTR */
	if(count == 0)
		ret = -1;
	/* loop through each character we read. We are looking for newlines
	 * so we can parse out and execute one command. */
	while(count > 0) {
		if(*pos == '\n') {
			/* We have a newline. This means we should have a full command
			 * and can attempt to interpret it. */
			*pos = '\0';
			process_command(outputfd, serialfd, inputbuf);
			/* now move our remaining buffer to the start of our buffer */
			pos++;
			memmove(inputbuf, pos, count);
			pos = inputbuf;
			memset(&(pos[count]), 0, end_pos - &(pos[count]));
		}
		else if(end_pos - pos <= 0) {
			/* We have a buffer overflow, we haven't seen a newline yet.
			 * Squash whatever is in our buffer. */
			fprintf(stderr, "process_input, buffer size exceeded\n");
			pos = inputbuf;
			memset(inputbuf, 0, BUF_SIZE);
			ret = -2;
			break;
		}
		else {
			/* nothing special, just keep looking */
			pos++;
		}
		count--;
	}

	return(ret);
}

int main(int argc, char *argv[])
{
	int retval;
	struct sigaction sa;
	fd_set monitorfds;

	/* necessary file handles */
	int inputfd = -1;
	int outputfd = -1;

	/* TODO open our input socket */
	inputfd = 0; /* stdin */
	/* TODO open our output socket */
	outputfd = 1; /* stdout */

	/* set up our signal handler */
	pipe(signalpipe);
	sa.sa_handler = &pipehandler;
	/*sa.sa_flags = SA_RESTART;*/
	sa.sa_flags = 0;
	sigfillset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	/* open the serial connection to the receiver */
	serialdevs = open_serial_device(SERIALDEVICE);

	/* init our command list */
	init_commands();

	/* Terminal settings are all done. Now it is time to watch for input
	 * on our socket and handle it as necessary. We also handle incoming
	 * status messages from the receiver.
	 */
	for(;;) {
		int maxfd;
		struct serialdev *dev;

		/* get our file descriptor set all set up */
		FD_ZERO(&monitorfds);
		FD_SET(inputfd, &monitorfds);
		maxfd = inputfd;
		/* add all of our serial devices */
		dev = serialdevs;
		while(dev) {
			FD_SET(dev->fd, &monitorfds);
			maxfd = dev->fd > maxfd ? dev->fd : maxfd;
			dev = dev->next;
		}
		/* add our signal pipe file descriptor */
		FD_SET(signalpipe[READ], &monitorfds);
		maxfd = signalpipe[READ] > maxfd ? signalpipe[READ] : maxfd;
		maxfd++;

		/* our main waiting point */
		retval = select(maxfd, &monitorfds, NULL, NULL, NULL);
		if(retval == -1 && errno == EINTR)
			continue;
		if(retval == -1) {
			perror("select()");
			cleanup(EXIT_FAILURE);
		}
		/* no timeout case, so if we get here we can read something */
		/* check to see if we have signals waiting */
		if(FD_ISSET(signalpipe[READ], &monitorfds)) {
			int signo;
			/* We don't want to read more than one signal out of the pipe.
			 * Anything else in there will be handled the next go-around. */
			read(signalpipe[READ], &signo, sizeof(int));
			realhandler(signo);
		}
		/* check to see if we have a status message on our serial devices */
		dev = serialdevs;
		while(dev) {
			if(FD_ISSET(dev->fd, &monitorfds)) {
				process_status(outputfd, dev->fd);
			}
			dev = dev->next;
		}
		/* check to see if we have input commands waiting */
		if(FD_ISSET(inputfd, &monitorfds)) {
			/* TODO eventually factor serialdevs->fd out of here */
			int ret = process_input(inputfd, outputfd, serialdevs->fd);
			if(ret == -1) {
				/* the file hit EOF, kill it. */
				close(inputfd);
				inputfd = -1;
				break;
			}
		}
	}

	cleanup(EXIT_SUCCESS);
}

