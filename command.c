
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
/* so we don't have to iterate the list every time to add */
static struct command *command_list_last = NULL;

/* common messages */
const char * const invalid_cmd = "error:Invalid Command\n";
const char * const rcvr_err = "error:Receiver Error\n";


typedef int (cmd_handler) (int, const char *);

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
	return write(fd, str, strlen(str));
}

/**
 * Convert a string to uppercase.
 * @param str string to convert (in place)
 * @return pointer to the string
 */
char *strtoupper(char *str)
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
	if(strcmp(status, "SLI10") == 0)
		easy_write(fd, "input:DVD\n");
	else if(strcmp(status, "SLI26") == 0)
		easy_write(fd, "input:Tuner\n");
	else if(strcmp(status, "SLI02") == 0)
		easy_write(fd, "input:TV\n");
	else if(strcmp(status, "SLI23") == 0)
		easy_write(fd, "input:CD\n");
	else {
		easy_write(fd, "todo:");
		easy_write(fd, status);
		easy_write(fd, "\n");
	}
}

/**
 * Write an invalid command message out to our output channel.
 * @param fd the file descriptor to write to
 * @return 0 on success, -1 on receiver failure
 */
static int cmd_attempt(int fd, const char *cmd)
{
	int ret;
	char *status;

	/* send the command to the receiver */
	ret = rcvr_send_command(cmd, &status);
	if(ret != -1) {
		/* parse the return and output a status message */
		parse_status(fd, status);
	} else {
		easy_write(fd, rcvr_err);
	}

	free(status);
	return(ret);
}


static int handle_power(int fd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(fd, "PWRQSTN\r");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(fd, "PWR01\r");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(fd, "PWR00\r");
	else
		/* unrecognized command */
		return cmd_invalid(fd);
}

static int handle_volume(int fd, const char *arg)
{
	long int level;
	char *test;
	char cmdstr[BUF_SIZE];

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(fd, "MVLQSTN\r");
	else if(strcmp(arg, "up") == 0)
		return cmd_attempt(fd, "MVLUP\r");
	else if(strcmp(arg, "down") == 0)
		return cmd_attempt(fd, "MVLDOWN\r");

	/* otherwise we probably have a number */
	level = strtol(arg, &test, 10);
	if(*test != '\0') {
		/* parse error, not a number */
		return cmd_invalid(fd);
	}
	if(level < 0 || level > 100) {
		/* range error */
		return cmd_invalid(fd);
	}
	/* create our command */
	sprintf(cmdstr, "MVL%lX\r", level);
	/* send the command */
	return cmd_attempt(fd, cmdstr);
}

static int handle_mute(int fd, const char *arg)
{
	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(fd, "AMTQSTN\r");
	else if(strcmp(arg, "on") == 0)
		return cmd_attempt(fd, "AMT01\r");
	else if(strcmp(arg, "off") == 0)
		return cmd_attempt(fd, "AMT00\r");
	else if(strcmp(arg, "toggle") == 0)
		return cmd_attempt(fd, "AMTTG\r");
	else
		/* unrecognized command */
		return cmd_invalid(fd);
}

static int handle_input(int fd, const char *arg)
{
	int ret;
	char *dup;

	if(!arg || strcmp(arg, "status") == 0)
		return cmd_attempt(fd, "SLIQSTN\r");
	/* allow lower or upper names */
	dup = strtoupper(strdup(arg));

	if(strcmp(arg, "DVD") == 0)
		ret = cmd_attempt(fd, "SLI10\r");
	else if(strcmp(arg, "TUNER") == 0)
		ret = cmd_attempt(fd, "SLI26\r");
	else if(strcmp(arg, "TV") == 0)
		ret = cmd_attempt(fd, "SLI02\r");
	else if(strcmp(arg, "CD") == 0)
		ret = cmd_attempt(fd, "SLI23\r");
	else
		/* unrecognized command */
		ret = cmd_invalid(fd);

	free(dup);
	return(ret);
}

static int handle_status(int fd, const char *arg)
{
	return cmd_invalid(fd);
}

static int handle_unimplemented(int fd, const char *arg)
{
	return cmd_invalid(fd);
}

/** 
 * Process a status message to be read from the receiver. Print a human-
 * readable status message on the given file descriptor.
 * @param fd the file descriptor to write any output back to
 * @return 0 on success, -1 on receiver failure
 */
int process_status(int fd)
{
	int ret;
	char *status;

	/* send the command to the receiver */
	ret = rcvr_handle_status(&status);
	if(ret != -1) {
		/* parse the return and output a status message */
		parse_status(fd, status);
	} else {
		easy_write(fd, rcvr_err);
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
void add_command(char *name, cmd_handler handler)
{
	/* create our new command object */
	struct command *cmd = malloc(sizeof(struct command));
	cmd->name = strdup(name);
	cmd->handler = handler;

	/* add it to our list, first item is special case */
	if(!command_list) {
		command_list = cmd;
	} else {
		command_list_last->next = cmd;
	}
	command_list_last = cmd;
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
	command_list_last = NULL;
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
 * @param fd the file descriptor to write any output back to
 * @param str the full command string, e.g. "power on"
 * @return 0 on success, -1 on receiver failure, -2 on invalid command
 */
int process_command(int fd, const char *str)
{
	char *cmdstr, *argstr;
	struct command *cmd;

	if(!str) {
		return cmd_invalid(fd);
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
			int ret = cmd->handler(fd, argstr);
			free(cmd);
			return(ret);
		}
	}

	/* we didn't find a handler, must be an invalid command */
	free(cmd);
	return cmd_invalid(fd);
}

