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

/* common messages */
const char * const invalid_cmd = "ERROR:Invalid Command\n";
const char * const rcvr_err = "ERROR:Receiver Error\n";


typedef int (cmd_handler) (int, int, const char *);

struct command {
	char *name;
	cmd_handler *handler;
	struct command *next;
};

/**
 * write wrapper that calls strlen() for the last parameter.
 * @param fd the file descriptor to write to
 * @param str the string to write
 * @return return value of the underlying write
 */
static int easy_write(int fd, const char *str)
{
	return xwrite(fd, str, strlen(str));
}

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
 * Write an invalid command message out to our output channel.
 * @param fd the file descriptor to write to
 * @return -2
 */
static int cmd_invalid(int fd)
{
	easy_write(fd, invalid_cmd);
	return(-2);
}

/**
 * Write a receiver status message out to our output channel.
 * @param fd the file descriptor to write to
 * @param status the receiver status message to make human readable
 */
static void parse_status(int fd, const char *status)
{
	char *trim, *sptr, *eptr;
	/* copy the string so we can trim the start and end portions off */
	trim = strdup(status);
	sptr = trim + strlen(START_RECV);
	eptr = strstr(sptr, END_RECV);
	if(eptr)
		*eptr = '\0';

	if(strcmp(sptr, "SLI10") == 0)
		easy_write(fd, "OK:input:DVD\n");
	else if(strcmp(sptr, "SLI26") == 0)
		easy_write(fd, "OK:input:Tuner\n");
	else if(strcmp(sptr, "SLI02") == 0)
		easy_write(fd, "OK:input:TV\n");
	else if(strcmp(sptr, "SLI23") == 0)
		easy_write(fd, "OK:input:CD\n");
	else if(strcmp(sptr, "SLI03") == 0)
		easy_write(fd, "OK:input:test\n");
	else {
		easy_write(fd, "OK:todo:");
		easy_write(fd, sptr);
		easy_write(fd, "\n");
	}
	free(trim);
}

/**
 * Attempt to write a receiver command out to the control channel.
 * This will take a in a simple receiver string minus any preamble or
 * ending characters, turn it into a valid command, and send it to the
 * receiver. It will then attempt to parse the status message returned
 * by the receiver and display that result on our output channel.
 * @param outputfd the fd used for any eventual output
 * @param serialfd the fd used for sending commands to the receiver
 * @param cmd a receiver command string, minus preamble and end characters
 * @return 0 on success, -1 on receiver failure
 */
static int cmd_attempt(int outputfd, int serialfd, const char *cmd)
{
	int ret;
	char *status = NULL;
	char *fullcmd;

	fullcmd = malloc(strlen(START_SEND) + strlen(cmd)
			+ strlen(END_SEND) + 1);
	sprintf(fullcmd, START_SEND "%s" END_SEND, cmd);

	/* send the command to the receiver */
	ret = rcvr_send_command(serialfd, fullcmd, &status);
	if(ret != -1) {
		/* parse the return and output a status message */
		parse_status(outputfd, status);
	} else {
		easy_write(outputfd, rcvr_err);
	}

	free(fullcmd);
	free(status);
	return(ret);
}


static int handle_power(int outputfd, int serialfd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(outputfd, serialfd, "PWRQSTN");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(outputfd, serialfd, "PWR01");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(outputfd, serialfd, "PWR00");
	else
		/* unrecognized command */
		return cmd_invalid(outputfd);
}

static int handle_volume(int outputfd, int serialfd, const char *arg)
{
	long int level;
	char *test;
	char cmdstr[6]; /* "MVLXX\0" */

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(outputfd, serialfd, "MVLQSTN");
	else if(strcmp(arg, "up") == 0)
		return cmd_attempt(outputfd, serialfd, "MVLUP");
	else if(strcmp(arg, "down") == 0)
		return cmd_attempt(outputfd, serialfd, "MVLDOWN");

	/* otherwise we probably have a number */
	level = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return cmd_invalid(outputfd);
	}
	if(level < 0 || level > 100) {
		/* range error */
		return cmd_invalid(outputfd);
	}
	/* create our command */
	sprintf(cmdstr, "MVL%lX", level);
	/* send the command */
	return cmd_attempt(outputfd, serialfd, cmdstr);
}

