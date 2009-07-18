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

/** Time (in milliseconds) to wait between receiver commands */
#define COMMAND_WAIT 80

/** Do we have the getaddrinfo function? */
#define HAVE_GETADDRINFO 1

/* allow marking of unused function parameters */
#if defined(__GNUC__)
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif

/* characters standard to the start and end of our communication messages */
#define START_SEND "!1"
#define END_SEND "\r\n"
#define START_RECV "!1"
#define END_RECV ""

/* power status bit values */
enum power {
	POWER_OFF   = 0,
	MAIN_POWER  = (1 << 0),
	ZONE2_POWER = (1 << 1),
	ZONE3_POWER = (1 << 2),
};

/* onkyo.c - functions operating on our static vars */
int queue_rcvr_command(char *cmd);

/* receiver.c - receiver interaction functions, status processing */
void init_statuses(void);
void free_statuses(void);
int rcvr_send_command(int serialfd, const char *cmd);
char *process_incoming_message(int serialfd, int logfd);
enum power initial_power_status(void);
enum power update_power_status(enum power pwr, const char *msg);

/* command.c - user command processing */
void init_commands(void);
void free_commands(void);
int process_command(const char *str);
int is_power_command(const char *cmd);

/* util.c - trivial utility functions */
int xopen(const char *path, int oflag);
int xclose(int fd);
ssize_t xread(int fd, void *buf, size_t len);
ssize_t xwrite(int fd, const void *buf, size_t len);
unsigned long hash_sdbm(const char *str);

#endif /* ONKYO_H */

/* vim: set ts=4 sw=4 noet: */
