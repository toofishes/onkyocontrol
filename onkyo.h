/**
 * @file onkyo.h
 * This file contains a list of defines and function defs shared by files
 * in our mini application.
 */

#ifndef ONKYO_H
#define ONKYO_H

#include <sys/time.h>  /* struct timeval */
#include <sys/types.h> /* ssize_t, size_t */

/** The default port number to listen on (note: it is a string, not a num) */
#define LISTENPORT "8701"

/** Max size for our connection pool */
#define MAX_CONNECTIONS 200

/** Size to use for all static buffers */
#define BUF_SIZE 64

/** Time (in milliseconds) to wait between receiver commands */
#define COMMAND_WAIT 80

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

/** Power status bit values */
enum power {
	POWER_OFF   = 0,
	MAIN_POWER  = (1 << 0),
	ZONE2_POWER = (1 << 1),
	ZONE3_POWER = (1 << 2),
};

/** Keep track of two paired file descriptors */
enum pipehalfs { READ = 0, WRITE = 1 };

/** Represents a command waiting to be sent to the receiver */
struct cmdqueue {
	unsigned long hash;
	char cmd[BUF_SIZE];
	struct cmdqueue *next;
};

/** Our Receiver device and associated dealings */
struct receiver {
	int fd;
	int type;
	enum power power;
	unsigned long cmds_sent;
	unsigned long msgs_received;
	struct timeval last_cmd;
	struct timeval zone2_sleep;
	struct timeval zone3_sleep;
	struct timeval next_sleep_update;
	struct cmdqueue *queue;
	struct receiver *next;
};


/* onkyo.c - general functions */
int write_to_connections(const char *msg);

/* receiver.c - receiver interaction functions, status processing */
void init_statuses(void);
int rcvr_send_command(struct receiver *rcvr);
int process_incoming_message(struct receiver *rcvr, int logfd);

/* command.c - user command processing */
void init_commands(void);
int process_command(struct receiver *rcvr, const char *str);
int is_power_command(const char *cmd);
int write_fakesleep_status(struct receiver *rcvr,
		struct timeval now, char zone);

/* util.c - trivial utility functions */
int xopen(const char *path, int oflag);
int xclose(int fd);
ssize_t xread(int fd, void *buf, size_t len);
ssize_t xwrite(int fd, const void *buf, size_t len);
unsigned long hash_sdbm(const char *str);

void timeval_diff(struct timeval * restrict a,
		struct timeval * restrict b, struct timeval * restrict result);
struct timeval timeval_min(struct timeval *restrict a,
		struct timeval * restrict b);
int timeval_positive(struct timeval *tv);
#define timeval_clear(tv) do { (tv).tv_sec = 0; (tv).tv_usec = 0; } while(0)

#endif /* ONKYO_H */

/* vim: set ts=4 sw=4 noet: */
