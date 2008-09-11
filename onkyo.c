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
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>

#include "onkyo.h"

/* an enum useful for keeping track of two paired file descriptors */
enum pipehalfs { READ = 0, WRITE = 1 };

typedef struct _conn {
	int fd;
	char *recv_buf;
	char *recv_buf_pos;
	char *send_buf;
	char *send_buf_pos;
	time_t last;
} conn;

/* our list of serial devices and old settings we know about */
static int serialdevs[MAX_SERIALDEVS];
static struct termios serialdevs_oldtio[MAX_SERIALDEVS];
/* our list of listening sockets/descriptors we accept connections on */
static int listeners[MAX_LISTENERS];
/* our list of open connections we process commands on */
static conn connections[MAX_CONNECTIONS];
/* pipe used for async-safe signal handling in our select */
static int signalpipe[2] = { -1, -1 };


/**
 * End a connection by setting the file descriptor to -1 and freeing
 * all buffers. This method will not check the file descriptor value first
 * to make sure it is valid.
 * @param c the connection to end
 */
static void end_connection(conn *c)
{
	xclose(c->fd);
	c->fd = -1;
	free(c->recv_buf);
	c->recv_buf = NULL;
	c->recv_buf_pos = NULL;
	free(c->send_buf);
	c->send_buf = NULL;
	c->send_buf_pos = NULL;
}

/* define here so we can add the noreturn attribute */
static void cleanup(int ret) __attribute__ ((noreturn));

/**
 * Cleanup all resources associated with our program, including memory,
 * open devices, files, sockets, etc. This function will not return.
 * The complete list is the following:
 * * serial devices (reset and close)
 * * our listeners
 * * any open connections
 * * our internal signal pipe
 * * our user command list
 * @arg ret
 */
static void cleanup(int ret)
{
	int i;

	/* loop through all our serial devices and reset/close them */
	for(i = 0; i < MAX_SERIALDEVS; i++) {
		if(serialdevs[i] > -1) {
			tcsetattr(serialdevs[i], TCSANOW, &(serialdevs_oldtio[i]));
			xclose(serialdevs[i]);
			serialdevs[i] = -1;
		}
	}

	/* loop through listener descriptors and close them */
	for(i = 0; i < MAX_LISTENERS; i++) {
		if(listeners[i] > -1) {
			xclose(listeners[i]);
			listeners[i] = -1;
		}
	}

	/* loop through connection descriptors and close them */
	for(i = 0; i < MAX_CONNECTIONS; i++) {
		if(connections[i].fd > -1) {
			end_connection(&connections[i]);
		}
	}

	/* close our signal listener */
	if(signalpipe[WRITE] > -1) {
		xclose(signalpipe[WRITE]);
		signalpipe[WRITE] = -1;
	}
	if(signalpipe[READ] > -1) {
		xclose(signalpipe[READ]);
		signalpipe[READ] = -1;
	}

	free_commands();
	exit(ret);
}

/**
 * Show the current status of our serial devices, listeners, and
 * connections. Print out the file descriptor integers for each thing
 * we are keeping an eye on.
 */
static void show_status(void)
{
	int i;

	printf("serial devices:\n  ");
	for(i = 0; i < MAX_SERIALDEVS; i++) {
		printf("%d ", serialdevs[i]);
	}
	printf("\nlisteners:\n  ");
	for(i = 0; i < MAX_LISTENERS; i++) {
		printf("%d ", listeners[i]);
	}
	printf("\nconnections:\n  ");
	for(i = 0; i < MAX_CONNECTIONS; i++) {
		printf("%d ", connections[i].fd);
	}
	printf("\n");
}

/**
 * Handle a signal in an async-safe fashion. The signal is written
 * to a pipe monitored in our main select() loop and will be handled
 * just as the other file descriptors there are.
 * @param signo the signal number
 */
static void pipehandler(int signo)
{
	if(signalpipe[WRITE] > -1) {
		/* write is async safe. write the signal number to the pipe. */
		xwrite(signalpipe[WRITE], &signo, sizeof(int));
	}
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
	} else if(signo == SIGPIPE) {
		fprintf(stderr, "attempted IO to a closed socket/pipe\n");
	} else if(signo == SIGUSR1) {
		show_status();
	}
}

/**
 * Open the serial device at the given path for use as a destination
 * receiver. Also adds it to our global list of serial devices.
 * @param path the path to the serial device, e.g. "/dev/ttyS0"
 * @return the serial device file descriptor
 */
