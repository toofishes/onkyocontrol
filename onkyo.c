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
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>

#include "onkyo.h"

//#define _POSIX_SOURCE 1 /* POSIX compliant source */

/* our global file handle on the serial port and input socket */
static int serialfd = -1;
static int inputfd = -1;
static int outputfd = -1;

struct termios oldtio;

/* pipe used for async-safe signal handling in our select */
enum sigpipehalfs { READ = 0, WRITE = 1 };
static int signalpipe[2] = { -1, -1 };

static void cleanup(int ret) __attribute__ ((noreturn));

static void cleanup(int ret)
{
	free_commands();
	if(serialfd > -1) {
		/* reset our serial line back to its former condition */
		tcsetattr(serialfd, TCSANOW, &oldtio);
		close(serialfd);
		serialfd = -1;
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

static void realhandler(int signo)
{
	if(signo == SIGINT) {
		fprintf(stderr, "\ninterrupt signal received\n");
		cleanup(EXIT_SUCCESS);
	}
}

/** 
 * Send a command to the receiver and ensure we have a response.
 * @param cmd the command to send to the receiver
 * @param status the status string returned by the receiver
 * @return 0 on success, -1 on failure
 */
int rcvr_send_command(const char *cmd, char **status)
{
	int cmdsize, retval;
	fd_set fds;
	struct timeval tv;

	if(!cmd)
		return(-1);
	cmdsize = strlen(cmd);

	/* write the command */
	retval = write(serialfd, cmd, cmdsize);
	if(retval < 0 || retval != cmdsize) {
		fprintf(stderr, "send_command, write returned %d\n", retval);
		return(-1);
	}
	/* we are now going to watch for a writeback */
	FD_ZERO(&fds);
	FD_SET(serialfd, &fds);
	/* receiver will respond with a status message within 50 ms */
	tv.tv_sec = 0;
	tv.tv_usec = 50 /*ms*/ * 1000;
	/* start monitoring our single serial FD with the given timeout */
	retval = select(serialfd + 1, &fds, NULL, NULL, &tv);
	/* check our return value */
	if(retval == -1) {
		perror("send_command, select()");
		return(-1);
	} else if(retval == 0) {
		fprintf(stderr, "send_command, no response from receiver\n");
		return(-1);
	}
	/* if we got here, we have data available to read */
	return rcvr_handle_status(status);
}

/** 
 * Handle a pending status message coming from the receiver. This is most
 * likely called after a select() on the serial fd returned that a read will
 * not block.
 * @param status the status string returned by the receiver
 * @return 0 on success, -1 on failure
 */
int rcvr_handle_status(char **status)
{
	int retval;
	char buf[BUF_SIZE];

	/* read the status message that should be present */
	retval = read(serialfd, &buf, BUF_SIZE - 1);
	/* TODO handle EINTR */
	buf[retval] = '\0';

	/* if we had a returned status, we are good to go */
	if(retval > 0) {
		/* return the status message if asked for */
		if(status) 
			*status = strdup(buf);
		return(0);
	}

	fprintf(stderr, "handle_status, read value was empty\n");
	return(-1);
}

/**
 * Process input from our input file descriptor and chop it into commands.
 * @return 0 on success, -1 on end of (input) file, and -2 on attempted
 * buffer overflow
 */
int process_input(void) {
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
			process_command(outputfd, inputbuf);
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
	struct termios newtio;
	fd_set monitorfds;

	/* TODO open our input socket */
	inputfd = 0; /* stdin */
	/* TODO open our output socket */
	outputfd = 1; /* stdout */

	/* set up our signal handler */
	pipe(signalpipe);
	sa.sa_handler = &pipehandler;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	/* Open serial device for reading and writing, but not as controlling
	 * TTY because we don't want to get killed if linenoise sends CTRL-C.
	 */
	serialfd = open(SERIALDEVICE, O_RDWR | O_NOCTTY);
	if (serialfd < 0) {
		perror(SERIALDEVICE);
		exit(EXIT_FAILURE);
	}

	/* save current serial port settings */
	tcgetattr(serialfd,&oldtio);

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
	tcflush(serialfd, TCIOFLUSH);
	tcsetattr(serialfd, TCSAFLUSH, &newtio);

	/* init our command list */
	init_commands();

	/* Terminal settings are all done. Now it is time to watch for input
	 * on our socket and handle it as necessary. We also handle incoming
	 * status messages from the receiver.
	 */
	for(;;) {
		int maxfd;

		/* get our file descriptor set all set up */
		FD_ZERO(&monitorfds);
		FD_SET(inputfd, &monitorfds);
		maxfd = inputfd;
		FD_SET(serialfd, &monitorfds);
		maxfd = serialfd > maxfd ? serialfd : maxfd;
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
		/* check to see if we have a status message waiting */
		if(FD_ISSET(serialfd, &monitorfds)) {
			process_status(outputfd);
		}
		/* check to see if we have input commands waiting */
		if(FD_ISSET(inputfd, &monitorfds)) {
			int ret = process_input();
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

