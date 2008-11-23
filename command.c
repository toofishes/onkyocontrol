/*
 *  command.c - Onkyo receiver user commands code
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
#include <ctype.h> /* toupper */
#include <errno.h>
#include <string.h>

#include "onkyo.h"

/*
 * commands:
 * power on
 * power off
 *
 * volume up
 * volume down
 * volume 55 (0 to 100 range)
 *
 * mute on
 * mute off
 * mute toggle
 *
 * input DVD
 * input cd
 * input tv
 * input tuner
 *
 * z2power on
 * z2power off
 * z2mute on
 * z2mute off
 * z2mute toggle
 * z2volume up
 * z2volume down
 * z2volume 56 (0 to 100 range)
 *
 * status (on demand or async):
 * power status (return on/off)
 *   OK:power:on
 * volume status (return 0-100)
 *   OK:volume:44
 * mute status (return on/off)
 *   OK:mute:off
 * input status (return string name)
 *   OK:input:DVD
 * status:
 *   OK:power:on
 *   OK:volume:44
 *   OK:mute:off
 *   OK:input:DVD
 *   ...
 */

static struct command *command_list = NULL;

typedef int (cmd_handler) (const char *, const char *);

struct command {
	char *name;
	char *prefix;
	cmd_handler *handler;
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
 * @param cmd1 the first part of the receiver command string, aka "PWR"
 * @param cmd2 the second part of the receiver command string, aka "QSTN"
 * @return 0 on success, -1 on missing args
 */
static int cmd_attempt(const char *cmd1, const char *cmd2)
{
	char *fullcmd;

	if(!cmd1 || !cmd2)
		return(-1);

	fullcmd = malloc(strlen(START_SEND) + strlen(cmd1) + strlen(cmd2)
			+ strlen(END_SEND) + 1);
	sprintf(fullcmd, START_SEND "%s%s" END_SEND, cmd1, cmd2);

	/* send the command to the receiver */
	queue_rcvr_command(fullcmd);
	return(0);
}


static int handle_boolean(const char *prefix, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(prefix, "QSTN");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(prefix, "01");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(prefix, "00");
	else if(strcmp(arg, "toggle") == 0) {
		/* toggle is applicable for mute, not for power */
		if(strcmp(prefix, "AMT") == 0 || strcmp(prefix, "ZMT") == 0)
			return cmd_attempt(prefix, "TG");
	}

	/* unrecognized command */
	return(-1);
}

static int handle_volume(const char *prefix, const char *arg)
{
	long int level;
	char *test;
	char cmdstr[3]; /* "XX\0" */

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(prefix, "QSTN");
	else if(strcmp(arg, "up") == 0)
		return cmd_attempt(prefix, "UP");
	else if(strcmp(arg, "down") == 0)
		return cmd_attempt(prefix, "DOWN");

	/* otherwise we probably have a number */
	level = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return(-1);
	}
	if(level < 0 || level > 100) {
		/* range error */
		return(-1);
	}
	/* create our command */
	sprintf(cmdstr, "%lX", level);
	/* send the command */
	return cmd_attempt(prefix, cmdstr);
}

