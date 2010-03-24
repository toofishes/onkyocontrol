/*
 *  command.c - Onkyo receiver user commands code
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

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h> /* toupper, isspace */
#include <errno.h>
#include <string.h>

#include "onkyo.h"

static struct command *command_list = NULL;

typedef int (cmd_handler) (const struct command *, const char *);

struct command {
	unsigned long hash;
	const char *name;
	const char *prefix;
	cmd_handler *handler;
	int fake;
	struct command *next;
};

/**
 * Convert a string to uppercase.
 * @param str string to convert (in place)
 * @return pointer to the string
 */
static char *strtoupper(char *str)
{
	char *ptr = str;
	while(*ptr) {
		*ptr = (char)toupper(*ptr);
		ptr++;
	}
	return(str);
}

/**
 * Attempt to write a receiver command out to the control channel.
 * This will take a in a simple receiver string minus any preamble or
 * ending characters, turn it into a valid command, and queue it to the
 * sent to the receiver.
 * @param cmd the command struct for the first part of the receiver command
 * string, usually containing a prefix aka "PWR"
 * @param arg the second part of the receiver command string, aka "QSTN"
 * @return 0 on success, -1 on missing args
 */
static int cmd_attempt(const struct command *cmd, const char *arg)
{
	char *fullcmd;

	if(!cmd || !arg)
		return(-1);

	if(!cmd->fake && !cmd->prefix)
		return(-1);

	fullcmd = malloc(strlen(START_SEND) + strlen(cmd->prefix) + strlen(arg)
			+ strlen(END_SEND) + 1);
	sprintf(fullcmd, START_SEND "%s%s" END_SEND, cmd->prefix, arg);

	/* send the command to the receiver */
	queue_rcvr_command(fullcmd);
	return(0);
}

static int cmd_attempt_raw(const char *fake, const char *arg)
{
	struct command c;
	c.prefix = fake;
	return cmd_attempt(&c, arg);
}

/**
 * Handle the standard question, up, and down operations if possible.
 * @param cmd the struct containing information on the command being called
 * @param arg the provided argument, e.g. "QSTN"
 * @return the return value of cmd_attempt() if we found a standard operation,
 * else -2
 */
static int handle_standard(const struct command *cmd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(cmd, "QSTN");
	else if(strcmp(arg, "up") == 0)
		return cmd_attempt(cmd, "UP");
	else if(strcmp(arg, "down") == 0)
		return cmd_attempt(cmd, "DOWN");
	return(-2);
}

static int handle_boolean(const struct command *cmd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(cmd, "QSTN");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(cmd, "01");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(cmd, "00");
	else if(strcmp(arg, "toggle") == 0) {
		const char *prefix = cmd->prefix;
		/* toggle is applicable for mute, not for power */
		if(strcmp(prefix, "AMT") == 0 || strcmp(prefix, "ZMT") == 0
				|| strcmp(prefix, "MT3") == 0)
			return cmd_attempt(cmd, "TG");
	}

	/* unrecognized command */
	return(-1);
}

static int handle_ranged(const struct command *cmd, const char *arg,
		int lower, int upper, const char *fmt)
{
	int ret;
	long level;
	char *test;
	char cmdstr[3]; /* "XX\0" */

	ret = handle_standard(cmd, arg);
	if(ret != -2)
		return (ret);

	/* otherwise we probably have a number */
	level = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return(-1);
	}
	if(level < lower || level > upper) {
		/* range error */
		return(-1);
	}
	/* create our command */
	sprintf(cmdstr, fmt, (unsigned long)level);
	/* send the command */
	return cmd_attempt(cmd, cmdstr);
}

static int handle_volume(const struct command *cmd, const char *arg)
{
	return handle_ranged(cmd, arg, 0, 100, "%02lX");
}

static int handle_preset(const struct command *cmd, const char *arg)
{
	return handle_ranged(cmd, arg, 0, 40, "%02lX");
}

static int handle_avsync(const struct command *cmd, const char *arg)
{
	/* the extra '0' is an easy way to not have to multiply by 10 */
	return handle_ranged(cmd, arg, 0, 250, "%03ld0");
}