static int open_serial_device(const char *path)
{
	int i, fd;
	struct termios newtio, oldtio;

	/* make sure we have room in our list */
	for(i = 0; i < MAX_SERIALDEVS && serialdevs[i] > -1; i++)
		/* no body, find first available spot */;
	if(i == MAX_SERIALDEVS) {
		fprintf(stderr, "max serial devices reached!\n");
		return(-1);
	}

	/* Open serial device for reading and writing, but not as controlling
	 * TTY because we don't want to get killed if linenoise sends CTRL-C.
	 */
	fd = open(path, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror(path);
		return(-1);
	}

	/* save current serial port settings */
	tcgetattr(fd, &oldtio);

	memset(&newtio, 0, sizeof(struct termios));
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
	tcflush(fd, TCIOFLUSH);
	tcsetattr(fd, TCSAFLUSH, &newtio);

	/* place the device and old settings in our arrays */
	serialdevs[i] = fd;
	memcpy(&(serialdevs_oldtio[i]), &oldtio, sizeof(struct termios));
	return(fd);
}

/**
 * Open a listening socket on the given bind address and port number.
 * Also add it to our global list of listeners.
 * @param host the hostname to bind to; NULL or "any" for all addresses
 * @param port the port number to listen on
 * @return the new socket fd
 */
static int open_listener(const char *host, int port)
{
	int i, fd;
	struct sockaddr_in sin;
	const int trueval = 1;

	/* make sure we have room in our list */
	for(i = 0; i < MAX_LISTENERS && listeners[i] > -1; i++)
		/* no body, find first available spot */;
	if(i == MAX_LISTENERS) {
		fprintf(stderr, "max listeners reached!\n");
		return(-1);
	}

	/* open an inet socket with the default protocol */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror(host);
		return(-1);
	}
	/* set up our connection host, port, etc. */
	memset(&sin, 0, sizeof(struct sockaddr_in));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	/* determine our resolved host address */
	if(!host || strcmp(host, "any") == 0) {
		sin.sin_addr.s_addr = INADDR_ANY;
	} else {
		struct hostent *he;
		if(!(he = gethostbyname(host))) {
			perror(host);
			return(-1);
		}
		/* assumption- using IPv4, he->h_addrtype = AF_INET */
		memcpy((char *)&sin.sin_addr.s_addr,
				(char *)he->h_addr, he->h_length);
	}
	/* set the ability to reuse local addresses */
	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&trueval,
				sizeof(int)) < 0) {
		perror("setsockopt()");
		return(-1);
	}
	/* bind to the given address */
	if(bind(fd, (struct sockaddr *)&sin, sizeof(struct sockaddr)) < 0) {
		perror("bind()");
		return(-1);
	}
	/* start listening */
	if(listen(fd, 5) < 0) {
		perror("listen()");
		return(-1);
	}

	/* place the listener in our array */
	listeners[i] = fd;
	return(fd);
}

/**
 * Establish everything we need for a connection once it has been
 * accepted. This will set up send and receive buffers and start
 * tracking the connection in our array.
 * @param fd the newly opened connections file descriptor
 */
static void open_connection(int fd)
{
	int i;
	/* add it to our list */
	for(i = 0; i < MAX_CONNECTIONS && connections[i].fd > -1; i++)
		/* no body, find first available spot */;
	if(i == MAX_CONNECTIONS) {
		fprintf(stderr, "max connections reached!\n");
		xclose(fd);
		return;
	}
	connections[i].fd = fd;
	connections[i].recv_buf = calloc(BUF_SIZE, sizeof(char));
	connections[i].recv_buf_pos = connections[i].recv_buf;
	/*
	connections[i].send_buf = calloc(BUF_SIZE, sizeof(char));
	connections[i].send_buf_pos = connections[i].send_buf;
	*/
}

/**
 * Process input from our input file descriptor and chop it into commands.
 * @param c the connection to read, write, and buffer from
 * @param serialfd the fd used for sending commands to the receiver
 * @return 0 on success, -1 on end of (input) file, and -2 on attempted
 * buffer overflow
 */
static int process_input(conn c, int serialfd) {
	/* a convienence ptr one past the end of our buffer */
	char * const end_pos = &c.recv_buf[BUF_SIZE];

	int ret = 0;
	int count;

	/*
	 * Picture time! Let's get overly verbose since we don't do this
	 * stuff too often.
	 *
	 *           ( read #1 ) ( read #2 ... )
	 * recv_buf: [v] [o] [l] [u] [m] [e] [\n] [p] [ ] ... [ ] [ ]
	 *       recv_buf_pos-----^                       end_pos-----^
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

	count = xread(c.fd, c.recv_buf_pos, end_pos - c.recv_buf_pos);
	if(count == 0)
		ret = -1;
	/* loop through each character we read. We are looking for newlines
	 * so we can parse out and execute one command. */
	while(count > 0) {
		if(*c.recv_buf_pos == '\n') {
			/* We have a newline. This means we should have a full command
			 * and can attempt to interpret it. */
			*c.recv_buf_pos = '\0';
			process_command(c.fd, serialfd, c.recv_buf);
			/* now move our remaining buffer to the start of our buffer */
			c.recv_buf_pos++;
			memmove(c.recv_buf, c.recv_buf_pos, count);
			c.recv_buf_pos = c.recv_buf;
			memset(&(c.recv_buf_pos[count]), 0,
					end_pos - &(c.recv_buf_pos[count]));
		}
		else if(end_pos - c.recv_buf_pos <= 1) {
			/* We have a buffer overflow, we haven't seen a newline yet
			 * and we are on the last character available in our buffer.
			 * Squash whatever is in our buffer. */
			fprintf(stderr, "process_input, buffer size exceeded\n");
			c.recv_buf_pos = c.recv_buf;
			memset(c.recv_buf, 0, BUF_SIZE);
			ret = -2;
			break;
		}
		else {
			/* nothing special, just keep looking */
			c.recv_buf_pos++;
		}
		count--;
	}

	return(ret);
}

