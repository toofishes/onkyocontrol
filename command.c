/*
 *  command.c - Onkyo receiver user commands code
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

#define _XOPEN_SOURCE 600 /* strdup */

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h> /* toupper, isspace */
#include <errno.h>
#include <string.h>

#include "onkyo.h"

/* forward-declared because of the circular reference */
struct command;

typedef int (cmd_handler) (struct receiver *, const struct command *, char *);

/** A specific command and associated handler function */
struct command {
	unsigned long hash;
	const char *name;
	const char *prefix;
	cmd_handler *handler;
};

/** A text to value mapping of code values, such as for inputs or modes */
struct code_map {
	unsigned long hash;
	const char *key;
	const char *value;
};

/**
 * Convert a string, in place, to uppercase.
 * @param str string to convert (in place)
 */
static void strtoupper(char *str)
{
	char *ptr = str;
	while(*ptr) {
		*ptr = (char)toupper((unsigned char)*ptr);
		ptr++;
	}
}

/**
 * Queue a receiver command to be sent when the device file descriptor
 * is available for writing. Queueing and sending asynchronously allows
 * the program to backlog many commands at once without blocking on the
 * potentially slow receiver device. When queueing, we check if this command
 * is already in the queue- if so, we do not queue it again.
 * @param rcvr the receiver the command should be queued for
 * @param cmd the command struct for the first part of the receiver command
 * string, usually containing a prefix aka "PWR"
 * @param arg the second part of the receiver command string, aka "QSTN"
 * @return 0 on success, -1 on missing args
 */
static int cmd_attempt(struct receiver *rcvr,
		const struct command *cmd, const char *arg)
{
	struct cmdqueue *q;

	if(!cmd || !arg)
		return -1;
	if(strlen(cmd->prefix) + strlen(arg) >= BUF_SIZE)
		return -1;

	q = malloc(sizeof(struct cmdqueue));
	if(!q)
		return -1;

	sprintf(q->cmd, "%s%s", cmd->prefix, arg);
	q->hash = hash_sdbm(q->cmd);
	q->next = NULL;

	if(rcvr->queue == NULL) {
		rcvr->queue = q;
	} else {
		struct cmdqueue *ptr = rcvr->queue;
		for(;;) {
			if(ptr->hash == q->hash) {
				/* command already in our queue, skip second copy */
				free(q);
				return 0;
			}
			if(!ptr->next)
				break;
			ptr = ptr->next;
		}
		ptr->next = q;
	}
	return 0;
}

static int cmd_attempt_raw(struct receiver *rcvr,
		const char *prefix, const char *arg)
{
	struct command c;
	c.prefix = prefix;
	return cmd_attempt(rcvr, &c, arg);
}

/**
 * Handle the standard question, up, and down operations if possible.
 * @param rcvr the receiver the command should be queued for
 * @param cmd the struct containing information on the command being called
 * @param arg the provided argument, e.g. "QSTN"
 * @return the return value of cmd_attempt() if we found a standard operation,
 * else -2
 */
static int handle_standard(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(rcvr, cmd, "QSTN");
	else if(strcmp(arg, "up") == 0)
		return cmd_attempt(rcvr, cmd, "UP");
	else if(strcmp(arg, "down") == 0)
		return cmd_attempt(rcvr, cmd, "DOWN");
	return -2;
}

static int handle_boolean(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(rcvr, cmd, "QSTN");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(rcvr, cmd, "01");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(rcvr, cmd, "00");
	else if(strcmp(arg, "toggle") == 0) {
		const char *prefix = cmd->prefix;
		/* toggle is applicable for mute, not for power */
		if(strcmp(prefix, "AMT") == 0 || strcmp(prefix, "ZMT") == 0
				|| strcmp(prefix, "MT3") == 0)
			return cmd_attempt(rcvr, cmd, "TG");
	}

	/* unrecognized command */
	return -1;
}

static int handle_ranged(struct receiver *rcvr,
		const struct command *cmd, char *arg,
		int lower, int upper, int offset, const char *fmt)
{
	int ret;
	long level;
	char *test;
	char cmdstr[3]; /* "XX\0" */

	ret = handle_standard(rcvr, cmd, arg);
	if(ret != -2)
		return ret;

	/* otherwise we probably have a number */
	level = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return -1;
	}
	if(level < lower || level > upper) {
		/* range error */
		return -1;
	}
	level += offset;
	/* create our command */
	sprintf(cmdstr, fmt, (unsigned long)level);
	/* send the command */
	return cmd_attempt(rcvr, cmd, cmdstr);
}

