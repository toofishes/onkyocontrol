/*
 *  receiver.c - Onkyo receiver interaction code
 *
 *  Copyright (c) 2008-2010 Dan McGee <dpmcgee@gmail.com>
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

/** A mapping of receiver status value to returned message */
struct status {
	unsigned long hash;
	const char *key;
	const char *value;
};

/** A mapping of receiver power status value to returned message as well as
 * helping to internally track power on/off state */
struct power_status {
	unsigned long hash;
	const char *key;
	const char *value;
	int zone;
	int power;
};

/**
 * Get the next receiver command that should be sent. This implementation has
 * logic to discard non-power commands if the receiver is not powered up.
 * @param rcvr the receiver to pull a command out of the queue for
 * @return the command to send (must be freed), NULL if none available
 */
static struct cmdqueue *next_rcvr_command(struct receiver *rcvr)
{
	/* Determine whether we should send the command. This depends on two
	 * factors:
	 * 1. If the power is on, always send the command.
	 * 2. If the power is off, send only power commands through.
	 */
	while(rcvr->queue) {
		/* dequeue the next cmd queue item */
		struct cmdqueue *ptr = rcvr->queue;
		rcvr->queue = rcvr->queue->next;

		if(rcvr->power || is_power_command(ptr->cmd)) {
			return ptr;
		} else {
			printf("skipping command as receiver power appears to be off\n");
			free(ptr);
		}
	}
	return NULL;
}

/** 
 * Send a command to the receiver. This should be used when a write() to the
 * given file descriptor is known to be non-blocking; e.g. after a select()
 * call on the descriptor.
 * @param rcvr the receiver to send a command to from the attached queue
 * @return 0 on success or no action taken, -1 on failure
 */
int rcvr_send_command(struct receiver *rcvr)
{
	struct cmdqueue *ptr;

	if(!rcvr->queue)
		return -1;

	ptr = next_rcvr_command(rcvr);
	if(ptr) {
		ssize_t retval;
		size_t cmdsize;
		char fullcmd[BUF_SIZE * 2];

		cmdsize = strlen(START_SEND) + strlen(ptr->cmd) + strlen(END_SEND);
		if(cmdsize >= BUF_SIZE * 2) {
			fprintf(stderr, "send_command, command too large: %zd\n", cmdsize);
			return -1;
		}

		sprintf(fullcmd, START_SEND "%s" END_SEND, ptr->cmd);

		/* write the command */
		retval = xwrite(rcvr->fd, fullcmd, cmdsize);
		/* set our last sent time */
		gettimeofday(&(rcvr->last_cmd), NULL);
		/* print command to console; newline is already in command */
		printf("command:  %s", fullcmd);
		free(ptr);

		if(retval < 0 || ((size_t)retval) != cmdsize) {
			fprintf(stderr, "send_command, write returned %zd\n", retval);
			printf("%s", rcvr_err);
			return -1;
		}
		rcvr->cmds_sent++;
	}
	return 0;
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
		return retval;
	}

	fprintf(stderr, "handle_status, read value was empty\n");
	return -1;
}

/**
 * Array to hold all status messages that can easily be transposed into
 * one of our own status messages. We can't handle all of them this way,
 * but for those we can this is a code and time saver.
 */
static struct status statuses[] = {
	{ 0, "AMT00", "OK:mute:off\n" },
	{ 0, "AMT01", "OK:mute:on\n" },

	{ 0, "SLI00", "OK:input:DVR\n" },
	{ 0, "SLI01", "OK:input:Cable\n" },
	{ 0, "SLI02", "OK:input:TV\n" },
	{ 0, "SLI03", "OK:input:AUX\n" },
	{ 0, "SLI04", "OK:input:AUX2\n" },
	{ 0, "SLI05", "OK:input:PC\n" },
	{ 0, "SLI10", "OK:input:DVD\n" },
	{ 0, "SLI20", "OK:input:Tape\n" },
	{ 0, "SLI22", "OK:input:Phono\n" },
	{ 0, "SLI23", "OK:input:CD\n" },
	{ 0, "SLI24", "OK:input:FM Tuner\n" },
	{ 0, "SLI25", "OK:input:AM Tuner\n" },
	{ 0, "SLI26", "OK:input:Tuner\n" },
	{ 0, "SLI27", "OK:input:Music Server\n" },
	{ 0, "SLI28", "OK:input:Internet Radio\n" },
	{ 0, "SLI29", "OK:input:USB\n" },
	{ 0, "SLI2A", "OK:input:USB Rear\n" },
	{ 0, "SLI40", "OK:input:Port\n" },
	{ 0, "SLI30", "OK:input:Multichannel\n" },
	{ 0, "SLI31", "OK:input:XM Radio\n" },
	{ 0, "SLI32", "OK:input:Sirius Radio\n" },
	{ 0, "SLIFF", "OK:input:Audyssey Speaker Setup\n" },