static int handle_input(const char *prefix, const char *arg)
{
	int ret;
	char *dup;

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(prefix, "QSTN");

	/* allow lower or upper names */
	dup = strtoupper(strdup(arg));

	if(strcmp(dup, "DVR") == 0 || strcmp(dup, "VCR") == 0)
		ret = cmd_attempt(prefix, "00");
	else if(strcmp(dup, "CABLE") == 0 || strcmp(dup, "SAT") == 0)
		ret = cmd_attempt(prefix, "01");
	else if(strcmp(dup, "TV") == 0)
		ret = cmd_attempt(prefix, "02");
	else if(strcmp(dup, "AUX") == 0)
		ret = cmd_attempt(prefix, "03");
	else if(strcmp(dup, "DVD") == 0)
		ret = cmd_attempt(prefix, "10");
	else if(strcmp(dup, "TAPE") == 0)
		ret = cmd_attempt(prefix, "20");
	else if(strcmp(dup, "PHONO") == 0)
		ret = cmd_attempt(prefix, "22");
	else if(strcmp(dup, "CD") == 0)
		ret = cmd_attempt(prefix, "23");
	else if(strcmp(dup, "FM") == 0 || strcmp(dup, "FM TUNER") == 0)
		ret = cmd_attempt(prefix, "24");
	else if(strcmp(dup, "AM") == 0 || strcmp(dup, "AM TUNER") == 0)
		ret = cmd_attempt(prefix, "25");
	else if(strcmp(dup, "TUNER") == 0)
		ret = cmd_attempt(prefix, "26");
	else if(strcmp(dup, "MULTICH") == 0)
		ret = cmd_attempt(prefix, "30");
	else if(strcmp(dup, "XM") == 0)
		ret = cmd_attempt(prefix, "31");
	else if(strcmp(dup, "SIRIUS") == 0)
		ret = cmd_attempt(prefix, "32");
	/* the following are only valid for zone 2 */
	else if(strcmp(prefix, "SLZ") == 0) {
		if(strcmp(dup, "OFF") == 0)
			ret = cmd_attempt(prefix, "7F");
		else if(strcmp(dup, "SOURCE") == 0)
			ret = cmd_attempt(prefix, "80");
		else
			/* unrecognized command */
			ret = -1;
	}
	else
		/* unrecognized command */
		ret = -1;

	free(dup);
	return(ret);
}

static int handle_mode(const char *prefix, const char *arg)
{
	int ret;
	char *dup;

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(prefix, "QSTN");

	/* allow lower or upper names */
	dup = strtoupper(strdup(arg));

	if(strcmp(dup, "STEREO") == 0)
		ret = cmd_attempt(prefix, "00");
	else if(strcmp(dup, "DIRECT") == 0)
		ret = cmd_attempt(prefix, "01");
	else if(strcmp(dup, "ACSTEREO") == 0)
		ret = cmd_attempt(prefix, "0C");
	else if(strcmp(dup, "FULLMONO") == 0)
		ret = cmd_attempt(prefix, "13");
	else if(strcmp(dup, "PURE") == 0)
		ret = cmd_attempt(prefix, "11");
	else if(strcmp(dup, "STRAIGHT") == 0)
		ret = cmd_attempt(prefix, "40");
	else if(strcmp(dup, "THX") == 0)
		ret = cmd_attempt(prefix, "42");
	else if(strcmp(dup, "PLIIMOVIE") == 0)
		ret = cmd_attempt(prefix, "80");
	else if(strcmp(dup, "PLIIMUSIC") == 0)
		ret = cmd_attempt(prefix, "81");
	else if(strcmp(dup, "NEO6CINEMA") == 0)
		ret = cmd_attempt(prefix, "82");
	else if(strcmp(dup, "NEO6MUSIC") == 0)
		ret = cmd_attempt(prefix, "83");
	else if(strcmp(dup, "PLIITHX") == 0)
		ret = cmd_attempt(prefix, "84");
	else if(strcmp(dup, "NEO6THX") == 0)
		ret = cmd_attempt(prefix, "85");
	else if(strcmp(dup, "PLIIGAME") == 0)
		ret = cmd_attempt(prefix, "86");
	else if(strcmp(dup, "NEURALTHX") == 0)
		ret = cmd_attempt(prefix, "88");
	else
		/* unrecognized command */
		ret = -1;

	free(dup);
	return(ret);
}

static int handle_tune(const char *prefix, const char *arg)
{
	char cmdstr[6]; /* "00000\0" */
	char *test;

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(prefix, "QSTN");
	else if(strcmp(arg, "up") == 0)
		return cmd_attempt(prefix, "UP");
	else if(strcmp(arg, "down") == 0)
		return cmd_attempt(prefix, "DOWN");

	/* Otherwise we should have a frequency. It can be one of two formats:
	 * FM: (1)00.0 (possible hundreds spot, with decimal point)
	 * AM: (1)000 (possible thousands spot, with NO decimal point)
	 */
	if(strchr(arg, '.')) {
		/* attempt to parse as FM */
		errno = 0;
		double freq = strtod(arg, &test);
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
		/* should be AM, single number with no decimal */
		errno = 0;
		int freq = strtol(arg, &test, 10);
		if(errno != 0) {
			/* parse error, not a number */
			return(-1);
		}
		if(freq < 530 || freq > 1710) {
			/* range error */
			return(-1);
		}
		/* we want to print something like "TUN00780" */
		sprintf(cmdstr, "%05d", freq);
	}
	return cmd_attempt(prefix, cmdstr);
}

