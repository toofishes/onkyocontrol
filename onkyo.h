/**
 * @file onkyo.h
 * This file contains a list of defines and function defs shared by files
 * in our mini application.
 */

#ifndef ONKYO_H
#define ONKYO_H

#include <sys/types.h> /* ssize_t, size_t */

/** The serial port device to connect on */
#define SERIALDEVICE "/dev/ttyS0"

/** The hostname to listen on; "any" will listen on any interface */
#define LISTENHOST "any"
/** The port number to listen on (note: it is a string, not a num) */
#define LISTENPORT "8701"

/** Max size for our listener pool */
#define MAX_LISTENERS 1
/** Max size for our connection pool */
#define MAX_CONNECTIONS 5

/** Size to use for all static buffers */
#define BUF_SIZE 64

/** Time (in seconds) to automatically time out connections */
#define CONN_TIMEOUT 300

/** Time (in milliseconds) to wait between receiver commands */
#define COMMAND_WAIT 80

/* characters standard to the start and end of our communication messages */
#define START_SEND "!1"
#define END_SEND "\n"
#define START_RECV "!1"
#define END_RECV ""

/* onkyo.c - functions operating on our static vars */
int queue_rcvr_command(char *cmd);

/* receiver.c - receiver interaction functions */
int rcvr_send_command(int serialfd, const char *cmd);
int rcvr_handle_status(int serialfd, char **status);

/* command.c - user command init/teardown */
void init_commands(void);
void free_commands(void);

/* command.c - command and status processing */
char *process_incoming_message(int serialfd);
int process_command(const char *str);

/* util.c - trivial utility functions */
void xclose(int fd);
ssize_t xread(int fd, void *buf, size_t len);
ssize_t xwrite(int fd, const void *buf, size_t len);
/* if using ISO C, strdup() is not actually defined, provide our own */
#ifndef strdup
char *strdup(const char *s);
#endif /* strdup */

#endif /* ONKYO_H */

/* vim: set ts=4 sw=4 noet: */