static int handle_swlevel(const struct command *cmd, const char *arg)
{
	int ret;
	long level;
	char *test;
	char cmdstr[3]; /* "XX\0" */

	ret = handle_standard(cmd, arg);
	if(ret != -2)
		return (ret);

	/* otherwise we probably have a number */
	level = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return(-1);
	}
	if(level < -15 || level > 12) {
		/* range error */
		return(-1);
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
	return cmd_attempt(cmd, cmdstr);
}

static const char * const inputs[][2] = {
	{ "DVR",       "00" },
	{ "VCR",       "00" },
	{ "CABLE",     "01" },
	{ "SAT",       "01" },
	{ "TV",        "02" },
	{ "AUX",       "03" },
	{ "AUX2",      "04" },
	{ "DVD",       "10" },
	{ "TAPE",      "20" },
	{ "PHONO",     "22" },
	{ "CD",        "23" },
	{ "FM",        "24" },
	{ "FM TUNER",  "24" },
	{ "AM",        "25" },
	{ "AM TUNER",  "25" },
	{ "TUNER",     "26" },
	{ "MULTICH",   "30" },
	{ "XM",        "31" },
	{ "SIRIUS",    "32" },
};

static int handle_input(const struct command *cmd, const char *arg)
{
	unsigned int i, loopsize;
	int ret;
	char *dup;

	ret = handle_standard(cmd, arg);
	if(ret != -2)
		return (ret);

	/* allow lower or upper names */
	dup = strtoupper(strdup(arg));
	ret = -1;

	/* compile-time constant */
	loopsize = sizeof(inputs) / sizeof(*inputs);
	for(i = 0; i < loopsize; i++) {
		if(strcmp(dup, inputs[i][0]) == 0) {
			ret = cmd_attempt(cmd, inputs[i][1]);
			break;
		}
	}
	/* the following are only valid for zones */
	if(ret == -1 &&
			(strcmp(cmd->prefix, "SLZ") == 0 ||
			 strcmp(cmd->prefix, "SL3") == 0)) {
		if(strcmp(dup, "OFF") == 0)
			ret = cmd_attempt(cmd, "7F");
		else if(strcmp(dup, "SOURCE") == 0)
			ret = cmd_attempt(cmd, "80");
	}

	free(dup);
	return(ret);
}

static const char * const modes[][2] = {
	{ "STEREO",     "00" },
	{ "DIRECT",     "01" },
	{ "MONOMOVIE",  "07" },
	{ "ORCHESTRA",  "08" },
	{ "UNPLUGGED",  "09" },
	{ "STUDIOMIX",  "0A" },
	{ "TVLOGIC",    "0B" },
	{ "ACSTEREO",   "0C" },
	{ "MONO",       "0F" },
	{ "PURE",       "11" },
	{ "FULLMONO",   "13" },
	{ "STRAIGHT",   "40" },
	{ "THX",        "42" },
	{ "PLIIMOVIE",  "80" },
	{ "PLIIMUSIC",  "81" },
	{ "NEO6CINEMA", "82" },
	{ "NEO6MUSIC",  "83" },
	{ "PLIITHX",    "84" },
	{ "NEO6THX",    "85" },
	{ "PLIIGAME",   "85" },
	{ "NEURALTHX",  "88" },
};

static int handle_mode(const struct command *cmd, const char *arg)
{
	unsigned int i, loopsize;
	int ret;
	char *dup;

	ret = handle_standard(cmd, arg);
	if(ret != -2)
		return (ret);

	/* allow lower or upper names */
	dup = strtoupper(strdup(arg));
	ret = -1;

	/* compile-time constant */
	loopsize = sizeof(modes) / sizeof(*modes);
	for(i = 0; i < loopsize; i++) {
		if(strcmp(dup, modes[i][0]) == 0) {
			ret = cmd_attempt(cmd, modes[i][1]);
			break;
		}
	}

	free(dup);
	return(ret);
}