static int handle_sleep(const char *prefix, const char *arg)
{
	long int mins;
	char *test;
	char cmdstr[3]; /* "XX\0" */

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(prefix, "QSTN");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(prefix, "OFF");

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
	sprintf(cmdstr, "%02lX", mins);
	/* send the command */
	return cmd_attempt(prefix, cmdstr);
}

static int handle_status(const char *prefix, const char *arg)
{
	int ret = 0;

	/* this handler is a bit different in that we call
	 * multiple receiver commands */
	if(!arg || strcmp(arg, "main") == 0) {
		ret += cmd_attempt("PWR", "QSTN");
		ret += cmd_attempt("MVL", "QSTN");
		ret += cmd_attempt("AMT", "QSTN");
		ret += cmd_attempt("SLI", "QSTN");
		ret += cmd_attempt("LMD", "QSTN");
		ret += cmd_attempt("TUN", "QSTN");
	} else if(strcmp(arg, "zone2") == 0) {
		ret += cmd_attempt("ZPW", "QSTN");
		ret += cmd_attempt("ZVL", "QSTN");
		ret += cmd_attempt("ZMT", "QSTN");
		ret += cmd_attempt("SLZ", "QSTN");
		ret += cmd_attempt("TUZ", "QSTN");
	} else {
		return(-1);
	}

	return(ret < 0 ? -2 : 0);
}

static int handle_raw(const char *prefix, const char *arg)
{
	return cmd_attempt("", arg);
}


/**
 * Add the command with the given name and handler to our command list. This
 * will allow process_command() to locate the correct handler for a command.
 * @param name the name of the command, e.g. "volume"
 * @param prefix the first part of the receiver command string, aka "PWR"
 * @param handler the function that will handle the command
 */
static void add_command(const char *name, const char *prefix,
		cmd_handler handler)
{
	/* create our new command object */
	struct command *cmd = calloc(1, sizeof(struct command));
	cmd->name = strdup(name);
	cmd->prefix = prefix ? strdup(prefix) : NULL;
	cmd->handler = handler;

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
	/*
	add_command(name,       prefix, handle_func); */
	add_command("power",    "PWR", handle_boolean);
	add_command("volume",   "MVL", handle_volume);
	add_command("mute",     "AMT", handle_boolean);
	add_command("input",    "SLI", handle_input);
	add_command("mode",     "LMD", handle_mode);
	add_command("tune",     "TUN", handle_tune);

	add_command("z2power",  "ZPW", handle_boolean);
	add_command("z2volume", "ZVL", handle_volume);
	add_command("z2mute",   "ZMT", handle_boolean);
	add_command("z2input",  "SLZ", handle_input);
	add_command("z2tune",   "TUZ", handle_tune);

	add_command("sleep",    "SLP", handle_sleep);

	add_command("status",   NULL,  handle_status);
	add_command("raw",      NULL,  handle_raw);
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
		free(cmd->name);
		free(cmd->prefix);
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
	char *cmdstr, *argstr;
	struct command *cmd;

	if(!str)
		return(-1);

	cmdstr = strdup(str);
	/* start by splitting the string after the cmd */
	argstr = strchr(cmdstr, ' ');
	/* if we had an arg, set our pointers correctly */
	if(argstr) {
		*argstr = '\0';
		argstr++;
	}

	cmd = command_list;
	while(cmd) {
		if(strcmp(cmd->name, cmdstr) == 0) {
			/* we found the handler, call it and return the result */
			int ret = cmd->handler(cmd->prefix, argstr);
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
	if(strstr(cmd, "PWR") != NULL || strstr(cmd, "ZPW") != NULL)
		return(1);
	return(0);
}

/* vim: set ts=4 sw=4 noet: */
