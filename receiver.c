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
#include <unistd.h>
#include <string.h>

#include "onkyo.h"

/** 
 * Send a command to the receiver and ensure we have a response.
 * @param serialfd the file descriptor the receiver is accessible on
 * @param cmd the command to send to the receiver
 * @return 0 on success, -1 on failure
 */
int rcvr_send_command(int serialfd, const char *cmd)
{
	int cmdsize, retval;

	if(!cmd)
		return(-1);
	cmdsize = strlen(cmd);

	/* write the command */
	retval = xwrite(serialfd, cmd, cmdsize);
	if(retval < 0 || retval != cmdsize) {
		fprintf(stderr, "send_command, write returned %d\n", retval);
		return(-1);
	}
	return(0);
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
		if(status)  {
			const size_t len = strlen(buf) + 1;
			*status = malloc(len * sizeof(char));
			if(*status)
				memcpy(*status, buf, len);
		}
		return(0);
	}

	fprintf(stderr, "handle_status, read value was empty\n");
	return(-1);
}

/* vim: set ts=4 sw=4 noet: */