static int handle_tune(const struct command *cmd, const char *arg)
{
	int ret;
	char cmdstr[6]; /* "00000\0" */
	char *test;

	ret = handle_standard(cmd, arg);
	if(ret != -2)
		return (ret);

	/* Otherwise we should have a frequency. It can be one of two formats:
	 * FM: (1)00.0 (possible hundreds spot, with decimal point)
	 * AM: (1)000 (possible thousands spot, with NO decimal point)
	 */
	if(strchr(arg, '.')) {
		double freq;
		/* attempt to parse as FM */
		errno = 0;
		freq = strtod(arg, &test);
		if(errno != 0) {
			/* parse error, not a number */
			return(-1);
		}
		if(freq < 87.4 || freq > 108.0) {
			/* range error */
			return(-1);
		}
		/* we want to print something like "TUN09790" */
		sprintf(cmdstr, "%05.0f", freq * 100.0);
	} else {
		long freq;
		/* should be AM, single number with no decimal */
		errno = 0;
		freq = strtol(arg, &test, 10);
		if(errno != 0) {
			/* parse error, not a number */
			return(-1);
		}
		if(freq < 530 || freq > 1710) {
			/* range error */
			return(-1);
		}
		/* we want to print something like "TUN00780" */
		sprintf(cmdstr, "%05ld", freq);
	}
	return cmd_attempt(cmd, cmdstr);
}

static int handle_sleep(const struct command *cmd, const char *arg)
{
	long mins;
	char *test;
	char cmdstr[3]; /* "XX\0" */

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(cmd, "QSTN");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(cmd, "OFF");

	/* otherwise we probably have a number */
	mins = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return(-1);
	}
	if(mins < 0 || mins > 90) {
		/* range error */
		return(-1);
	}
	/* create our command */
	sprintf(cmdstr, "%02lX", (unsigned long)mins);
	/* send the command */
	return cmd_attempt(cmd, cmdstr);
}

static int handle_memory(const struct command *cmd, const char *arg)
{
	if(!arg)
		return(-1);
	if(strcmp(arg, "lock") == 0)
		return cmd_attempt(cmd, "LOCK");
	else if(strcmp(arg, "unlock") == 0)
		return cmd_attempt(cmd, "UNLK");
	return(-1);
}

static int handle_status(UNUSED const struct command *cmd, const char *arg)
{
	int ret = 0;

	/* this handler is a bit different in that we call
	 * multiple receiver commands */
	if(!arg || strcmp(arg, "main") == 0) {
		ret += cmd_attempt_raw("PWR", "QSTN");
		ret += cmd_attempt_raw("MVL", "QSTN");
		ret += cmd_attempt_raw("AMT", "QSTN");
		ret += cmd_attempt_raw("SLI", "QSTN");
		ret += cmd_attempt_raw("LMD", "QSTN");
		ret += cmd_attempt_raw("TUN", "QSTN");
	} else if(strcmp(arg, "zone2") == 0) {
		ret += cmd_attempt_raw("ZPW", "QSTN");
		ret += cmd_attempt_raw("ZVL", "QSTN");
		ret += cmd_attempt_raw("ZMT", "QSTN");
		ret += cmd_attempt_raw("SLZ", "QSTN");
		ret += cmd_attempt_raw("TUZ", "QSTN");
	} else if(strcmp(arg, "zone3") == 0) {
		ret += cmd_attempt_raw("PW3", "QSTN");
		ret += cmd_attempt_raw("VL3", "QSTN");
		ret += cmd_attempt_raw("MT3", "QSTN");
		ret += cmd_attempt_raw("SL3", "QSTN");
		ret += cmd_attempt_raw("TU3", "QSTN");
	} else {
		return(-1);
	}

	return(ret < 0 ? -2 : 0);
}

static int handle_raw(const struct command *cmd, const char *arg)
{
	return cmd_attempt(cmd, arg);
}

/**
 * Add the command with the given name and handler to our command list. This
 * will allow process_command() to locate the correct handler for a command.
 * @param name the name of the command, e.g. "volume"
 * @param prefix the first part of the receiver command string, aka "PWR"
 * @param handler the function that will handle the command
 */
static void add_command(const char *name, const char *prefix,
		cmd_handler *handler, int fake)
{
	/* create our new command object */
	struct command *cmd = malloc(sizeof(struct command));
	cmd->hash = hash_sdbm(name);
	cmd->name = name;
	cmd->prefix = prefix;
	cmd->handler = handler;
	cmd->fake = fake;
	cmd->next = NULL;

	/* add it to our list, first item is special case */
	if(!command_list) {
		command_list = cmd;
	} else {
		struct command *ptr = command_list;
		while(ptr->next)
			ptr = ptr->next;
		ptr->next = cmd;
	}
}