int main(int argc, char *argv[])
{
	int i, retval;
	struct sigaction sa;
	fd_set readfds;

	/* set our file descriptor arrays to -1 */
	for(i = 0; i < MAX_SERIALDEVS; i++)
		serialdevs[i] = -1;
	for(i = 0; i < MAX_LISTENERS; i++)
		listeners[i] = -1;
	for(i = 0; i < MAX_CONNECTIONS; i++)
		connections[i].fd = -1;

	/* set up our signal handler */
	pipe(signalpipe);
	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = &pipehandler;
	sa.sa_flags = SA_RESTART;
	sa.sa_flags = 0;
	sigfillset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	/* open the serial connection to the receiver */
	open_serial_device(SERIALDEVICE);

	/* init our command list */
	init_commands();

	/* open our listener connection */
	open_listener(NULL, 8701);

	/* Terminal settings are all done. Now it is time to watch for input
	 * on our socket and handle it as necessary. We also handle incoming
	 * status messages from the receiver.
	 *
	 * Attempt to keep the crazyness in order:
	 * signalpipe, serialdevs, listeners, connections
	 */
	for(;;) {
		int maxfd = -1;

		/* get our file descriptor sets set up */
		FD_ZERO(&readfds);
		/* add our signal pipe file descriptor */
		FD_SET(signalpipe[READ], &readfds);
		maxfd = signalpipe[READ] > maxfd ? signalpipe[READ] : maxfd;
		/* add all of our serial devices */
		for(i = 0; i < MAX_SERIALDEVS; i++) {
			if(serialdevs[i] > -1) {
				FD_SET(serialdevs[i], &readfds);
				maxfd = serialdevs[i] > maxfd ? serialdevs[i] : maxfd;
			}
		}
		/* add all of our listeners */
		for(i = 0; i < MAX_LISTENERS; i++) {
			if(listeners[i] > -1) {
				FD_SET(listeners[i], &readfds);
				maxfd = listeners[i] > maxfd ? listeners[i] : maxfd;
			}
		}
		/* add all of our active connections */
		for(i = 0; i < MAX_CONNECTIONS; i++) {
			if(connections[i].fd > -1) {
				FD_SET(connections[i].fd, &readfds);
				maxfd = connections[i].fd > maxfd ? connections[i].fd : maxfd;
			}
		}

		/* our main waiting point */
		retval = select(maxfd + 1, &readfds, NULL, NULL, NULL);
		if(retval == -1 && errno == EINTR)
			continue;
		if(retval == -1) {
			perror("select()");
			cleanup(EXIT_FAILURE);
		}
		/* check to see if we have signals waiting */
		if(FD_ISSET(signalpipe[READ], &readfds)) {
			int signo;
			/* We don't want to read more than one signal out of the pipe.
			 * Anything else in there will be handled the next go-around. */
			xread(signalpipe[READ], &signo, sizeof(int));
			realhandler(signo);
		}
		/* check to see if we have a status message on our serial devices */
		for(i = 0; i < MAX_SERIALDEVS; i++) {
			if(serialdevs[i] > -1
					&& FD_ISSET(serialdevs[i], &readfds)) {
				/* TODO eventually factor hardcoded output dev out of here,
				 * we just had to pick one to send status messages to. */
				process_status(fileno(stdout), serialdevs[i]);
			}
		}
		/* check to see if we have listeners ready to accept */
		for(i = 0; i < MAX_LISTENERS; i++) {
			if(listeners[i] > -1
					&& FD_ISSET(listeners[i], &readfds)) {
				/* accept the incoming connection on the socket */
				struct sockaddr sa;
				socklen_t sl = sizeof(struct sockaddr);
				int fd = accept(listeners[i], &sa, &sl);
				if(fd >= 0) {
					open_connection(fd);
				} else if(fd < 0 && (errno != EAGAIN && errno != EINTR)) {
					perror("accept()");
				}
			}
		}
		/* check if we have connections with data ready to read */
		for(i = 0; i < MAX_CONNECTIONS; i++) {
			if(connections[i].fd > -1
					&& FD_ISSET(connections[i].fd, &readfds)) {
				/* TODO eventually factor serialdevs[0] out of here */
				int ret = process_input(connections[i], serialdevs[0]);
				if(ret == -1) {
					/* the connection hit EOF, kill it. */
					end_connection(&connections[i]);
				}
			}
		}
	}
}

