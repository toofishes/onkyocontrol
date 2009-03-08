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
#include <string.h>

#include "onkyo.h"

#define MAIN_POWER 0x1
#define ZONE2_POWER 0x2

static struct status *status_list = NULL;

struct status {
	unsigned long hash;
	const char *key;
	const char *value;
	struct status *next;
};

/** 
 * Send a command to the receiver. This should be used when a write() to the
 * given file descriptor is known to be non-blocking; e.g. after a select()
 * call on the descriptor.
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
static int rcvr_handle_status(int serialfd, char **status)
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
			*status = malloc((retval + 1) * sizeof(char));
			if(*status)
				memcpy(*status, buf, retval + 1);
		}
		return(0);
	}

	fprintf(stderr, "handle_status, read value was empty\n");
	return(-1);
}

/**
 * Array to hold all status messages that can easily be transposed into
 * one of our own status messages. We can't handle all of them this way,
 * but for those we can this is a code and time saver.
 */
static const char * const statuses[][2] = {
	{ "PWR00", "OK:power:off\n" },
	{ "PWR01", "OK:power:on\n" },

	{ "AMT00", "OK:mute:off\n" },
	{ "AMT01", "OK:mute:on\n" },

	{ "SLI00", "OK:input:DVR\n" },
	{ "SLI01", "OK:input:Cable\n" },
	{ "SLI02", "OK:input:TV\n" },
	{ "SLI03", "OK:input:AUX\n" },
	{ "SLI10", "OK:input:DVD\n" },
	{ "SLI20", "OK:input:Tape\n" },
	{ "SLI22", "OK:input:Phono\n" },
	{ "SLI23", "OK:input:CD\n" },
	{ "SLI24", "OK:input:FM Tuner\n" },
	{ "SLI25", "OK:input:AM Tuner\n" },
	{ "SLI26", "OK:input:Tuner\n" },
	{ "SLI30", "OK:input:Multichannel\n" },
	{ "SLI31", "OK:input:XM Radio\n" },
	{ "SLI32", "OK:input:Sirius Radio\n" },
	{ "SLIFF", "OK:input:Audyssey Speaker Setup\n" },

	{ "LMD00", "OK:mode:Stereo\n" },
	{ "LMD01", "OK:mode:Direct\n" },
	{ "LMD0C", "OK:mode:All Channel Stereo\n" },
	{ "LMD00", "OK:mode:Mono\n" },
	{ "LMD11", "OK:mode:Pure Audio\n" },
	{ "LMD13", "OK:mode:Full Mono\n" },
	{ "LMD40", "OK:mode:Straight Decode\n" },
	{ "LMD42", "OK:mode:THX Cinema\n" },
	{ "LMD80", "OK:mode:Pro Logic IIx Movie\n" },
	{ "LMD81", "OK:mode:Pro Logic IIx Music\n" },
	{ "LMD82", "OK:mode:Neo:6 Cinema\n" },
	{ "LMD83", "OK:mode:Neo:6 Music\n" },
	{ "LMD84", "OK:mode:PLIIx THX Cinema\n" },
	{ "LMD85", "OK:mode:Neo:6 THX Cinema\n" },
	{ "LMD86", "OK:mode:Pro Logic IIx Game\n" },
	{ "LMD88", "OK:mode:Neural THX\n" },
	{ "LMDN/A", "ERROR:mode:Not Available\n" },

	{ "ZPW00", "OK:zone2power:off\n" },
	{ "ZPW01", "OK:zone2power:on\n" },

	{ "ZMT00", "OK:zone2mute:off\n" },
	{ "ZMT01", "OK:zone2mute:on\n" },

	{ "SLZ00", "OK:zone2input:DVR\n" },
	{ "SLZ01", "OK:zone2input:Cable\n" },
	{ "SLZ02", "OK:zone2input:TV\n" },
	{ "SLZ03", "OK:zone2input:AUX\n" },
	{ "SLZ10", "OK:zone2input:DVD\n" },
	{ "SLZ20", "OK:zone2input:Tape\n" },
	{ "SLZ22", "OK:zone2input:Phono\n" },
	{ "SLZ23", "OK:zone2input:CD\n" },
	{ "SLZ24", "OK:zone2input:FM Tuner\n" },
	{ "SLZ25", "OK:zone2input:AM Tuner\n" },
	{ "SLZ26", "OK:zone2input:Tuner\n" },
	{ "SLZ30", "OK:zone2input:Multichannel\n" },
	{ "SLZ31", "OK:zone2input:XM Radio\n" },
	{ "SLZ32", "OK:zone2input:Sirius Radio\n" },
	{ "SLZ7F", "OK:zone2input:Off\n" },
	{ "SLZ80", "OK:zone2input:Source\n" },

	{ "DIM00", "OK:Dimmer:Bright\n" },
	{ "DIM01", "OK:Dimmer:Dim\n" },
	{ "DIM02", "OK:Dimmer:Dark\n" },
	{ "DIM08", "OK:Dimmer:Bright (LED off)\n" },
};


/**
 * Hash the given string to an unsigned long value.
 * This is the standard sdbm hashing algorithm.
 * @param str string to hash
 * @return the hash value of the given string
 */
static unsigned long sdbm(const char *str)
{
	unsigned long hash = 0;
	int c;
	while((c = *str++))
		hash = c + (hash << 6) + (hash << 16) - hash;

	return(hash);
}

/**
 * Initialize our list of static statuses. This must be called before the first
 * call to process_incoming_message(). This initialization gives us a slight
 * performance gain by pre-hashing our status values to unsigned longs,
 * allowing the status lookups to be relatively low-cost.
 */