/**
 * Initialize our list of commands. This must be called before the first
 * call to process_command().
 */
void init_commands(void)
{
	struct command *ptr;
	unsigned int cmd_count = 0;
	/*
	add_command(name,       prefix, handle_func,real/fake); */
	add_command("power",    "PWR", handle_boolean, 0);
	add_command("volume",   "MVL", handle_volume,  0);
	add_command("mute",     "AMT", handle_boolean, 0);
	add_command("input",    "SLI", handle_input,   0);
	add_command("mode",     "LMD", handle_mode,    0);
	add_command("tune",     "TUN", handle_tune,    0);
	add_command("preset",   "PRS", handle_preset,  0);
	add_command("swlevel",  "SWL", handle_swlevel, 0);
	add_command("avsync",   "AVS", handle_avsync,  0);
	add_command("memory",   "MEM", handle_memory,  0);

	add_command("zone2power",  "ZPW", handle_boolean, 0);
	add_command("zone2volume", "ZVL", handle_volume,  0);
	add_command("zone2mute",   "ZMT", handle_boolean, 0);
	add_command("zone2input",  "SLZ", handle_input,   0);
	add_command("zone2tune",   "TUZ", handle_tune,    0);
	add_command("zone2preset", "PRZ", handle_preset,  0);

	add_command("zone3power",  "PW3", handle_boolean, 0);
	add_command("zone3volume", "VL3", handle_volume,  0);
	add_command("zone3mute",   "MT3", handle_boolean, 0);
	add_command("zone3input",  "SL3", handle_input,   0);
	add_command("zone3tune",   "TU3", handle_tune,    0);
	add_command("zone3preset", "PR3", handle_preset,  0);

	add_command("sleep",       "SLP", handle_sleep,   0);
	add_command("zone2sleep",  "ZSP", handle_sleep,   1);
	add_command("zone3sleep",  "SP3", handle_sleep,   1);

	add_command("status",   NULL,  handle_status,  0);
	add_command("raw",      "",    handle_raw,     0);

	ptr = command_list;
	while(ptr) {
		cmd_count++;
		ptr = ptr->next;
	}
	printf("%u commands added to command list.\n", cmd_count);
}

/**
 * Free our list of commands.
 */
void free_commands(void)
{
	struct command *cmd = command_list;
	command_list = NULL;
	while(cmd) {
		struct command *cmdnext = cmd->next;
		free(cmd);
		cmd = cmdnext;
	}
}

/** 
 * Process an incoming command, parsing it into the standard "<cmd> <arg>"
 * format. Attempt to locate a handler for the given command and delegate
 * the work to it. If no handler is found, return an error; otherwise
 * return the relevant human-readable status message.
 * @param str the full command string, e.g. "power on"
 * @return 0 if the command string was correct and sent, -1 on invalid command
 * string
 */
int process_command(const char *str)
{
	unsigned long hashval;
	char *cmdstr, *argstr;
	char *c;
	struct command *cmd;

	if(!str)
		return(-1);

	cmdstr = strdup(str);
	/* start by killing trailing whitespace of any sort */
	c = cmdstr + strlen(cmdstr) - 1;
	while(isspace(*c)) {
		*c-- = '\0';
	}
	/* start by splitting the string after the cmd */
	argstr = strchr(cmdstr, ' ');
	/* if we had an arg, set our pointers correctly */
	if(argstr) {
		*argstr = '\0';
		argstr++;
	}

	hashval = hash_sdbm(cmdstr);
	cmd = command_list;
	while(cmd) {
		if(cmd->hash == hashval) {
			/* we found the handler, call it and return the result */
			int ret = cmd->handler(cmd, argstr);
			free(cmdstr);
			return(ret);
		}
		cmd = cmd->next;
	}

	/* we didn't find a handler, must be an invalid command */
	free(cmdstr);
	return(-1);
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
		return(1);
	return(0);
}

/* vim: set ts=4 sw=4 noet: */