static int handle_volume(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	return handle_ranged(rcvr, cmd, arg, 0, 100, 0, "%02lX");
}

static int handle_dbvolume(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	return handle_ranged(rcvr, cmd, arg, -82, 18, 82, "%02lX");
}

static int handle_preset(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	return handle_ranged(rcvr, cmd, arg, 0, 40, 0, "%02lX");
}

static int handle_avsync(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	/* the extra '0' is an easy way to not have to multiply by 10 */
	return handle_ranged(rcvr, cmd, arg, 0, 250, 0, "%03ld0");
}

static int handle_swlevel(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	int ret;
	long level;
	char *test;
	char cmdstr[3]; /* "XX\0" */

	ret = handle_standard(rcvr, cmd, arg);
	if(ret != -2)
		return ret;

	/* otherwise we probably have a number */
	level = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return -1;
	}
	if(level < -15 || level > 12) {
		/* range error */
		return -1;
	}
	/* create our command */
	if(level == 0) {
		sprintf(cmdstr, "00");
	} else if(level > 0) {
		sprintf(cmdstr, "+%1lX", (unsigned long)level);
	} else { /* level < 0 */
		sprintf(cmdstr, "-%1lX", (unsigned long)-level);
	}
	/* send the command */
	return cmd_attempt(rcvr, cmd, cmdstr);
}

static struct code_map inputs[] = {
	{ 0, "DVR",       "00" },
	{ 0, "VCR",       "00" },
	{ 0, "CABLE",     "01" },
	{ 0, "SAT",       "01" },
	{ 0, "TV",        "02" },
	{ 0, "AUX",       "03" },
	{ 0, "AUX2",      "04" },
	{ 0, "PC",        "05" },
	{ 0, "DVD",       "10" },
	{ 0, "TAPE",      "20" },
	{ 0, "PHONO",     "22" },
	{ 0, "CD",        "23" },
	{ 0, "FM",        "24" },
	{ 0, "FM TUNER",  "24" },
	{ 0, "AM",        "25" },
	{ 0, "AM TUNER",  "25" },
	{ 0, "TUNER",     "26" },
	{ 0, "MUSIC SERVER", "27" },
	{ 0, "SERVER",    "27" },
	{ 0, "IRADIO",    "28" },
	{ 0, "USB",       "29" },
	{ 0, "USB REAR",  "2A" },
	{ 0, "PORT",      "40" },
	{ 0, "MULTICH",   "30" },
	{ 0, "XM",        "31" },
	{ 0, "SIRIUS",    "32" },
	{ 0, NULL,        NULL },
};

static int handle_input(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	int ret;
	unsigned long hashval;
	struct code_map *input;

	ret = handle_standard(rcvr, cmd, arg);
	if(ret != -2)
		return ret;

	/* allow lower or upper names */
	strtoupper(arg);
	ret = -1;

	hashval = hash_sdbm(arg);
	for(input = inputs; input->key; input++) {
		if(input->hash == hashval) {
			ret = cmd_attempt(rcvr, cmd, input->value);
			break;
		}
	}
	/* the following are only valid for zones */
	if(ret == -1 &&
			(strcmp(cmd->prefix, "SLZ") == 0 ||
			 strcmp(cmd->prefix, "SL3") == 0)) {
		if(strcmp(arg, "OFF") == 0)
			ret = cmd_attempt(rcvr, cmd, "7F");
		else if(strcmp(arg, "SOURCE") == 0)
			ret = cmd_attempt(rcvr, cmd, "80");
	}

	return ret;
}

