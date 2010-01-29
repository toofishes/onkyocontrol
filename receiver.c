/*
 *  receiver.c - Onkyo receiver interaction code
 *
 *  Copyright (c) 2008, 2009 Dan McGee <dpmcgee@gmail.com>
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

#define _BSD_SOURCE 1 /* strdup */
#define _GNU_SOURCE 1 /* memmem */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "onkyo.h"

extern const char * const rcvr_err;

static struct status *status_list = NULL;

struct status {
	unsigned long hash;
	const char *key;
	const char *value;
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
	size_t cmdsize;
	ssize_t retval;

	if(!cmd)
		return(-1);
	cmdsize = strlen(cmd);

	/* write the command */
	retval = xwrite(serialfd, cmd, cmdsize);
	if(retval < 0 || ((size_t)retval) != cmdsize) {
		fprintf(stderr, "send_command, write returned %zd\n", retval);
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
 * @return the read size on success, -1 on failure
 */
static int rcvr_handle_status(int serialfd, char **status)
{
	ssize_t retval;
	char buf[BUF_SIZE];

	memset(buf, 0, BUF_SIZE);
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
		return(retval);
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
	{ "SLI04", "OK:input:AUX2\n" },
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
	{ "LMD07", "OK:mode:Mono Movie\n" },
	{ "LMD08", "OK:mode:Orchestra\n" },
	{ "LMD09", "OK:mode:Unplugged\n" },
	{ "LMD0A", "OK:mode:Studio-Mix\n" },
	{ "LMD0B", "OK:mode:TV Logic\n" },
	{ "LMD0C", "OK:mode:All Channel Stereo\n" },
	{ "LMD0D", "OK:mode:Theater-Dimensional\n" },
	{ "LMD0F", "OK:mode:Mono\n" },
	{ "LMD10", "OK:mode:Test Tone\n" },
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
	{ "LMDN/A", "ERROR:mode:N/A\n" },

	{ "MEMLOCK", "OK:memory:locked\n" },
	{ "MEMUNLK", "OK:memory:unlocked\n" },
	{ "MEMN/A",  "ERROR:memory:N/A\n" },

	{ "ZPW00", "OK:zone2power:off\n" },
	{ "ZPW01", "OK:zone2power:on\n" },

	{ "ZMT00", "OK:zone2mute:off\n" },
	{ "ZMT01", "OK:zone2mute:on\n" },

	{ "ZVLN/A", "ERROR:zone2volume:N/A\n" },

	{ "SLZ00", "OK:zone2input:DVR\n" },
	{ "SLZ01", "OK:zone2input:Cable\n" },
	{ "SLZ02", "OK:zone2input:TV\n" },
	{ "SLZ03", "OK:zone2input:AUX\n" },
	{ "SLZ04", "OK:zone2input:AUX2\n" },
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

	{ "PW300", "OK:zone3power:off\n" },
	{ "PW301", "OK:zone3power:on\n" },

	{ "MT300", "OK:zone3mute:off\n" },
	{ "MT301", "OK:zone3mute:on\n" },

	{ "VL3N/A", "ERROR:zone3volume:N/A\n" },

	{ "SL300", "OK:zone3input:DVR\n" },
	{ "SL301", "OK:zone3input:Cable\n" },
	{ "SL302", "OK:zone3input:TV\n" },
	{ "SL303", "OK:zone3input:AUX\n" },
	{ "SL304", "OK:zone3input:AUX2\n" },
	{ "SL310", "OK:zone3input:DVD\n" },
	{ "SL320", "OK:zone3input:Tape\n" },
	{ "SL322", "OK:zone3input:Phono\n" },
	{ "SL323", "OK:zone3input:CD\n" },
	{ "SL324", "OK:zone3input:FM Tuner\n" },
	{ "SL325", "OK:zone3input:AM Tuner\n" },
	{ "SL326", "OK:zone3input:Tuner\n" },
	{ "SL330", "OK:zone3input:Multichannel\n" },
	{ "SL331", "OK:zone3input:XM Radio\n" },
	{ "SL332", "OK:zone3input:Sirius Radio\n" },
	{ "SL37F", "OK:zone3input:Off\n" },
	{ "SL380", "OK:zone3input:Source\n" },

	{ "DIF00", "OK:display:Volume\n" },
	{ "DIF01", "OK:display:Mode\n" },
	{ "DIF02", "OK:display:Digital Format\n" },
	{ "DIFN/A", "ERROR:display:N/A\n" },

	{ "DIM00", "OK:dimmer:Bright\n" },
	{ "DIM01", "OK:dimmer:Dim\n" },
	{ "DIM02", "OK:dimmer:Dark\n" },
	{ "DIM03", "OK:dimmer:Shut-off\n" },
	{ "DIM08", "OK:dimmer:Bright (LED off)\n" },
	{ "DIMN/A", "ERROR:dimmer:N/A\n" },

	{ "LTN00", "OK:latenight:off\n" },
	{ "LTN01", "OK:latenight:low\n" },
	{ "LTN02", "OK:latenight:high\n" },

	{ "RAS00", "OK:re-eq:off\n" },
	{ "RAS01", "OK:re-eq:on\n" },

	{ "HDO00", "OK:hdmiout:off\n" },
	{ "HDO01", "OK:hdmiout:on\n" },

	{ "RES00", "OK:resolution:Through\n" },
	{ "RES01", "OK:resolution:Auto\n" },
	{ "RES02", "OK:resolution:480p\n" },
	{ "RES03", "OK:resolution:720p\n" },
	{ "RES04", "OK:resolution:1080i\n" },
	{ "RES05", "OK:resolution:1080p\n" },

	{ "SLA00", "OK:audioselector:Auto\n" },
	{ "SLA01", "OK:audioselector:Multichannel\n" },
	{ "SLA02", "OK:audioselector:Analog\n" },
	{ "SLA03", "OK:audioselector:iLink\n" },
	{ "SLA04", "OK:audioselector:HDMI\n" },

	{ "TGA00", "OK:triggera:off\n" },
	{ "TGA01", "OK:triggera:on\n" },
	{ "TGAN/A", "ERROR:triggera:N/A\n" },

	{ "TGB00", "OK:triggerb:off\n" },
	{ "TGB01", "OK:triggerb:on\n" },
	{ "TGBN/A", "ERROR:triggerb:N/A\n" },

	{ "TGC00", "OK:triggerc:off\n" },
	{ "TGC01", "OK:triggerc:on\n" },
	{ "TGCN/A", "ERROR:triggerc:N/A\n" },

	/* Do not remove! */
	{ NULL,    NULL },
};

/**
 * Initialize our list of static statuses. This must be called before the first
 * call to process_incoming_message(). This initialization gives us a slight
 * performance gain by pre-hashing our status values to unsigned longs,
 * allowing the status lookups to be relatively low-cost.
 */
void init_statuses(void)
{
	unsigned int i, loopsize;
	struct status *st;
	/* compile-time constant, should be # rows in statuses */
	loopsize = sizeof(statuses) / sizeof(*statuses);
	st = status_list = malloc(loopsize * sizeof(struct status));

	for(i = 0; i < loopsize; i++) {
		st->hash = hash_sdbm(statuses[i][0]);
		st->key = statuses[i][0];
		st->value = statuses[i][1];
		st++;
	}
	printf("%u status messages prehashed in status list.\n", loopsize);
}

/**
 * Free our list of statuses.
 */
void free_statuses(void)
{
	struct status *st = status_list;
	status_list = NULL;
	free(st);
}

/**
 * Form the human readable status message from the receiver return value.
 * Note that the status parameter is freely modified as necessary and
 * should not be expected to be readable after this method has completed.
 * @param size the length of the status message as it might contain null
 * bytes
 * @param status the receiver status message to make human readable
 * @return the human readable status message, must be freed
 */
static char *parse_status(int size, char *status)
{
	unsigned long hashval;
	char *sptr, *eptr, *ret = NULL;
	struct status *st = status_list;
	/* Trim the start and end portions off. We want to strip any leading
	 * garbage, including null bytes, and just start where we find the
	 * START_RECV characters. */
	sptr = memmem(status, size, START_RECV, strlen(START_RECV));
	if(sptr) {
		sptr += strlen(START_RECV);
		eptr = strstr(sptr, END_RECV);
		if(eptr)
			*eptr = '\0';
	} else {
		/* Hmm, we couldn't find the start chars. WTF? */
		ret = strdup(rcvr_err);
		return(ret);
	}

	hashval = hash_sdbm(sptr);
	/* this depends on the {NULL, NULL} keypair at the end of the list */
	while(st->hash != 0) {
		if(st->hash == hashval) {
			ret = strdup(st->value);
			break;
		}
		st++;
	}
	if(ret) {
		return(ret);
	}

	/* We couldn't use our easy method of matching statuses to messages,
	 * so handle the special cases. */

	if(strncmp(sptr, "MVL", 3) == 0 || strncmp(sptr, "ZVL", 3) == 0
			|| strncmp(sptr, "VL3", 3) == 0) {
		/* parse the volume number out */
		char *pos;
		/* read volume level in as a base 16 (hex) number */
		long level = strtol(sptr + 3, &pos, 16);
		if(*sptr == 'M') {
			/* main volume level */
			ret = calloc(10 + 3 + 1, sizeof(char));
			sprintf(ret, "OK:volume:%ld\n", level);
		} else if(*sptr == 'Z') {
			/* zone 2 volume level */
			ret = calloc(15 + 3 + 1, sizeof(char));
			sprintf(ret, "OK:zone2volume:%ld\n", level);
		} else if(*sptr == 'V') {
			/* zone 3 volume level */
			ret = calloc(15 + 3 + 1, sizeof(char));
			sprintf(ret, "OK:zone3volume:%ld\n", level);
		}
	}

	/* TUN, TUZ, TU3 */
	else if(strncmp(sptr, "TU", 2) == 0) {
		/* parse the frequency number out */
		char *pos, *tunemsg;
		/* read frequency in as a base 10 number */
		long freq = strtol(sptr + 3, &pos, 10);
		/* decide whether we are main or zones */
		if(sptr[2] == 'N') {
			tunemsg = "OK:tune:";
		} else if(sptr[2] == 'Z') {
			tunemsg = "OK:zone2tune:";
		} else if(sptr[2] == '3') {
			tunemsg = "OK:zone3tune:";
		}
		if(freq > 8000) {
			/* FM frequency, something like 09790 was read */
			ret = calloc(strlen(tunemsg) + 5 + 5, sizeof(char));
			/* Use some awesome integer math to format the output */
			sprintf(ret, "%s%ld.%ld FM\n", tunemsg, freq / 100, (freq / 10) % 10);
		} else {
			/* AM frequency, something like 00780 was read */
			ret = calloc(strlen(tunemsg) + 4 + 5, sizeof(char));
			sprintf(ret, "%s%ld AM\n", tunemsg, freq);
		}
	}

	else if(strncmp(sptr, "PRS", 3) == 0 || strncmp(sptr, "PRZ", 3) == 0
			|| strncmp(sptr, "PR3", 3) == 0) {
		/* parse the preset number out */
		char *pos, *prsmsg;
		/* read value in as a base 16 (hex) number */
		long value = strtol(sptr + 3, &pos, 16);
		/* decide whether we are main or zones */
		if(sptr[2] == 'S') {
			prsmsg = "OK:preset:";
		} else if(sptr[2] == 'Z') {
			prsmsg = "OK:zone2preset:";
		} else if(sptr[2] == '3') {
			prsmsg = "OK:zone3preset:";
		}
		ret = calloc(strlen(prsmsg) + 2 + 1, sizeof(char));
		sprintf(ret, "%s%ld\n", prsmsg, value);
	}

	else if(strncmp(sptr, "SLP", 3) == 0) {
		/* parse the minutes out */
		char *pos;
		/* read sleep timer in as a base 16 (hex) number */
		long mins = strtol(sptr + 3, &pos, 16);
		ret = calloc(9 + 3 + 1, sizeof(char));
		sprintf(ret, "OK:sleep:%ld\n", mins);
	}

	else if(strncmp(sptr, "SWL", 3) == 0) {
		/* parse the level out */
		char *pos;
		/* read volume level in as a base 16 (hex) number */
		long level = strtol(sptr + 3, &pos, 16);
		ret = calloc(11 + 3 + 1, sizeof(char));
		sprintf(ret, "OK:swlevel:%+ld\n", level);
	}

	else if(strncmp(sptr, "AVS", 3) == 0) {
		/* parse the time value out */
		char *pos;
		/* read time value in as a base 10 number */
		long level = strtol(sptr + 3, &pos, 10);
		/* AVS1000 -> 100 ms delay */
		level /= 10;
		ret = calloc(11 + 3 + 1, sizeof(char));
		sprintf(ret, "OK:avsync:%ld\n", level);
	}

	else {
		ret = calloc(8 + strlen(sptr) + 2, sizeof(char));
		sprintf(ret, "OK:todo:%s\n", sptr);
	}
	return(ret);
}

/**
 * Return the bitmask value for the initial unknown power status.
 * @return the initial power status bitmask value
 */
enum power initial_power_status(void) {
	return(POWER_OFF);
}

/**
 * Update the power status if necessary by looking at the given message. This
 * message may or may not be related to power; if it is not then the status
 * will not be updated. If it is, perform some bitmask-foo to update the power
 * status depending on what zone was turned on or off.
 * @param pwr the current power status bitmask value
 * @param msg the message to process
 * @return the new power status bitmask value
 */
enum power update_power_status(enum power pwr, const char *msg) {
	/* var is a bitmask, manage power/z2power separately */
	if(strcmp(msg, "OK:power:off\n") == 0) {
		pwr &= ~MAIN_POWER;
	} else if(strcmp(msg, "OK:power:on\n") == 0) {
		pwr |= MAIN_POWER;
	} else if(strcmp(msg, "OK:zone2power:off\n") == 0) {
		pwr &= ~ZONE2_POWER;
	} else if(strcmp(msg, "OK:zone2power:on\n") == 0) {
		pwr |= ZONE2_POWER;
	} else if(strcmp(msg, "OK:zone3power:off\n") == 0) {
		pwr &= ~ZONE3_POWER;
	} else if(strcmp(msg, "OK:zone3power:on\n") == 0) {
		pwr |= ZONE3_POWER;
	}
	return(pwr);
}

/**
 * Process a status message to be read from the receiver (one that the
 * receiver initiated). Return a human-readable status message.
 * @param serialfd the fd used for sending commands to the receiver
 * @param logfd the fd used for logging raw status messages
 * @return the human readable status message, must be freed
 */
char *process_incoming_message(int serialfd, int logfd)
{
	int size;
	char *msg, *status = NULL;

	/* get the output from the receiver */
	size = rcvr_handle_status(serialfd, &status);
	if(size != -1) {
		/* log the message if we have a logfd */
		if(logfd > 0) {
			xwrite(logfd, status, size + 1);
		}
		/* parse the return and output a status message */
		msg = parse_status(size, status);
	} else {
		msg = strdup(rcvr_err);
	}

	free(status);
	return(msg);
}

/* vim: set ts=4 sw=4 noet: */