	{ 0, "LMD00", "OK:mode:Stereo\n" },
	{ 0, "LMD01", "OK:mode:Direct\n" },
	{ 0, "LMD07", "OK:mode:Mono Movie\n" },
	{ 0, "LMD08", "OK:mode:Orchestra\n" },
	{ 0, "LMD09", "OK:mode:Unplugged\n" },
	{ 0, "LMD0A", "OK:mode:Studio-Mix\n" },
	{ 0, "LMD0B", "OK:mode:TV Logic\n" },
	{ 0, "LMD0C", "OK:mode:All Channel Stereo\n" },
	{ 0, "LMD0D", "OK:mode:Theater-Dimensional\n" },
	{ 0, "LMD0F", "OK:mode:Mono\n" },
	{ 0, "LMD10", "OK:mode:Test Tone\n" },
	{ 0, "LMD11", "OK:mode:Pure Audio\n" },
	{ 0, "LMD13", "OK:mode:Full Mono\n" },
	{ 0, "LMD15", "OK:mode:DTS Surround Sensation\n" },
	{ 0, "LMD16", "OK:mode:Audyssey DSX\n" },
	{ 0, "LMD40", "OK:mode:Straight Decode\n" },
	{ 0, "LMD41", "OK:mode:Dolby EX/DTS ES\n" },
	{ 0, "LMD42", "OK:mode:THX Cinema\n" },
	{ 0, "LMD43", "OK:mode:THX Surround EX\n" },
	{ 0, "LMD44", "OK:mode:THX Music\n" },
	{ 0, "LMD45", "OK:mode:THX Games\n" },
	{ 0, "LMD80", "OK:mode:Pro Logic IIx Movie\n" },
	{ 0, "LMD81", "OK:mode:Pro Logic IIx Music\n" },
	{ 0, "LMD82", "OK:mode:Neo:6 Cinema\n" },
	{ 0, "LMD83", "OK:mode:Neo:6 Music\n" },
	{ 0, "LMD84", "OK:mode:PLIIx THX Cinema\n" },
	{ 0, "LMD85", "OK:mode:Neo:6 THX Cinema\n" },
	{ 0, "LMD86", "OK:mode:Pro Logic IIx Game\n" },
	{ 0, "LMD88", "OK:mode:Neural THX\n" },
	{ 0, "LMDN/A", "ERROR:mode:N/A\n" },

	{ 0, "MEMLOCK", "OK:memory:locked\n" },
	{ 0, "MEMUNLK", "OK:memory:unlocked\n" },
	{ 0, "MEMN/A",  "ERROR:memory:N/A\n" },

	{ 0, "ZMT00", "OK:zone2mute:off\n" },
	{ 0, "ZMT01", "OK:zone2mute:on\n" },

	{ 0, "ZVLN/A", "ERROR:zone2volume:N/A\n" },

	{ 0, "SLZ00", "OK:zone2input:DVR\n" },
	{ 0, "SLZ01", "OK:zone2input:Cable\n" },
	{ 0, "SLZ02", "OK:zone2input:TV\n" },
	{ 0, "SLZ03", "OK:zone2input:AUX\n" },
	{ 0, "SLZ04", "OK:zone2input:AUX2\n" },
	{ 0, "SLZ10", "OK:zone2input:DVD\n" },
	{ 0, "SLZ20", "OK:zone2input:Tape\n" },
	{ 0, "SLZ22", "OK:zone2input:Phono\n" },
	{ 0, "SLZ23", "OK:zone2input:CD\n" },
	{ 0, "SLZ24", "OK:zone2input:FM Tuner\n" },
	{ 0, "SLZ25", "OK:zone2input:AM Tuner\n" },
	{ 0, "SLZ26", "OK:zone2input:Tuner\n" },
	{ 0, "SLZ30", "OK:zone2input:Multichannel\n" },
	{ 0, "SLZ31", "OK:zone2input:XM Radio\n" },
	{ 0, "SLZ32", "OK:zone2input:Sirius Radio\n" },
	{ 0, "SLZ7F", "OK:zone2input:Off\n" },
	{ 0, "SLZ80", "OK:zone2input:Source\n" },