static struct code_map modes[] = {
	{ 0, "STEREO",     "00" },
	{ 0, "DIRECT",     "01" },
	{ 0, "MONOMOVIE",  "07" },
	{ 0, "ORCHESTRA",  "08" },
	{ 0, "UNPLUGGED",  "09" },
	{ 0, "STUDIOMIX",  "0A" },
	{ 0, "TVLOGIC",    "0B" },
	{ 0, "ACSTEREO",   "0C" },
	{ 0, "THEATERD",   "0D" },
	{ 0, "MONO",       "0F" },
	{ 0, "PURE",       "11" },
	{ 0, "FULLMONO",   "13" },
	{ 0, "DTSSS",      "15" },
	{ 0, "DSX",        "16" },
	{ 0, "STRAIGHT",   "40" },
	{ 0, "DOLBYEX",    "41" },
	{ 0, "DTSES",      "41" },
	{ 0, "THX",        "42" },
	{ 0, "THXEX",      "43" },
	{ 0, "THXMUSIC",   "44" },
	{ 0, "THXGAMES",   "45" },
	{ 0, "PLIIMOVIE",  "80" },
	{ 0, "PLIIMUSIC",  "81" },
	{ 0, "NEO6CINEMA", "82" },
	{ 0, "NEO6MUSIC",  "83" },
	{ 0, "PLIITHX",    "84" },
	{ 0, "NEO6THX",    "85" },
	{ 0, "PLIIGAME",   "86" },
	{ 0, "NEURALTHX",  "88" },
	{ 0, NULL,         NULL },
};

static int handle_mode(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	int ret;
	unsigned long hashval;
	struct code_map *mode;

	ret = handle_standard(rcvr, cmd, arg);
	if(ret != -2)
		return ret;

	/* allow lower or upper names */
	strtoupper(arg);
	ret = -1;

	hashval = hash_sdbm(arg);
	for(mode = modes; mode->key; mode++) {
		if(mode->hash == hashval) {
			ret = cmd_attempt(rcvr, cmd, mode->value);
			break;
		}
	}

	return ret;
}

static int handle_tune(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	int ret;
	char cmdstr[6]; /* "00000\0" */
	char *test;

	ret = handle_standard(rcvr, cmd, arg);
	if(ret != -2)
		return ret;

	/* Otherwise we should have a frequency. It can be one of two formats:
	 * FM: (1)00.0 (possible hundreds spot, with decimal point)
	 * AM: (1)000 (possible thousands spot, with NO decimal point)
	 */
	if(strchr(arg, '.')) {
		long freq, frac_freq;
		/* attempt to parse as FM */
		errno = 0;
		freq = strtol(arg, &test, 10);
		/* this should start parsing after the '.' */
		frac_freq = strtol(test + 1, &test, 10);
		if(errno != 0) {
			/* parse error, not a number */
			return -1;
		}
		/* allowed range: 87.5 to 107.9 inclusive */
		if(frac_freq < 0 || frac_freq > 9 ||
				(freq <= 87 && frac_freq < 5) ||
				freq >= 108) {
			/* range error */
			return -1;
		}
		/* we want to print something like "TUN09790" */
		freq = freq * 100 + frac_freq * 10;
		sprintf(cmdstr, "%05ld", freq);
	} else {
		long freq;
		/* should be AM, single number with no decimal */
		errno = 0;
		freq = strtol(arg, &test, 10);
		if(errno != 0) {
			/* parse error, not a number */
			return -1;
		}
		if(freq < 530 || freq > 1710) {
			/* range error */
			return -1;
		}
		/* we want to print something like "TUN00780" */
		sprintf(cmdstr, "%05ld", freq);
	}
	return cmd_attempt(rcvr, cmd, cmdstr);
}

static int handle_sleep(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	long mins;
	char *test;
	char cmdstr[3]; /* "XX\0" */

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(rcvr, cmd, "QSTN");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(rcvr, cmd, "OFF");

	/* otherwise we probably have a number */
	mins = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return -1;
	}
	if(mins < 0 || mins > 90) {
		/* range error */
		return -1;
	}
	/* create our command */
	sprintf(cmdstr, "%02lX", (unsigned long)mins);
	/* send the command */
	return cmd_attempt(rcvr, cmd, cmdstr);
}

int write_fakesleep_status(struct receiver *rcvr,
		struct timeval now, char zone)
{
	long mins;
	time_t when;
	char msg[BUF_SIZE];

	if(zone == '2')
		when = rcvr->zone2_sleep.tv_sec;
	else if(zone == '3')
		when = rcvr->zone3_sleep.tv_sec;
	else
		return - 1;

	mins = when > now.tv_sec ? (when - now.tv_sec + 59) / 60 : 0;
	snprintf(msg, sizeof(msg), "OK:zone%csleep:%ld\n", zone, mins);
	write_to_connections(msg);
	return 0;
}

