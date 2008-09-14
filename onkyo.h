/**
 * @file onkyo.h
 * This file contains a list of defines and function defs shared by files
 * in our mini application.
 */

#ifndef ONKYO_H
#define ONKYO_H

#include <unistd.h> /* close, read, write */
#include <errno.h> /* for errno refs */

/** The serial port device to connect on */
#define SERIALDEVICE "/dev/ttyS0"

/** Max size for our serial device pool */
#define MAX_SERIALDEVS 1
/** Max size for our listener pool */
#define MAX_LISTENERS 1
/** Max size for our connection pool */
#define MAX_CONNECTIONS 5

/** Size to use for all static buffers */
#define BUF_SIZE 64

/** Time (in seconds) to automatically time out connections */
#define CONN_TIMEOUT 300

/* characters standard to the start and end of our communication messages */
#define START_SEND "!1"
#define END_SEND "\n"
#define START_RECV "!1"
#define END_RECV ""

/* receiver.c - receiver interaction functions */
int rcvr_send_command(int serialfd, const char *cmd);
int rcvr_handle_status(int serialfd, char **status);

/* command.c - user command init/teardown */
void init_commands(void);
void free_commands(void);

/* command.c - command and status processing */
char *process_incoming_message(int serialfd);
int process_command(int serialfd, const char *str);

/* trivial functions, keep them inlined */
static inline void xclose(int fd)
{
	while(close(fd) && errno == EINTR);
}

static inline ssize_t xread(int fd, void *buf, size_t len)
{
	ssize_t nr;
	while(1) {
		nr = read(fd, buf, len);
		if ((nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return nr;
	}
}

static inline ssize_t xwrite(int fd, const void *buf, size_t len)
{
	ssize_t nr;
	while(1) {
		nr = write(fd, buf, len);
		if ((nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return nr;
	}
}

#endif /* ONKYO_H */

/* vim: set ts=4 sw=4 noet: */