	{ 0, "MT300", "OK:zone3mute:off\n" },
	{ 0, "MT301", "OK:zone3mute:on\n" },

	{ 0, "VL3N/A", "ERROR:zone3volume:N/A\n" },

	{ 0, "SL300", "OK:zone3input:DVR\n" },
	{ 0, "SL301", "OK:zone3input:Cable\n" },
	{ 0, "SL302", "OK:zone3input:TV\n" },
	{ 0, "SL303", "OK:zone3input:AUX\n" },
	{ 0, "SL304", "OK:zone3input:AUX2\n" },
	{ 0, "SL310", "OK:zone3input:DVD\n" },
	{ 0, "SL320", "OK:zone3input:Tape\n" },
	{ 0, "SL322", "OK:zone3input:Phono\n" },
	{ 0, "SL323", "OK:zone3input:CD\n" },
	{ 0, "SL324", "OK:zone3input:FM Tuner\n" },
	{ 0, "SL325", "OK:zone3input:AM Tuner\n" },
	{ 0, "SL326", "OK:zone3input:Tuner\n" },
	{ 0, "SL330", "OK:zone3input:Multichannel\n" },
	{ 0, "SL331", "OK:zone3input:XM Radio\n" },
	{ 0, "SL332", "OK:zone3input:Sirius Radio\n" },
	{ 0, "SL37F", "OK:zone3input:Off\n" },
	{ 0, "SL380", "OK:zone3input:Source\n" },

	{ 0, "DIF00", "OK:display:Volume\n" },
	{ 0, "DIF01", "OK:display:Mode\n" },
	{ 0, "DIF02", "OK:display:Digital Format\n" },
	{ 0, "DIFN/A", "ERROR:display:N/A\n" },

	{ 0, "DIM00", "OK:dimmer:Bright\n" },
	{ 0, "DIM01", "OK:dimmer:Dim\n" },
	{ 0, "DIM02", "OK:dimmer:Dark\n" },
	{ 0, "DIM03", "OK:dimmer:Shut-off\n" },
	{ 0, "DIM08", "OK:dimmer:Bright (LED off)\n" },
	{ 0, "DIMN/A", "ERROR:dimmer:N/A\n" },

	{ 0, "LTN00", "OK:latenight:off\n" },
	{ 0, "LTN01", "OK:latenight:low\n" },
	{ 0, "LTN02", "OK:latenight:high\n" },

	{ 0, "RAS00", "OK:re-eq:off\n" },
	{ 0, "RAS01", "OK:re-eq:on\n" },

	{ 0, "ADY00", "OK:audyssey:off\n" },
	{ 0, "ADY01", "OK:audyssey:on\n" },
	{ 0, "ADQ00", "OK:dynamiceq:off\n" },
	{ 0, "ADQ01", "OK:dynamiceq:on\n" },

	{ 0, "HDO00", "OK:hdmiout:off\n" },
	{ 0, "HDO01", "OK:hdmiout:on\n" },

	{ 0, "RES00", "OK:resolution:Through\n" },
	{ 0, "RES01", "OK:resolution:Auto\n" },
	{ 0, "RES02", "OK:resolution:480p\n" },
	{ 0, "RES03", "OK:resolution:720p\n" },
	{ 0, "RES04", "OK:resolution:1080i\n" },
	{ 0, "RES05", "OK:resolution:1080p\n" },

	{ 0, "SLA00", "OK:audioselector:Auto\n" },
	{ 0, "SLA01", "OK:audioselector:Multichannel\n" },
	{ 0, "SLA02", "OK:audioselector:Analog\n" },
	{ 0, "SLA03", "OK:audioselector:iLink\n" },
	{ 0, "SLA04", "OK:audioselector:HDMI\n" },

	{ 0, "TGA00", "OK:triggera:off\n" },
	{ 0, "TGA01", "OK:triggera:on\n" },
	{ 0, "TGAN/A", "ERROR:triggera:N/A\n" },

	{ 0, "TGB00", "OK:triggerb:off\n" },
	{ 0, "TGB01", "OK:triggerb:on\n" },
	{ 0, "TGBN/A", "ERROR:triggerb:N/A\n" },

	{ 0, "TGC00", "OK:triggerc:off\n" },
	{ 0, "TGC01", "OK:triggerc:on\n" },
	{ 0, "TGCN/A", "ERROR:triggerc:N/A\n" },

	/* Do not remove! */
	{ 0, NULL,    NULL },
};

