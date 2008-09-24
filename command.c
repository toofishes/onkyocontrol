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
#include <unistd.h>
#include <ctype.h>
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
 *   power:on
 * volume status (return 0-100)
 *   volume:44
 * mute status (return on/off)
 *   mute:off
 * input status (return string name)
 *   input:DVD
 * status:
 *   power:on
 *   volume:44
 *   mute:off
 *   input:DVD
 *   z2power:off
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
		(*ptr) = toupper(*ptr);
		ptr++;
	}
	return(str);
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

	{ "LMD00", "OK:mode:Stereo\n" },
	{ "LMD01", "OK:mode:Direct\n" },
	{ "LMD0C", "OK:mode:All Channel Stereo\n" },
	{ "LMD11", "OK:mode:Pure Audio\n" },
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
};

/**
 * Write a receiver status message out to our output channel.
 * @param status the receiver status message to make human readable
 * @return the human readable status message, must be freed
 */
static char *parse_status(const char *status)
{
	unsigned int i, loopsize;
	char *trim, *sptr, *eptr, *ret = NULL;
	/* copy the string so we can trim the start and end portions off */
	trim = strdup(status);
	sptr = trim + strlen(START_RECV);
	eptr = strstr(sptr, END_RECV);
	if(eptr)
		*eptr = '\0';

	/* compile-time constant, should be # rows in statuses */
	loopsize = sizeof(statuses) / sizeof(*statuses);
	for(i = 0; i < loopsize; i++) {
		if(strcmp(sptr, statuses[i][0]) == 0) {
			ret = strdup(statuses[i][1]);
			break;
		}
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

	else {
		ret = calloc(8 + strlen(sptr) + 2, sizeof(char));
		sprintf(ret, "OK:todo:%s\n", sptr);
	}
	free(trim);
	return(ret);
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
	int ret;
	char *fullcmd;

	if(!cmd1 || !cmd2)
		return(-1);

	fullcmd = malloc(strlen(START_SEND) + strlen(cmd1) + strlen(cmd2)
			+ strlen(END_SEND) + 1);
	sprintf(fullcmd, START_SEND "%s%s" END_SEND, cmd1, cmd2);

	/* send the command to the receiver */
	ret = queue_rcvr_command(fullcmd);
	return(ret);
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

	if(strcmp(dup, "CABLE") == 0)
		ret = cmd_attempt(prefix, "01");
	else if(strcmp(dup, "SAT") == 0)
		ret = cmd_attempt(prefix, "01");
	else if(strcmp(dup, "TV") == 0)
		ret = cmd_attempt(prefix, "02");
	else if(strcmp(dup, "AUX") == 0)
		ret = cmd_attempt(prefix, "03");
	else if(strcmp(dup, "DVD") == 0)
		ret = cmd_attempt(prefix, "10");
	else if(strcmp(dup, "CD") == 0)
		ret = cmd_attempt(prefix, "23");
	else if(strcmp(dup, "FM") == 0 || strcmp(dup, "FM TUNER") == 0)
		ret = cmd_attempt(prefix, "24");
	else if(strcmp(dup, "AM") == 0 || strcmp(dup, "AM TUNER") == 0)
		ret = cmd_attempt(prefix, "25");
	else if(strcmp(dup, "TUNER") == 0)
		ret = cmd_attempt(prefix, "26");
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
 * Process a status message to be read from the receiver (one that the
 * receiver initiated). Return a human-readable status message.
 * @param serialfd the fd used for sending commands to the receiver
 * @return the human readable status message, must be freed
 */
char *process_incoming_message(int serialfd)
{
	int ret;
	char *msg, *status = NULL;

	/* send the command to the receiver */
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

/* vim: set ts=4 sw=4 noet: */
