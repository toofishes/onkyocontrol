/*
 *  onkyo.c - Onkyo receiver communication program
 *  Dan McGee <dpmcgee@gmail.com>
 *
 *  Originally based on:
 *  miniterm.c by Sven Goldt <goldt@math.tu-berlin.de>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>

#define SERIALDEVICE "/dev/ttyS1"

/* TODO not sure if we will need these */
#define END_SEND '\r'
#define END_RECV 0x1A

#define BUF_SIZE 256

//#define _POSIX_SOURCE 1 /* POSIX compliant source */

const char * const CMD_PWR_ON = "PWRO1\n";
const char * const CMD_PWR_OFF = "PWRO0\r";
const char * const QST_PWR_STATUS = "PWRQSTN\r";

const char * const CMD_VOL_UP = "MVLUP\r";
const char * const CMD_VOL_DOWN = "MVLDOWN\r";
const char * const QST_VOL_STATUS = "MVLQSTN\r";

const char * const CMD_MUTE_ON = "AMT01\r";
const char * const CMD_MUTE_OFF = "AMT00\r";
const char * const QST_MUTE_STATUS = "AMTQSTN\r";

const char * const CMD_INPUT_DVD   = "SLI10\r";
const char * const CMD_INPUT_TUNER = "SLI26\r";
const char * const CMD_INPUT_TV    = "SLI02\r";
const char * const CMD_INPUT_CD    = "SLI23\r";
const char * const QST_INPUT_STATUS = "SLIQSTN\r";

/* our global file handle on the serial port and input socket */
int serialfd = -1;
int inputfd = -1;
int outputfd = -1;

/** 
 * Send a command to the receiver and ensure we have a response.
 * @param cmd the command to send to the receiver
 * @param status the status string returned by the receiver
 * @return 0 on success, -1 on failure
 */
static int send_command(const char *cmd, char **status)
{
	int cmdsize, retval;
	char buf[BUF_SIZE];
	fd_set fds;
	struct timeval tv;

	if(!cmd)
		return(-1);
	cmdsize = strlen(cmd);

	/* TODO get lock on serial device read? */

	/* write the command */
	retval = write(serialfd, cmd, cmdsize);
	if(retval < 0 || retval != cmdsize) {
		/* TODO handle write error */
		printf("cmd %s, returned %d, expected %d\n", cmd, retval, cmdsize);
		perror("send_command, write()");
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
		fprintf(stderr, "send_command, no response from receiver");
		return(-1);
	}
	/* if we got here, we have data available to read */
	retval = read(serialfd, &buf, BUF_SIZE - 1);
	buf[retval] = '\0';
	/* if we had a returned status, we are good to go */
	if(retval > 0)
		/* return the status message if asked for */
		if(status) 
			*status = strdup(buf);
		return(0);

	return(-1);
}

static int handle_status_message(char **status)
{
	int retval;
	char buf[BUF_SIZE];

	/* read the status message that should be present */
	retval = read(serialfd, &buf, BUF_SIZE - 1);
	buf[retval] = '\0';

	/* if we had a returned status, we are good to go */
	if(retval > 0)
		/* return the status message if asked for */
		if(status) 
			*status = strdup(buf);
		return(0);

	return(-1);
}

static void nicefy_status(const char *status)
{
	if(!status)
		return;
	if(strcmp(status, CMD_PWR_ON) == 0)
		printf("Power turned on\n");
	else if(strcmp(status, CMD_PWR_OFF) == 0)
		printf("Power turned off\n");
	else if(strcmp(status, CMD_INPUT_DVD) == 0)
		printf("DVD input selected\n");
	else if(strcmp(status, CMD_INPUT_TUNER) == 0)
		printf("Radio input selected\n");
	else if(strcmp(status, CMD_INPUT_TV) == 0)
		printf("TV input selected\n");
	else if(strcmp(status, CMD_INPUT_CD) == 0)
		printf("CD/Computer input selected\n");
	else
		printf("<<<<Unknown Message: %s>>>>\n", status);
}

int main(int argc, char *argv[])
{
	int retval;
	struct termios oldtio, newtio;
	fd_set monitorfds;

	/* TODO open our input socket */
	inputfd = 0; /* stdin */
	/* TODO open our output socket */
	outputfd = 1; /* stdout */

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
	/* add the Onkyo EOF char to stop canonical reads on */
	/* newtio.c_cc[VEOL] = END_RECV; */

	/* clean the line and activate the settings */
	tcflush(serialfd, TCIFLUSH);
	tcsetattr(serialfd, TCSANOW, &newtio);

	/* Terminal settings are all done. Now it is time to watch for input
	 * on our socket and handle it as necessary. We also handle incoming
	 * status messages from the receiver.
	 */
	for(;;) {
		int maxfd;

		FD_ZERO(&monitorfds);
		FD_SET(inputfd, &monitorfds);
		FD_SET(serialfd, &monitorfds);
		maxfd = (inputfd > serialfd ? inputfd : serialfd) + 1;

		retval = select(maxfd, &monitorfds, NULL, NULL, NULL);
		if(retval == -1 && errno == EINTR)
			continue;
		if(retval == -1) {
			perror("select()");
			tcsetattr(serialfd, TCSANOW, &oldtio);
			close(serialfd);
			exit(EXIT_FAILURE);
		}
		/* no timeout case, so if we get here we can read something */
		/* check to see if we have a status message waiting */
		if(FD_ISSET(serialfd, &monitorfds)) {
			char *status;
			handle_status_message(&status);
			nicefy_status(status);
			free(status);
		}
		/* check to see if we have input commands waiting */
		if(FD_ISSET(inputfd, &monitorfds)) {
			char *status;
			/* TEMP: consume the input on stdin */
			/* TODO: we need to read this in lines, not chunks */
			char buf[BUF_SIZE];
			read(inputfd, &buf, BUF_SIZE - 1);
			send_command(CMD_PWR_ON, &status);
			nicefy_status(status);
			free(status);
		}
	}

	/* TODO: add a sigint handler so we actually run the code below */

	/* reset our serial line back to its former condition */
	tcsetattr(serialfd, TCSANOW, &oldtio);
	close(serialfd);
	exit(EXIT_SUCCESS);
}

