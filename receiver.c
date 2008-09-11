/*
 *  receiver.c - Onkyo receiver interaction code
 *
 *  Copyright (c) 2008 Dan McGee <dpmcgee@gmail.com>
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
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "onkyo.h"

/** 
 * Send a command to the receiver and ensure we have a response.
 * @param serialfd the file descriptor the receiver is accessible on
 * @param cmd the command to send to the receiver
 * @param status the status string returned by the receiver
 * @return 0 on success, -1 on failure
 */
int rcvr_send_command(int serialfd, const char *cmd, char **status)
{
	int cmdsize, retval;
	fd_set fds;
	struct timeval tv;

	if(!cmd)
		return(-1);
	cmdsize = strlen(cmd);

	/* write the command */
	retval = xwrite(serialfd, cmd, cmdsize);
	if(retval < 0 || retval != cmdsize) {
		fprintf(stderr, "send_command, write returned %d\n", retval);
		return(-1);
	}
	/* receiver will respond with a status message within 50 ms */
	tv.tv_sec = 0;
	tv.tv_usec = 50 /*ms*/ * 1000;
	do {
		/* we are now going to watch for a writeback */
		FD_ZERO(&fds);
		FD_SET(serialfd, &fds);
		/* start monitoring our single serial FD with the given timeout */
		retval = select(serialfd + 1, &fds, NULL, NULL, &tv);
		/* make sure we weren't interrupted while waiting; if so run again */
	} while(retval == -1 && errno == EINTR);
	/* check our return value */
	if(retval == -1) {
		perror("send_command, select()");
		return(-1);
	} else if(retval == 0) {
		fprintf(stderr, "send_command, no response from receiver\n");
		return(-1);
	}
	/* if we got here, we have data available to read */
	return rcvr_handle_status(serialfd, status);
}

/** 
 * Handle a pending status message coming from the receiver. This is most
 * likely called after a select() on the serial fd returned that a read will
 * not block. It may also be used to get the status message that is returned
 * after sending a command.
 * @param serialfd the file descriptor the receiver is accessible on
 * @param status the status string returned by the receiver
 * @return 0 on success, -1 on failure
 */
int rcvr_handle_status(int serialfd, char **status)
{
	int retval;
	char buf[BUF_SIZE];

	/* read the status message that should be present */
	retval = xread(serialfd, &buf, BUF_SIZE - 1);

	/* if we had a returned status, we are good to go */
	if(retval > 0) {
		buf[retval] = '\0';
		/* return the status message if asked for */
		if(status) 
			*status = strdup(buf);
		return(0);
	}

	fprintf(stderr, "handle_status, read value was empty\n");
	return(-1);
}