static int handle_mute(int outputfd, int serialfd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(outputfd, serialfd, "AMTQSTN");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(outputfd, serialfd, "AMT01");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(outputfd, serialfd, "AMT00");
	else if(strcmp(arg, "toggle") == 0)
		return cmd_attempt(outputfd, serialfd, "AMTTG");
	else
		/* unrecognized command */
		return cmd_invalid(outputfd);
}

static int handle_input(int outputfd, int serialfd, const char *arg)
{
	int ret;
	char *dup;

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(outputfd, serialfd, "SLIQSTN");
	/* allow lower or upper names */
	dup = strtoupper(strdup(arg));

	if(strcmp(arg, "DVD") == 0)
		ret = cmd_attempt(outputfd, serialfd, "SLI10");
	else if(strcmp(arg, "TUNER") == 0)
		ret = cmd_attempt(outputfd, serialfd, "SLI26");
	else if(strcmp(arg, "TV") == 0)
		ret = cmd_attempt(outputfd, serialfd, "SLI02");
	else if(strcmp(arg, "CD") == 0)
		ret = cmd_attempt(outputfd, serialfd, "SLI23");
	else
		/* unrecognized command */
		ret = cmd_invalid(outputfd);

	free(dup);
	return(ret);
}

static int handle_status(int outputfd, int serialfd, const char *arg)
{
	int ret = 0;
	/* this handler is a bit different in that we call 4 receiver commands
	 * and get the output from each. */
	ret += cmd_attempt(outputfd, serialfd, "PWRQSTN");
	ret += cmd_attempt(outputfd, serialfd, "MVLQSTN");
	ret += cmd_attempt(outputfd, serialfd, "AMTQSTN");
	ret += cmd_attempt(outputfd, serialfd, "SLIQSTN");

	return(ret);
}

static int handle_unimplemented(int outputfd, int serialfd, const char *arg)
{
	return cmd_invalid(outputfd);
}

/** 
 * Process a status message to be read from the receiver (one that the
 * receiver initiated). Print a human-readable status message on the
 * given file descriptor.
 * @param outputfd the fd used for any eventual output
 * @param serialfd the fd used for sending commands to the receiver
 * @return 0 on success, -1 on receiver failure
 */
int process_incoming_message(int outputfd, int serialfd)
{
	int ret;
	char *status = NULL;

	/* send the command to the receiver */
	ret = rcvr_handle_status(serialfd, &status);
	if(ret != -1) {
		/* parse the return and output a status message */
		parse_status(outputfd, status);
	} else {
		easy_write(outputfd, rcvr_err);
	}

	free(status);
	return(ret);
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
	add_command("status",   handle_status);

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
 * Process an incoming command, parsing it into the standard <cmd> <arg>
 * format. Attempt to locate a handler for the given command and delegate
 * the work to it. If no handler is found, return an error.
 * @param outputfd the fd used for any eventual output
 * @param serialfd the fd used for sending commands to the receiver
 * @param str the full command string, e.g. "power on"
 * @return 0 on success, -1 on receiver failure, -2 on invalid command
 */
int process_command(int outputfd, int serialfd, const char *str)
{
	char *cmdstr, *argstr;
	struct command *cmd;

	if(!str) {
		return cmd_invalid(outputfd);
	}

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
			int ret = cmd->handler(outputfd, serialfd, argstr);
			free(cmdstr);
			return(ret);
		}
		cmd = cmd->next;
	}

	/* we didn't find a handler, must be an invalid command */
	free(cmdstr);
	return cmd_invalid(outputfd);
}