static struct power_status power_statuses[] = {
	{ 0, "PWR00", "OK:power:off\n",      1, 0 },
	{ 0, "PWR01", "OK:power:on\n",       1, 1 },

	{ 0, "ZPW00", "OK:zone2power:off\n", 2, 0 },
	{ 0, "ZPW01", "OK:zone2power:on\n",  2, 1 },

	{ 0, "PW300", "OK:zone3power:off\n", 3, 0 },
	{ 0, "PW301", "OK:zone3power:on\n",  3, 1 },

	/* Do not remove! */
	{ 0, NULL,    NULL,            -1,-1 },
};

/**
 * Initialize our list of static statuses. This must be called before the first
 * call to process_incoming_message(). This initialization gives us a slight
 * performance gain by pre-hashing our status values to unsigned longs,
 * allowing the status lookups to be relatively low-cost.
 */
void init_statuses(void)
{
	unsigned int status_count = 0;
	struct status *status;
	struct power_status *pwr_status;
	for(status = statuses; status->key; status++) {
		status->hash = hash_sdbm(status->key);
		status_count++;
	}
	for(pwr_status = power_statuses; pwr_status->key; pwr_status++) {
		pwr_status->hash = hash_sdbm(pwr_status->key);
		status_count++;
	}
	printf("%u status messages prehashed in status list.\n", status_count);
}

static void update_power_status(struct receiver *rcvr, int zone, int value);

/**
 * Form the human readable status message from the receiver return value.
 * Note that the status parameter is freely modified as necessary and
 * should not be expected to be readable after this method has completed.
 * @param rcvr the receiver the message was received from
 * @param size the length of the status message as it might contain null
 * bytes
 * @param status the receiver status message to make human readable
 * @return 0 on normal status, -1 on parse errors
 */
static int parse_status(struct receiver *rcvr, int size, char *status)
{
	unsigned long hashval;
	char buf[BUF_SIZE];
	char *sptr, *eptr;
	struct status *st;
	struct power_status *pwr_st;

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
		write_to_connections(rcvr_err);
		return -1;
	}

	hashval = hash_sdbm(sptr);
	/* this depends on the {NULL} entry at the end of the list */
	st = statuses;
	while(st->hash != 0) {
		if(st->hash == hashval) {
			write_to_connections(st->value);
			return 0;
		}
		st++;
	}

	pwr_st = power_statuses;
	while(pwr_st->hash != 0) {
		if(pwr_st->hash == hashval) {
			update_power_status(rcvr, pwr_st->zone, pwr_st->power);
			write_to_connections(pwr_st->value);
			return 0;
		}
		pwr_st++;
	}

	/* We couldn't use our easy method of matching statuses to messages,
	 * so handle the special cases. */

	if(strncmp(sptr, "MVL", 3) == 0 || strncmp(sptr, "ZVL", 3) == 0
			|| strncmp(sptr, "VL3", 3) == 0) {
		char buf2[BUF_SIZE];
		/* parse the volume number out */
		char *pos;
		/* read volume level in as a base 16 (hex) number */
		long level = strtol(sptr + 3, &pos, 16);
		if(*sptr == 'M') {
			/* main volume level */
			snprintf(buf, BUF_SIZE, "OK:volume:%ld\n", level);
			snprintf(buf2, BUF_SIZE, "OK:dbvolume:%ld\n", level - 82);
		} else if(*sptr == 'Z') {
			/* zone 2 volume level */
			snprintf(buf, BUF_SIZE, "OK:zone2volume:%ld\n", level);
			snprintf(buf2, BUF_SIZE, "OK:zone2dbvolume:%ld\n", level - 82);
		} else if(*sptr == 'V') {
			/* zone 3 volume level */
			snprintf(buf, BUF_SIZE, "OK:zone3volume:%ld\n", level);
			snprintf(buf2, BUF_SIZE, "OK:zone3dbvolume:%ld\n", level - 82);
		}
		/* this block is special compared to the rest; we write out buf2 here
		 * but let the normal write at the end handle buf as usual */
		write_to_connections(buf2);
	}

	/* TUN, TUZ, TU3 */
	else if(strncmp(sptr, "TU", 2) == 0) {
		/* parse the frequency number out */
		char *pos, *tunemsg;
		/* read frequency in as a base 10 number */
		long freq = strtol(sptr + 3, &pos, 10);
		/* decide whether we are main or zones */
		tunemsg = "OK:tune:";
		if(sptr[2] == 'Z') {
			tunemsg = "OK:zone2tune:";
		} else if(sptr[2] == '3') {
			tunemsg = "OK:zone3tune:";
		}
		if(freq > 8000) {
			/* FM frequency, something like 09790 was read */
			/* Use some awesome integer math to format the output */
			snprintf(buf, BUF_SIZE, "%s%ld.%ld FM\n", tunemsg,
					freq / 100, (freq / 10) % 10);
		} else {
			/* AM frequency, something like 00780 was read */
			snprintf(buf, BUF_SIZE, "%s%ld AM\n", tunemsg, freq);
		}
	}

	else if(strncmp(sptr, "PRS", 3) == 0 || strncmp(sptr, "PRZ", 3) == 0
			|| strncmp(sptr, "PR3", 3) == 0) {
		/* parse the preset number out */
		char *pos, *prsmsg;
		/* read value in as a base 16 (hex) number */
		long value = strtol(sptr + 3, &pos, 16);
		/* decide whether we are main or zones */
		prsmsg = "OK:preset:";
		if(sptr[2] == 'Z') {
			prsmsg = "OK:zone2preset:";
		} else if(sptr[2] == '3') {
			prsmsg = "OK:zone3preset:";
		}
		snprintf(buf, BUF_SIZE, "%s%ld\n", prsmsg, value);
	}

	else if(strncmp(sptr, "SLP", 3) == 0) {
		/* parse the minutes out */
		char *pos;
		/* read sleep timer in as a base 16 (hex) number */
		long mins = strtol(sptr + 3, &pos, 16);
		snprintf(buf, BUF_SIZE, "OK:sleep:%ld\n", mins);
	}

	else if(strncmp(sptr, "SWL", 3) == 0) {
		/* parse the level out */
		char *pos;
		/* read volume level in as a base 16 (hex) number */
		long level = strtol(sptr + 3, &pos, 16);
		snprintf(buf, BUF_SIZE, "OK:swlevel:%+ld\n", level);
	}

	else if(strncmp(sptr, "AVS", 3) == 0) {
		/* parse the time value out */
		char *pos;
		/* read time value in as a base 10 number */
		long level = strtol(sptr + 3, &pos, 10);
		/* AVS1000 -> 100 ms delay */
		level /= 10;
		snprintf(buf, BUF_SIZE, "OK:avsync:%ld\n", level);
	}

	else {
		snprintf(buf, BUF_SIZE, "OK:todo:%s\n", sptr);
	}

	write_to_connections(buf);
	return 0;
}