static int handle_fakesleep(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	struct timeval now;
	char *test;
	char zone;

	gettimeofday(&now, NULL);
	zone = cmd->prefix[0];

	if(zone != '2' && zone != '3')
		return -1;

	if(!arg || strcmp(arg, "status") == 0) {
		/* do nothing with the arg, we'll end up writing a message */
	} else if(strcmp(arg, "off") == 0) {
		/* clear out any future receiver set sleep time */
		if(zone == '2') {
			timeval_clear(rcvr->zone2_sleep);
		} else if(zone == '3') {
			timeval_clear(rcvr->zone3_sleep);
		}
	} else {
		/* otherwise we probably have a number */
		long mins = strtol(arg, &test, 10);
		if(*test != '\0') {
			/* parse error, not a number */
			return -1;
		}
		if(mins < 0) {
			/* range error */
			return -1;
		}
		if(zone == '2') {
			rcvr->zone2_sleep = now;
			rcvr->zone2_sleep.tv_sec += 60 * mins;
		} else if(zone == '3') {
			rcvr->zone3_sleep = now;
			rcvr->zone3_sleep.tv_sec += 60 * mins;
		}
	}

	write_fakesleep_status(rcvr, now, zone);
	return 0;
}

static int handle_memory(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	if(!arg)
		return -1;
	if(strcmp(arg, "lock") == 0)
		return cmd_attempt(rcvr, cmd, "LOCK");
	else if(strcmp(arg, "unlock") == 0)
		return cmd_attempt(rcvr, cmd, "UNLK");
	return -1;
}


static int handle_status(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	int ret = 0;

	/* this handler is a bit different in that we call
	 * multiple receiver commands */
	if(strcmp(cmd->name, "status") == 0 && (!arg || strcmp(arg, "main") == 0)) {
		ret += cmd_attempt_raw(rcvr, "PWR", "QSTN");
		ret += cmd_attempt_raw(rcvr, "MVL", "QSTN");
		ret += cmd_attempt_raw(rcvr, "AMT", "QSTN");
		ret += cmd_attempt_raw(rcvr, "SLI", "QSTN");
		ret += cmd_attempt_raw(rcvr, "LMD", "QSTN");
		ret += cmd_attempt_raw(rcvr, "TUN", "QSTN");
	} else if(strcmp(cmd->name, "zone2status") == 0 || (arg && strcmp(arg, "zone2") == 0)) {
		ret += cmd_attempt_raw(rcvr, "ZPW", "QSTN");
		ret += cmd_attempt_raw(rcvr, "ZVL", "QSTN");
		ret += cmd_attempt_raw(rcvr, "ZMT", "QSTN");
		ret += cmd_attempt_raw(rcvr, "SLZ", "QSTN");
		ret += cmd_attempt_raw(rcvr, "TUZ", "QSTN");
	} else if(strcmp(cmd->name, "zone3status") == 0 || (arg && strcmp(arg, "zone3") == 0)) {
		ret += cmd_attempt_raw(rcvr, "PW3", "QSTN");
		ret += cmd_attempt_raw(rcvr, "VL3", "QSTN");
		ret += cmd_attempt_raw(rcvr, "MT3", "QSTN");
		ret += cmd_attempt_raw(rcvr, "SL3", "QSTN");
		ret += cmd_attempt_raw(rcvr, "TU3", "QSTN");
	} else {
		return -1;
	}

	return ret < 0 ? -2 : 0;
}

static int handle_raw(struct receiver *rcvr,
		const struct command *cmd, char *arg)
{
	return cmd_attempt(rcvr, cmd, arg);
}

static int handle_quit(UNUSED struct receiver *rcvr,
		UNUSED const struct command *cmd, UNUSED char *arg)
{
	return -2;
}

static struct command command_list[] = {
	/*
	{ 0, name,      prefix, handle_func }, */
	{ 0, "power",    "PWR", handle_boolean },
	{ 0, "volume",   "MVL", handle_volume },
	{ 0, "dbvolume", "MVL", handle_dbvolume },
	{ 0, "mute",     "AMT", handle_boolean },
	{ 0, "input",    "SLI", handle_input },
	{ 0, "mode",     "LMD", handle_mode },
	{ 0, "tune",     "TUN", handle_tune },
	{ 0, "preset",   "PRS", handle_preset },
	{ 0, "swlevel",  "SWL", handle_swlevel },
	{ 0, "avsync",   "AVS", handle_avsync },
	{ 0, "memory",   "MEM", handle_memory },
	{ 0, "audyssey", "ADY", handle_boolean },
	{ 0, "dyneq",    "ADQ", handle_boolean },

