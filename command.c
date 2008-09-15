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

typedef int (cmd_handler) (int, const char *);

struct command {
	char *name;
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
 * Write a receiver status message out to our output channel.
 * @param status the receiver status message to make human readable
 * @return the human readable status message, must be freed
 */
static char *parse_status(const char *status)
{
	char *trim, *sptr, *eptr, *ret;
	/* copy the string so we can trim the start and end portions off */
	trim = strdup(status);
	sptr = trim + strlen(START_RECV);
	eptr = strstr(sptr, END_RECV);
	if(eptr)
		*eptr = '\0';

	if(strcmp(sptr, "PWR00") == 0)
		ret = strdup("OK:power:off\n");
	else if(strcmp(sptr, "PWR01") == 0)
		ret = strdup("OK:power:on\n");

	else if(strcmp(sptr, "AMT00") == 0)
		ret = strdup("OK:mute:off\n");
	else if(strcmp(sptr, "AMT01") == 0)
		ret = strdup("OK:mute:on\n");

	else if(strncmp(sptr, "MVL", 3) == 0) {
		/* parse the volume number out */
		char *pos;
		/* read volume level in as a base 16 (hex) number */
		long level = strtol(sptr + 3, &pos, 16);
		/* TODO parse error, pos != '\0' */
		ret = calloc(10 + 3 + 1, sizeof(char));
		sprintf(ret, "OK:volume:%ld\n", level);
	}

	else if(strcmp(sptr, "SLI01") == 0)
		ret = strdup("OK:input:Cable\n");
	else if(strcmp(sptr, "SLI02") == 0)
		ret = strdup("OK:input:TV\n");
	else if(strcmp(sptr, "SLI03") == 0)
		ret = strdup("OK:input:Aux\n");
	else if(strcmp(sptr, "SLI10") == 0)
		ret = strdup("OK:input:DVD\n");
	else if(strcmp(sptr, "SLI23") == 0)
		ret = strdup("OK:input:CD\n");
	else if(strcmp(sptr, "SLI24") == 0)
		ret = strdup("OK:input:FM Tuner\n");
	else if(strcmp(sptr, "SLI25") == 0)
		ret = strdup("OK:input:AM Tuner\n");
	else if(strcmp(sptr, "SLI26") == 0)
		ret = strdup("OK:input:Tuner\n");

	else if(strcmp(sptr, "LMD00") == 0)
		ret = strdup("OK:mode:Stereo\n");
	else if(strcmp(sptr, "LMD01") == 0)
		ret = strdup("OK:mode:Direct\n");
	else if(strcmp(sptr, "LMD0C") == 0)
		ret = strdup("OK:mode:All Channel Stereo\n");
	else if(strcmp(sptr, "LMD11") == 0)
		ret = strdup("OK:mode:Pure Audio\n");
	else if(strcmp(sptr, "LMD40") == 0)
		ret = strdup("OK:mode:Straight Decode\n");
	else if(strcmp(sptr, "LMD42") == 0)
		ret = strdup("OK:mode:THX Cinema\n");
	else if(strcmp(sptr, "LMD80") == 0)
		ret = strdup("OK:mode:Pro Logic IIx Movie\n");
	else if(strcmp(sptr, "LMD81") == 0)
		ret = strdup("OK:mode:Pro Logic IIx Music\n");
	else if(strcmp(sptr, "LMD86") == 0)
		ret = strdup("OK:mode:Pro Logic IIx Game\n");

	else if(strncmp(sptr, "TUN", 3) == 0) {
		/* parse the frequency number out */
		char *pos;
		/* read frequency in as a base 10 number */
		long freq = strtol(sptr + 3, &pos, 10);
		/* TODO parse error, pos != '\0' */
		if(freq > 8000) {
			/* FM frequency, something like 09790 was read */
			ret = calloc(8 + 5 + 5, sizeof(char));
			sprintf(ret, "OK:tune:%3.1f FM\n", (double)freq / 100.0);
		} else {
			/* AM frequency, something like 00780 was read */
			ret = calloc(8 + 4 + 5, sizeof(char));
			sprintf(ret, "OK:tune:%ld AM\n", freq);
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
 * ending characters, turn it into a valid command, and send it to the
 * receiver. It will then attempt to parse the status message returned
 * by the receiver and return a human-readable status message.
 * @param serialfd the fd used for sending commands to the receiver
 * @param cmd a receiver command string, minus preamble and end characters
 * @return 0 on success, -2 on receiver failure
 */
static int cmd_attempt(int serialfd, const char *cmd)
{
	int ret;
	char *fullcmd;

	fullcmd = malloc(strlen(START_SEND) + strlen(cmd)
			+ strlen(END_SEND) + 1);
	sprintf(fullcmd, START_SEND "%s" END_SEND, cmd);

	/* send the command to the receiver */
	ret = rcvr_send_command(serialfd, fullcmd);

	free(fullcmd);
	return(ret < 0 ? -2 : 0);
}


static int handle_power(int serialfd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(serialfd, "PWRQSTN");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(serialfd, "PWR01");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(serialfd, "PWR00");

	/* unrecognized command */
	return(-1);
}

static int handle_volume(int serialfd, const char *arg)
{
	long int level;
	char *test;
	char cmdstr[6]; /* "MVLXX\0" */

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(serialfd, "MVLQSTN");
	else if(strcmp(arg, "up") == 0)
		return cmd_attempt(serialfd, "MVLUP");
	else if(strcmp(arg, "down") == 0)
		return cmd_attempt(serialfd, "MVLDOWN");

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
	sprintf(cmdstr, "MVL%lX", level);
	/* send the command */
	return cmd_attempt(serialfd, cmdstr);
}

static int handle_mute(int serialfd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(serialfd, "AMTQSTN");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(serialfd, "AMT01");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(serialfd, "AMT00");
	else if(strcmp(arg, "toggle") == 0)
		return cmd_attempt(serialfd, "AMTTG");

	/* unrecognized command */
	return(-1);
}

static int handle_input(int serialfd, const char *arg)
{
	int ret;
	char *dup;

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(serialfd, "SLIQSTN");
	/* allow lower or upper names */
	dup = strtoupper(strdup(arg));

	if(strcmp(dup, "CABLE") == 0)
		ret = cmd_attempt(serialfd, "SLI01");
	else if(strcmp(dup, "TV") == 0)
		ret = cmd_attempt(serialfd, "SLI02");
	else if(strcmp(dup, "AUX") == 0)
		ret = cmd_attempt(serialfd, "SLI03");
	else if(strcmp(dup, "DVD") == 0)
		ret = cmd_attempt(serialfd, "SLI10");
	else if(strcmp(dup, "CD") == 0)
		ret = cmd_attempt(serialfd, "SLI23");
	else if(strcmp(dup, "FM") == 0)
		ret = cmd_attempt(serialfd, "SLI24");
	else if(strcmp(dup, "AM") == 0)
		ret = cmd_attempt(serialfd, "SLI25");
	else if(strcmp(dup, "TUNER") == 0)
		ret = cmd_attempt(serialfd, "SLI26");
	else
		/* unrecognized command */
		ret = -1;

	free(dup);
	return(ret);
}

static int handle_mode(int serialfd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(serialfd, "LMDQSTN");

	/* unrecognized command */
	return(-1);
}

static int handle_tune(int serialfd, const char *arg)
{
	char cmdstr[9]; /* "TUN00000\0" */

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(serialfd, "TUNQSTN");
	else if(strcmp(arg, "up") == 0)
		return cmd_attempt(serialfd, "TUNUP");
	else if(strcmp(arg, "down") == 0)
		return cmd_attempt(serialfd, "TUNDOWN");

	/* Otherwise we should have a frequency. It can be one of two formats:
	 * FM: (1)00.0
	 * AM: (1)000
	 * TODO intelligent parsing */
	if(strchr(arg, '.')) {
		/* attempt to parse as FM */
		char *test;
		double freq = strtod(arg, &test);
		if(*test != '\0') {
			/* parse error, not a number */
			return(-1);
		}
		if(freq < 87.5 || freq > 107.9) {
			/* range error */
			return(-1);
		}
		/* we want to print something like "TUN09790" */
		sprintf(cmdstr, "TUN%05.0f", freq * 100.0);
	} else {
		/* should be AM, single number with no decimal */
		char *test;
		int freq = strtol(arg, &test, 10);
		if(*test != '\0') {
			/* parse error, not a number */
			return(-1);
		}
		if(freq < 530 || freq > 1710) {
			/* range error */
			return(-1);
		}
		/* we want to print something like "TUN00780" */
		sprintf(cmdstr, "TUN%05d", freq);
	}
	return cmd_attempt(serialfd, cmdstr);
}

static int handle_status(int serialfd, const char *arg)
{
	int ret = 0;

	/* this handler is a bit different in that we call 4 receiver commands */
	ret += cmd_attempt(serialfd, "PWRQSTN");
	ret += cmd_attempt(serialfd, "MVLQSTN");
	ret += cmd_attempt(serialfd, "AMTQSTN");
	ret += cmd_attempt(serialfd, "SLIQSTN");
	ret += cmd_attempt(serialfd, "LMDQSTN");
	ret += cmd_attempt(serialfd, "TUNQSTN");

	return(ret < 0 ? -2 : 0);
}

static int handle_raw(int serialfd, const char *arg)
{
	return cmd_attempt(serialfd, arg);
}

static int handle_unimplemented(int serialfd, const char *arg)
{
	return(-1);
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
 * @param handler the function that will handle the command
 */
static void add_command(char *name, cmd_handler handler)
{
	/* create our new command object */
	struct command *cmd = calloc(1, sizeof(struct command));
	cmd->name = strdup(name);
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
	add_command(name,       handle_func); */
	add_command("power",    handle_power);
	add_command("volume",   handle_volume);
	add_command("mute",     handle_mute);
	add_command("input",    handle_input);
	add_command("mode",     handle_mode);
	add_command("tune",     handle_tune);
	add_command("status",   handle_status);
	add_command("raw",      handle_raw);

	add_command("z2power",  handle_unimplemented);
	add_command("z2volume", handle_unimplemented);
	add_command("z2mute",   handle_unimplemented);
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
		free(cmd);
		cmd = cmdnext;
	}
}

/** 
 * Process an incoming command, parsing it into the standard "<cmd> <arg>"
 * format. Attempt to locate a handler for the given command and delegate
 * the work to it. If no handler is found, return an error; otherwise
 * return the relevant human-readable status message.
 * @param serialfd the fd used for sending commands to the receiver
 * @param str the full command string, e.g. "power on"
 * @return 0 if the command string was correct and sent, -1 on invalid command
 * string, -2 on failed receiver write
 */
int process_command(int serialfd, const char *str)
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
			int ret = cmd->handler(serialfd, argstr);
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