/**
 * Update the power status for the given zone to the given value. This
 * message may or may not be related to power; if it is not then the status
 * will not be updated. If it is, perform some bitmask-foo to update the power
 * status depending on what zone was turned on or off.
 * @param rcvr the receiver the message was received from
 * @param zone the value 1, 2, or 3 corresponding to the zone
 * @param value 1 for on, 0 for off
 */
static void update_power_status(struct receiver *rcvr, int zone, int value)
{
	/* var is a bitmask, manage power/zone2power separately */
	if(zone == 1 && value == 0) {
		rcvr->power &= ~MAIN_POWER;
	} else if(zone == 1 && value == 1) {
		rcvr->power |= MAIN_POWER;
	} else if(zone == 2 && value == 0) {
		rcvr->power &= ~ZONE2_POWER;
		timeval_clear(rcvr->zone2_sleep);
	} else if(zone == 2 && value == 1) {
		rcvr->power |= ZONE2_POWER;
	} else if(zone == 3 && value == 0) {
		rcvr->power &= ~ZONE3_POWER;
		timeval_clear(rcvr->zone3_sleep);
	} else if(zone == 3 && value == 1) {
		rcvr->power |= ZONE3_POWER;
	}
}

/**
 * Process a status message to be read from the receiver (one that the
 * receiver initiated). Return a human-readable status message.
 * @param rcvr the receiver to process the command for
 * @param logfd the fd used for logging raw status messages
 * @return 0 on successful processing, -1 on failure
 */
int process_incoming_message(struct receiver *rcvr, int logfd)
{
	int size, ret;
	char *status = NULL;

	/* get the output from the receiver */
	size = rcvr_handle_status(rcvr->fd, &status);
	if(size != -1) {
		/* log the message if we have a logfd */
		if(logfd > 0)
			xwrite(logfd, status, size + 1);
		/* parse the return and output a status message */
		ret = parse_status(rcvr, size, status);
		if(!ret)
			rcvr->msgs_received++;
	} else {
		write_to_connections(rcvr_err);
		ret = -1;
	}

	free(status);
	return ret;
}

/* vim: set ts=4 sw=4 noet: */