	{ 0, "status",   NULL,  handle_status },

	{ 0, "zone2power",  "ZPW", handle_boolean },
	{ 0, "zone2volume", "ZVL", handle_volume },
	{ 0, "zone2dbvolume","ZVL",handle_dbvolume },
	{ 0, "zone2mute",   "ZMT", handle_boolean },
	{ 0, "zone2input",  "SLZ", handle_input },
	{ 0, "zone2tune",   "TUZ", handle_tune },
	{ 0, "zone2preset", "PRZ", handle_preset },

	{ 0, "zone2status", NULL,  handle_status },

	{ 0, "zone3power",  "PW3", handle_boolean },
	{ 0, "zone3volume", "VL3", handle_volume },
	{ 0, "zone3dbvolume","VL3",handle_dbvolume },
	{ 0, "zone3mute",   "MT3", handle_boolean },
	{ 0, "zone3input",  "SL3", handle_input },
	{ 0, "zone3tune",   "TU3", handle_tune },
	{ 0, "zone3preset", "PR3", handle_preset },

	{ 0, "zone3status", NULL,  handle_status },

	{ 0, "sleep",       "SLP", handle_sleep },
	{ 0, "zone2sleep",  "2",   handle_fakesleep },
	{ 0, "zone3sleep",  "3",   handle_fakesleep },

	{ 0, "raw",  "", handle_raw },
	{ 0, "quit", "", handle_quit },

	{ 0, NULL, NULL, NULL },
};

/**
 * Initialize our list of commands. This must be called before the first
 * call to process_command().
 */
void init_commands(void)
{
	unsigned int cmd_count = 0;
	struct command *ptr;
	struct code_map *code;

	for(ptr = command_list; ptr->name; ptr++) {
		ptr->hash = hash_sdbm(ptr->name);
		cmd_count++;
	}

	for(code = inputs; code->key; code++) {
		code->hash = hash_sdbm(code->key);
	}
	for(code = modes; code->key; code++) {
		code->hash = hash_sdbm(code->key);
	}

	printf("%u commands added to command list.\n", cmd_count);
}

/** 
 * Process an incoming command, parsing it into the standard "<cmd> <arg>"
 * format. Attempt to locate a handler for the given command and delegate
 * the work to it. If no handler is found, return an error; otherwise
 * return the relevant human-readable status message.
 * @param rcvr the receiver to process the command for
 * @param str the full command string, e.g. "power on"
 * @return 0 if the command string was correct and sent, -1 on invalid command,
 * -2 if we should quit/close the connection
 * string
 */
int process_command(struct receiver *rcvr, const char *str)
{
	unsigned long hashval;
	char *cmdstr, *argstr;
	char *c;
	struct command *cmd;

	if(!str)
		return -1;

	cmdstr = strdup(str);
	/* start by killing trailing whitespace of any sort */
	c = cmdstr + strlen(cmdstr) - 1;
	while(isspace(*c) && c >= cmdstr)
		*c-- = '\0';
	/* start by splitting the string after the cmd */
	argstr = strchr(cmdstr, ' ');
	/* if we had an arg, set our pointers correctly */
	if(argstr) {
		*argstr = '\0';
		argstr++;
	}

	hashval = hash_sdbm(cmdstr);
	for(cmd = command_list; cmd->name; cmd++) {
		if(cmd->hash == hashval) {
			/* we found the handler, call it and return the result */
			int ret = cmd->handler(rcvr, cmd, argstr);
			free(cmdstr);
			return ret;
		}
	}

	/* we didn't find a handler, must be an invalid command */
	free(cmdstr);
	return -1;
}


/**
 * Determine if a command is related to the receiver power status. This can
 * be either a query or an explicit command to turn the power on or off.
 * @param cmd the command to process (e.g. "!1PWRQSTN\n")
 * @return whether the command is related to receiver power
 */
int is_power_command(const char *cmd) {
	if(strstr(cmd, "PWR") != NULL || strstr(cmd, "ZPW") != NULL
			|| strstr(cmd, "PW3") != NULL)
		return 1;
	return 0;
}

/* vim: set ts=4 sw=4 noet: */