void init_statuses(void)
{
	unsigned int i, loopsize;
	/* compile-time constant, should be # rows in statuses */
	loopsize = sizeof(statuses) / sizeof(*statuses);
	struct status *ptr = status_list;
	for(i = 0; i < loopsize; i++) {
		struct status *st = malloc(sizeof(struct status));
		st->hash = sdbm(statuses[i][0]);
		st->key = statuses[i][0];
		st->value = statuses[i][1];
		st->next = NULL;

		if(!ptr) {
			status_list = st;
			ptr = st;
		} else {
			ptr->next = st;
			ptr = ptr->next;
		}
	}
}

/**
 * Form the human readable status message from the receiver return value.
 * @param status the receiver status message to make human readable
 * @return the human readable status message, must be freed
 */
static char *parse_status(const char *status)
{
	unsigned long hashval;
	char *trim, *sptr, *eptr, *ret = NULL;
	struct status *statuses = status_list;
	/* copy the string so we can trim the start and end portions off */
	trim = strdup(status);
	sptr = trim + strlen(START_RECV);
	eptr = strstr(sptr, END_RECV);
	if(eptr)
		*eptr = '\0';

	hashval = sdbm(sptr);
	while(statuses) {
		if(statuses->hash == hashval) {
			ret = strdup(statuses->value);
			break;
		}
		statuses = statuses->next;
	}
	if(ret) {
		free(trim);
		return(ret);
	}

	/* We couldn't use our easy method of matching statuses to messages,
	 * so handle the special cases. */

	if(strncmp(sptr, "MVL", 3) == 0 || strncmp(sptr, "ZVL", 3) == 0) {
		/* parse the volume number out */
		char *pos;
		/* read volume level in as a base 16 (hex) number */
		long level = strtol(sptr + 3, &pos, 16);
		if(*sptr == 'M') {
			/* main volume level */
			ret = calloc(10 + 3 + 1, sizeof(char));
			sprintf(ret, "OK:volume:%ld\n", level);
		} else {
			/* zone 2 volume level */
			ret = calloc(15 + 3 + 1, sizeof(char));
			sprintf(ret, "OK:zone2volume:%ld\n", level);
		}
	}

	else if(strncmp(sptr, "TUN", 3) == 0 || strncmp(sptr, "TUZ", 3) == 0) {
		/* parse the frequency number out */
		char *pos, *tunemsg;
		/* read frequency in as a base 10 number */
		long freq = strtol(sptr + 3, &pos, 10);
		/* decide whether we are main or zone 2 */
		if(sptr[2] == 'N') {
			tunemsg = "OK:tune:";
		} else {
			tunemsg = "OK:zone2tune:";
		}
		if(freq > 8000) {
			/* FM frequency, something like 09790 was read */
			ret = calloc(strlen(tunemsg) + 5 + 5, sizeof(char));
			sprintf(ret, "%s%3.1f FM\n", tunemsg, (double)freq / 100.0);
		} else {
			/* AM frequency, something like 00780 was read */
			ret = calloc(strlen(tunemsg) + 4 + 5, sizeof(char));
			sprintf(ret, "%s%ld AM\n", tunemsg, freq);
		}
	}

	else if(strncmp(sptr, "SLP", 3) == 0) {
		/* parse the minutes out */
		char *pos;
		/* read volume level in as a base 16 (hex) number */
		long mins = strtol(sptr + 3, &pos, 16);
		ret = calloc(9 + 3 + 1, sizeof(char));
		sprintf(ret, "OK:sleep:%ld\n", mins);
	}

	else {
		ret = calloc(8 + strlen(sptr) + 2, sizeof(char));
		sprintf(ret, "OK:todo:%s\n", sptr);
	}
	free(trim);
	return(ret);
}

/**
 * Return the bitmask value for the initial unknown power status.
 * @return the initial power status bitmask value
 */
int initial_power_status(void) {
	return(MAIN_POWER | ZONE2_POWER);
}

/**
 * Update the power status if necessary by looking at the given message. This
 * message may or may not be related to power; if it is not then the status
 * will not be updated. If it is, perform some bitmask-foo to update the power
 * status depending on what zone was turned on or off.
 * @param power the current power status bitmask value
 * @param msg the message to process
 * @return the new power status bitmask value
 */
int update_power_status(int power, const char *msg) {
	/* var is a bitmask, manage power/z2power separately */
	if(strcmp(msg, "OK:power:off\n") == 0) {
		power &= ~MAIN_POWER;
	} else if(strcmp(msg, "OK:power:on\n") == 0) {
		power |= MAIN_POWER;
	} else if(strcmp(msg, "OK:zone2power:off\n") == 0) {
		power &= ~ZONE2_POWER;
	} else if(strcmp(msg, "OK:zone2power:on\n") == 0) {
		power |= ZONE2_POWER;
	}
	return(power);
}

/**
 * Process a status message to be read from the receiver (one that the
 * receiver initiated). Return a human-readable status message.
 * @param serialfd the fd used for sending commands to the receiver
 * @return the human readable status message, must be freed
 */
char *process_incoming_message(int serialfd)
{
	int ret;
	char *msg, *status = NULL;

	/* get the output from the receiver */
	ret = rcvr_handle_status(serialfd, &status);
	if(ret != -1) {
		/* parse the return and output a status message */
		msg = parse_status(status);
	} else {
		msg = strdup("ERROR:Receiver Error\n");
	}

	free(status);
	return(msg);
}

/* vim: set ts=4 sw=4 noet: */
