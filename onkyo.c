/*
 *  onkyo.c - Onkyo receiver communication program
 *
 *  Copyright (c) 2008-2010 Dan McGee <dpmcgee@gmail.com>
 *
 *  Originally based on:
 *  miniterm.c - Sven Goldt <goldt@math.tu-berlin.de>
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

#define _POSIX_C_SOURCE 1 /* signal handlers, getaddrinfo */
#define _XOPEN_SOURCE 500 /* SA_RESTART */

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h> /* chdir, fork, pipe, setsid */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <termios.h>
#include <string.h>
#include <time.h>

#include "onkyo.h"

struct fdlist {
	int fd;
	struct fdlist *next;
};

struct conn {
	int fd;
	char *recv_buf;
	char *recv_buf_pos;
	struct conn *next;
};

/** file descriptor for raw output logging */
static int logfd = -1;
/** our list of receivers we send commands to */
static struct receiver *receivers = NULL;
/** our list of listening sockets/descriptors we accept connections on */
static struct fdlist *listeners = NULL;
/** our list of open connections we process commands on */
static struct conn *connections = NULL;
/** pipe used for async-safe signal handling in our select */
static int signalpipe[2] = { -1, -1 };

/* common messages */
static const char * const startup_msg = "OK:onkyocontrol v1.1\n";
static const char * const invalid_cmd = "ERROR:Invalid Command\n";
static const char * const max_conns = "ERROR:Max Connections Reached\n";
const char * const rcvr_err = "ERROR:Receiver Error\n";

/**
 * Establish everything we need for a connection once it has been
 * accepted. This will set up send and receive buffers and start
 * tracking the connection in our array.
 * @param fd the newly opened connection's file descriptor
 * @return 0 if initial write was successful, -1 if max connections
 * reached, -2 on write failure (connection is closed for any failure)
 */
static int open_connection(int fd)
{
	int i, on = 1;
	struct conn *ptr, *prev = NULL;

	/* We don't need/want delay; messages are always short and complete */
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, (socklen_t)sizeof(on));
	/* We also want sockets to timeout if they die and we don't notice */
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, (socklen_t)sizeof(on));

	/* attempt an initial status message write */
	if(xwrite(fd, startup_msg, strlen(startup_msg)) == -1) {
		xclose(fd);
		return(-2);
	}

	/* add it to our linked list, ensuring we don't have too many already */
	ptr = connections;
	for(i = 0; i < MAX_CONNECTIONS; i++) {
		if(!ptr || ptr->fd == -1)
			break;
		prev = ptr;
		ptr = ptr->next;
	}
	if(i >= MAX_CONNECTIONS) {
		fprintf(stderr, "max connections (%d) reached!\n", MAX_CONNECTIONS);
		xwrite(fd, max_conns, strlen(max_conns));
		xclose(fd);
		return(-1);
	}

	if(!ptr) {
		ptr = calloc(1, sizeof(struct conn));
	}
	if(!ptr->recv_buf) {
		ptr->recv_buf = calloc(BUF_SIZE, sizeof(char));
		ptr->recv_buf_pos = ptr->recv_buf;
		ptr->next = NULL;
	}
	ptr->fd = fd;
	if(prev) {
		prev->next = ptr;
	} else {
		/* this was the first one */
		connections = ptr;
	}

	return(0);
}

/**
 * End a connection by setting the file descriptor to -1 and optionally freeing
 * all buffers. This method will only attempt to close the file descriptor if
 * it is > -1. Buffers should only be freed if closing down; keeping them
 * around will save the need to continuously free and allocate memory, and they
 * are cleared no matter what.
 * @param c the connection to end
 * @param freebufs whether to free the connection buffers
 */
static void end_connection(struct conn *c, int freebufs)
{
	int fd = c->fd;
	c->fd = -1;
	if(fd > -1)
		xclose(fd);
	if(freebufs) {
		free(c->recv_buf);
		c->recv_buf = NULL;
	} else {
		memset(c->recv_buf, 0, BUF_SIZE);
	}
	c->recv_buf_pos = c->recv_buf;
	printf("connection closed\n");
}

/**
 * Cleanup all resources associated with our program, including memory,
 * open devices, files, sockets, etc. This function will not return.
 * The complete list of cleanup actions is the following:
 * - command queue (empty it)
 * - serial device (reset and close)
 * - our listeners
 * - any open connections
 * - our internal signal pipe
 * - our user command list
 * @param ret the eventual exit code for our program
 */
static void cleanup(int ret) __attribute__ ((noreturn));
static void cleanup(int ret)
{
	while(receivers) {
		struct receiver *rcvr = receivers;
		/* clear our command queue */
		while(rcvr->queue) {
			struct cmdqueue *ptr = rcvr->queue;
			free(ptr->cmd);
			rcvr->queue = ptr->next;
			free(ptr);
		}
		/* reset/close our receiver device */
		if(rcvr->fd > -1) {
			xclose(rcvr->fd);
		}
		receivers = receivers->next;
		free(rcvr);
	}

	/* close the log file descriptor */
	if(logfd > -1) {
		xclose(logfd);
		logfd = -1;
	}

	/* loop through listener descriptors and close them */
	while(listeners) {
		struct fdlist *ptr = listeners;
		if(listeners->fd > -1) {
			struct sockaddr saddr;
			socklen_t sl = (socklen_t)sizeof(struct sockaddr);
			if(getsockname(listeners->fd, &saddr, &sl)) {
				perror("getsockname()");
			} else {
				/* for unix sockets, we want to unlink the path */
				if(saddr.sa_family == AF_UNIX) {
					unlink(((struct sockaddr_un *)&saddr)->sun_path);
				}
			}
			xclose(listeners->fd);
			listeners->fd = -1;
		}
		listeners = listeners->next;
		free(ptr);
	}

	/* loop through connection descriptors and close them */
	while(connections) {
		struct conn *ptr = connections;
		end_connection(connections, 1);
		connections = connections->next;
		free(ptr);
	}

	/* close our signal listener */
	if(signalpipe[WRITE] > -1) {
		xclose(signalpipe[WRITE]);
		signalpipe[WRITE] = -1;
	}
	if(signalpipe[READ] > -1) {
		xclose(signalpipe[READ]);
		signalpipe[READ] = -1;
	}

	free_statuses();
	free_commands();
	exit(ret);
}

/**
 * Handle a signal in an async-safe fashion. The signal is written
 * to a pipe monitored in our main select() loop and will be handled
 * just as the other file descriptors there are.
 * @param signo the signal number
 */
static void pipehandler(int signo)
{
	if(signalpipe[WRITE] > -1) {
		/* write is async safe. write the signal number to the pipe. */
		xwrite(signalpipe[WRITE], &signo, sizeof(int));
	}
}

/**
 * Show the current status of our serial devices, listeners, and
 * connections. Print out the file descriptor integers for each thing
 * we are keeping an eye on.
 */
static void show_status(void)
{
	struct receiver *r;
	struct fdlist *l;
	struct conn *c;
	r = receivers;
	while(r) {
		printf("receiver      : %d (%d)\n", r->fd, r->type);
		printf("power status  : %X; main (%s)  zone2 (%s)  zone3 (%s)\n",
				r->power,
				r->power & MAIN_POWER  ? "ON" : "off",
				r->power & ZONE2_POWER ? "ON" : "off",
				r->power & ZONE3_POWER ? "ON" : "off");

		r = r->next;
	}
	printf("log file      : %d\n", logfd);

	printf("listeners     : ");
	l = listeners;
	while(l) {
		printf("%d ", l->fd);
		l = l->next;
	}

	printf("\nconnections   : ");
	c = connections;
	while(c) {
		printf("%d ", c->fd);
		c = c->next;
	}
}

/**
 * Handler for signals called when a signal was detected in our select()
 * loop. This ensures we can safely handle the signal and not have weird
 * interactions with interrupted system calls and other such fun. This
 * should never be called from within the signal handler set via sigaction.
 * @param signo the signal number
 */
static void realhandler(int signo)
{
	if(signo == SIGINT) {
		fprintf(stderr, "\ninterrupt signal received\n");
		cleanup(EXIT_SUCCESS);
	} else if(signo == SIGPIPE) {
		fprintf(stderr, "attempted IO to a closed socket/pipe\n");
	} else if(signo == SIGUSR1) {
		show_status();
	}
}

/**
 * Create and open a logfile for the raw serial device output.
 * @param path the path to the logfile
 */
static void log_raw_serial(const char *path)
{
	logfd = creat(path, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (logfd < 0) {
		perror(path);
		cleanup(EXIT_FAILURE);
	}
}

/**
 * Daemonize our program, forking and setting a new session ID. This will
 * ensure we are not associated with the terminal we are called in, allowing
 * us to be started like any other daemon program.
 */
static void daemonize(void)
{
	int pid;

	/* redirect our stdout and stderr to nowhere since we are not going to
	 * be associated with a terminal */
	fflush(NULL);
	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);

	/* pull off the amazing trickeration needed to get in the background */
	fflush(NULL);
	pid = fork();
	if (pid > 0)
		_exit(EXIT_SUCCESS);
	else if (pid < 0) {
		fprintf(stderr, "problems fork'ing for daemon!\n");
	}

	if (chdir("/") < 0) {
		fprintf(stderr, "problems changing to root directory\n");
	}

	if (setsid() < 0) {
		fprintf(stderr, "problems setsid'ing\n");
	}

	fflush(NULL);
	pid = fork();
	if (pid > 0)
		_exit(EXIT_SUCCESS);
	else if (pid < 0) {
		fprintf(stderr, "problems fork'ing for daemon!\n");
	}
}

/**
 * Open the serial device at the given path for use as a destination
 * receiver. Also adds it to our global list of serial devices.
 * @param path the path to the serial device, e.g. "/dev/ttyS0"
 * @return the serial device file descriptor
 */
static int open_serial_device(const char *path)
{
	int ret;
	struct termios newtio;
	struct receiver *rcvr = calloc(1, sizeof(struct receiver));

	/* Open serial device for reading and writing, but not as controlling
	 * TTY because we don't want to get killed if linenoise sends CTRL-C.
	 */
	rcvr->fd = xopen(path, O_RDWR | O_NOCTTY);
	if (rcvr->fd < 0)
		goto cleanup;

	memset(&newtio, 0, sizeof(struct termios));
	/* Set:
	 * B9600 - 9600 baud
	 * No flow control
	 * CS8 - 8n1 (8 bits, no parity, 1 stop bit)
	 * Don't hangup automatically
	 * CLOCAL - ignore modem status
	 * CREAD - enable receiving characters
	 */
	newtio.c_cflag = B9600 | CS8 | CLOCAL | CREAD;
	/* ignore bytes with parity errors and make terminal raw and dumb */
	newtio.c_iflag = IGNPAR;
	/* raw output mode */
	newtio.c_oflag = 0;
	/* canonical input mode- end read at a line descriptor */
	newtio.c_lflag = ICANON;
	/* add the Onkyo-used EOF char to allow canonical read */
	newtio.c_cc[VEOL] = END_RECV[strlen(END_RECV) - 1];

	/* clean the line and activate the settings */
	ret = tcflush(rcvr->fd, TCIOFLUSH);
	if(ret < 0)
		goto cleanup;

	ret = tcsetattr(rcvr->fd, TCSAFLUSH, &newtio);
	if(ret < 0)
		goto cleanup;

	/* a few more pieces of info filled in */
	rcvr->power = initial_power_status();
	/* queue up an initial power command */
	process_command(rcvr, "power");

	/* place the device in our global list */
	if(!receivers) {
		receivers = rcvr;
	} else {
		struct receiver *ptr = rcvr;
		while(ptr->next)
			ptr = ptr->next;
		ptr->next = rcvr;
	}

	return(rcvr->fd);

cleanup:
	perror(path);
	free(rcvr);
	return(-1);
}

/**
 * Attempt to listen on an open fd, and if successful, add it to our listener
 * list. This does not do any sort of socket opening call; that is left to the
 * caller.
 * @param fd the socket fd to listen on and add to list
 * @return the provided socket fd, or -1 on listen() failure
 */
static int listen_and_add(int fd)
{
	/* start listening */
	if(fd != -1 && listen(fd, 5) < 0) {
		perror("listen()");
		fd = -1;
	}

	/* add the listener to our list */
	if(fd != -1) {
		struct fdlist *ptr, *prev = NULL;
		ptr = listeners;
		while(ptr) {
			prev = ptr;
			ptr = ptr->next;
		}
		ptr = malloc(sizeof(struct fdlist));
		ptr->fd = fd;
		ptr->next = NULL;
		if(prev) {
			prev->next = ptr;
		} else {
			/* this was the first one */
			listeners = ptr;
		}
	}
	return(fd);
}

/**
 * Open a listening socket on the given bind address and port number.
 * Also add it to our global list of listeners.
 * @param host the hostname to bind to; NULL or "any" for all addresses
 * @param service the service name or port number to bind to
 * @return the new socket fd
 */
static int open_net_listener(const char * restrict host,
		const char * restrict service)
{
	int ret, fd = -1;
	struct addrinfo hints;
	struct addrinfo *result, *rp;

	/* set up our hints structure with known info */
	memset(&hints, 0, sizeof(struct addrinfo));
	/* allow IPv4 or IPv6 */
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	/* passive flag is ignored if a node is specified in getaddrinfo call */
	hints.ai_flags = AI_PASSIVE;
	hints.ai_protocol = 0;

	/* decide whether to pass a host into our getaddrinfo call. */
	if(host && (*host == '\0' || strcmp(host, "any") == 0)) {
		host = NULL;
	}
	if(!service) {
		service = LISTENPORT;
	}
	ret = getaddrinfo(host, service, &hints, &result);
	if(ret != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
		return(-1);
	}

	/* we get a list of results back, try to bind to each until one works */
	for(rp = result; rp != NULL; rp = rp->ai_next) {
		int on = 1;
		/* attempt to open the socket */
		fd = socket(result->ai_family, result->ai_socktype,
				result->ai_protocol);
		if(fd == -1)
			continue;

		/* attempt to set the ability to reuse local addresses */
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, (socklen_t)sizeof(on));

		/* attempt bind to the given address */
		if(bind(fd, result->ai_addr, rp->ai_addrlen) == 0)
			/* we found one that works! */
			break;

		/* oh noes, didn't work, keep looping */
		xclose(fd);
	}

	/* if we didn't find an available socket/bind, we are out of luck */
	if(rp == NULL) {
		fprintf(stderr, "could not bind to any available addresses\n");
		fd = -1;
	}

	/* make sure we free the result of our addr query */
	freeaddrinfo(result);

	return(listen_and_add(fd));
}

/**
 * Open a local socket at the given path.
 * Also add it to our global list of listeners.
 * @param path the path to the unix socket to create
 * @return the new socket fd
 */
static int open_socket_listener(const char *path)
{
	int fd = -1;
	struct sockaddr_un addr;

	if(strlen(path) > sizeof(addr.sun_path) - 1) {
		fprintf(stderr, "socket path too long\n");
		return(-1);
	}

	/* attempt to open the socket */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(fd == -1) {
		perror("socket()");
		return(-1);
	}

	/* attempt to bind to the socket */
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if(bind(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) != 0) {
		perror("bind()");
		return(-1);
	}

	return(listen_and_add(fd));
}

/**
 * Determine if we can send a command to the receiver by ensuring it has been
 * a certain time since the previous sent command. If we can send a command,
 * 1 is returned and timeoutval is left undefined. If we cannot send, then 0
 * is returned and the timeoutval is set accordingly.
 * @param last the last time we sent a command to the receiver
 * @param timeoutval location to store timeout before next permitted send
 * @return 1 if we can send a command, 0 if we cannot (and timeoutval is set)
 */
static int can_send_command(struct timeval * restrict last,
		struct timeval * restrict timeoutval)
{
	/* ensure it has been long enough since the last sent command */
	struct timeval now;
	time_t secs, wait_sec;
	suseconds_t usecs, wait_usec;
	gettimeofday(&now, NULL);
	/* Calculate our time difference between now and previous.
	 * Make sure we end up with an in-range usecs value. */
	secs = now.tv_sec - last->tv_sec;
	usecs = now.tv_usec - last->tv_usec;
	if(usecs < 0) {
		usecs += 1000000;
		secs -= 1;
	}
	wait_usec = 1000 * COMMAND_WAIT;
	wait_sec = wait_usec / 1000000;
	wait_usec -= wait_sec * 1000000;
	/* check if both of our difference values are > wait values */
	if(secs > wait_sec || (secs == wait_sec && usecs > wait_usec)) {
		/* it has been long enough, note that timeoutval is untouched */
		return(1);
	}
	/* it hasn't been long enough, set the timeout as necessary */
	timeoutval->tv_sec = wait_sec - secs;
	timeoutval->tv_usec = wait_usec - usecs;
	if(timeoutval->tv_usec < 0) {
		timeoutval->tv_usec += 1000000;
		timeoutval->tv_sec -= 1;
	}
	return(0);
}

/**
 * Process input from our input file descriptor and chop it into commands.
 * @param c the connection to read, write, and buffer from
 * @return 0 on success, -1 on end of (input) file, -2 on a failed write
 * to the output buffer, -3 on attempted buffer overflow
 */
static int process_input(struct conn *c)
{
	/* a convienence ptr one past the end of our buffer */
	const char * const end_pos = &(c->recv_buf[BUF_SIZE]);

	int ret = 0;
	int count;

	/*
	 * Picture time! Let's get overly verbose since we don't do this
	 * stuff too often.
	 *
	 *           ( read #1 ) ( read #2 ... )
	 * recv_buf: [v] [o] [l] [u] [m] [e] [\n] [p] [ ] ... [ ] [ ]
	 *       recv_buf_pos-----^                       end_pos-----^
	 * count: 5
	 *
	 * Basically we will perform a read, getting as many chars as are
	 * available. If we didn't get a newline on the first read, then
	 * pos will be located at the end of the previous read. If we find
	 * a newline, we will substring from start to pos (excluding newline)
	 * and pass that off to our command parser. We will then memmove
	 * anything remaining in our buffer to the beginning and continue
	 * the cycle.
	 */

	count = xread(c->fd, c->recv_buf_pos, end_pos - c->recv_buf_pos);
	if(count == 0)
		ret = -1;
	/* loop through each character we read. We are looking for newlines
	 * so we can parse out and execute one command. */
	while(count > 0) {
		if(*c->recv_buf_pos == '\n') {
			int processret;
			struct receiver *r;
			/* We have a newline. This means we should have a full command
			 * and can attempt to interpret it. */
			*c->recv_buf_pos = '\0';
			r = receivers;
			while(r) {
				processret = process_command(r, c->recv_buf);
				r = r->next;
			}
			if(processret == -1) {
				/* watch our write for a failure */
				if(xwrite(c->fd, invalid_cmd, strlen(invalid_cmd)) == -1)
					ret = -2;
			} else if(processret == -2) {
				end_connection(c, 0);
			}
			/* now move our remaining buffer to the start of our buffer */
			c->recv_buf_pos++;
			memmove(c->recv_buf, c->recv_buf_pos, count - 1);
			c->recv_buf_pos = c->recv_buf;
			memset(&(c->recv_buf_pos[count - 1]), 0,
					end_pos - &(c->recv_buf_pos[count - 1]));
			if(ret == -2)
				break;
		}
		else if(end_pos - c->recv_buf_pos <= 1) {
			/* We have a buffer overflow, we haven't seen a newline yet
			 * and we are on the last character available in our buffer.
			 * Squash whatever is in our buffer. */
			fprintf(stderr, "process_input, buffer size exceeded\n");
			c->recv_buf_pos = c->recv_buf;
			memset(c->recv_buf, 0, BUF_SIZE);
			ret = -3;
			break;
		}
		else {
			/* nothing special, just keep looking */
			c->recv_buf_pos++;
		}
		count--;
	}

	return(ret);
}

static struct timeval min_timeval(struct timeval *tv1, struct timeval *tv2)
{
	if(tv1->tv_sec < tv2->tv_sec) {
		return(*tv1);
	} else if(tv1->tv_sec > tv2->tv_sec) {
		return(*tv2);
	}
	/* getting here means seconds are equal */
	return(tv1->tv_usec < tv2->tv_usec ? *tv1 : *tv2);
}

static const struct option opts[] = {
	{"bind",      optional_argument, 0, 'b'},
	{"daemon",    no_argument,       0, 'd'},
	{"help",      no_argument,       0, 'h'},
	{"log",       required_argument, 0, 'l'},
	{"serial",    required_argument, 0, 's'},
	{"socket",    required_argument, 0, 'u'},
	{0,           0,                 0, 0  },
};

static void usage(char *argv[])
{
	printf("Usage: %s [options]\n\n", argv[0]);
	printf("Daemon to monitor and control an Onkyo A/V receiver. Options are:\n\n");
	printf("  -b, --bind [addr]      Bind and listen for incoming connections\n");
	printf("  -d, --daemon           Fork and run in background\n");
	printf("  -h, --help             Show this help\n");
	printf("  -l, --log <file>       Log raw I/O to specified file\n");
	printf("  -s, --serial <dev>     Serial device receiver is connected to\n");
	printf("  -u, --socket <file>    Listen for connections on UNIX socket\n");
	printf("\n");
	printf("By default, the daemon is dumb- it will not connect to a receiver "
			"or listen on\nany address. Command line flags must be passed to "
			"both listen and connect.\n\n");
	printf("For the -b/--bind option, the address can be specified in "
			"host:service format,\nwhere either part is optional. For example, "
			"\"localhost:8701\", \"1.2.3.4\", and\n\":12300\" are all "
			"acceptable. The default is to bind to all interfaces and use\n"
			"port 8701.\n\n");

	printf("Example:\n");
	printf("  %s -d -b -s /dev/ttyS0\n\n", argv[0]);
	printf("This will daemonize, listen on the default *:8701 address, and "
			"connect to a\nreceiver via serial at /dev/ttyS0.\n");
	printf("\n");
}

/**
 * Program main routine. Responsible for setting up all our initial monitoring
 * such as the signal pipe, serial devices, listeners, and valid commands. We
 * then enter our main event loop which does a select() on all available file
 * descriptors and takes the correct actions based on the results. This loop
 * does not end unless a SIGINT is received, which will eventually trickle
 * down and call the #cleanup() function.
 * @param argc
 * @param argv
 * @return an exit status code (although main never returns in our code)
 */
int main(int argc, char *argv[])
{
	int retval, opt;
	struct sigaction sa;
	/* options storage */
	int daemon = 0;
	char *bind_addr = NULL, *socket_path = NULL;
	char *log_path = NULL, *serialdev_path = NULL;

	/* options parsing */
	while((opt = getopt_long(argc, argv, "b::dhl:s:u:", opts, NULL))) {
		if(opt < 0)
			break;
		switch(opt) {
			case 'b':
				if(optarg)
					bind_addr = strdup(optarg);
				else
					bind_addr = strdup("");
				break;
			case 'd':
				daemon = 1;
				break;
			case 'h':
				usage(argv);
				cleanup(EXIT_SUCCESS);
				break;
			case 'l':
				log_path = strdup(optarg);
				break;
			case 's':
				serialdev_path = strdup(optarg);
				break;
			case 'u':
				socket_path = strdup(optarg);
				break;
			case '?':
				usage(argv);
				cleanup(EXIT_FAILURE);
				break;
		}
	}

	/* set up our signal handlers */
	pipe(signalpipe);
	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = &pipehandler;
	sa.sa_flags = SA_RESTART;
	sa.sa_flags = 0;
	sigfillset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	/* open the serial connection to the receiver */
	if(serialdev_path) {
		retval = open_serial_device(serialdev_path);
		free(serialdev_path);
		if(retval == -1)
			cleanup(EXIT_FAILURE);
	}

	/* init our command list */
	init_commands();
	/* init our status processing */
	init_statuses();

	/* open our listener connections */
	if(bind_addr) {
		char *pos;
		/* attempt to split our bind address into host:port */
		pos = strrchr(bind_addr, ':');
		if(pos) {
			*pos = '\0';
			pos++;
		}
		retval = open_net_listener(bind_addr, pos);
		free(bind_addr);
		if(retval == -1)
			cleanup(EXIT_FAILURE);
	}
	if(socket_path) {
		retval = open_socket_listener(socket_path);
		free(socket_path);
		if(retval == -1)
			cleanup(EXIT_FAILURE);
	}

	/* log if we have a path from options parsing */
	if(log_path) {
		log_raw_serial(log_path);
		free(log_path);
		log_path = NULL;
	}

	/* background if everything was successful */
	if(daemon) {
		daemonize();
	}

	/* Terminal settings are all done. Now it is time to watch for input
	 * on our socket and handle it as necessary. We also handle incoming
	 * status messages from the receiver.
	 *
	 * Attempt to keep the crazyness in order:
	 * signalpipe, receivers, listeners, connections
	 */
	for(;;) {
		int maxfd = -1;
		fd_set readfds, writefds;
		struct timeval timeoutval;
		struct timeval *timeout = NULL;

		struct receiver *r;
		struct fdlist *l;
		struct conn *c;

		/* get our file descriptor sets set up */
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		/* add our signal pipe file descriptor */
		FD_SET(signalpipe[READ], &readfds);
		maxfd = signalpipe[READ] > maxfd ? signalpipe[READ] : maxfd;
		/* add our receiver list */
		r = receivers;
		while(r) {
			if(r->fd > -1) {
				FD_SET(r->fd, &readfds);
				maxfd = r->fd > maxfd ? r->fd : maxfd;
				/* check for write possibility if we have commands in queue */
				if(r->queue) {
					struct timeval tv;
					if(can_send_command(&(r->last_cmd), &tv)) {
						FD_SET(r->fd, &writefds);
					} else {
						if(!timeout) {
							timeoutval = tv;
							timeout = &timeoutval;
						} else {
							/* We want the smallest timeout, so replace the
							 * existing if new is smaller. */
							timeoutval = min_timeval(timeout, &tv);
						}
					}
				}
			}
			r = r->next;
		}
		/* add all of our listeners */
		l = listeners;
		while(l) {
			if(l->fd > -1) {
				FD_SET(l->fd, &readfds);
				maxfd = l->fd > maxfd ? l->fd : maxfd;
			}
			l = l->next;
		}
		/* add all of our active connections */
		c = connections;
		while(c) {
			if(c->fd > -1) {
				FD_SET(c->fd, &readfds);
				maxfd = c->fd > maxfd ? c->fd : maxfd;
			}
			c = c->next;
		}

		/* our main waiting point */
		retval = select(maxfd + 1, &readfds, &writefds, NULL, timeout);
		if(retval == -1 && errno == EINTR)
			continue;
		if(retval == -1) {
			perror("select()");
			cleanup(EXIT_FAILURE);
		}
		/* check to see if we have signals waiting */
		if(FD_ISSET(signalpipe[READ], &readfds)) {
			int signo;
			/* We don't want to read more than one signal out of the pipe.
			 * Anything else in there will be handled the next go-around. */
			xread(signalpipe[READ], &signo, sizeof(int));
			realhandler(signo);
		}
		r = receivers;
		while(r) {
			/* check if we have a status message from the receivers */
			if(r->fd > -1 && FD_ISSET(r->fd, &readfds)) {
				size_t len;
				char *msg = process_incoming_message(r->fd, logfd);
				len = strlen(msg);
				/* print to stdout and all current open connections */
				printf("response: %s", msg);
				c = connections;
				while(c) {
					if(c->fd > -1) {
						ssize_t ret = xwrite(c->fd, msg, len);
						if(ret == -1)
							end_connection(c, 0);
					}
					c = c->next;
				}
				/* check for power messages- update our power state variable */
				r->power = update_power_status(r->power, msg);
				free(msg);
			}
			/* check if we have outgoing messages to send to receiver */
			if(r->fd > -1 && r->queue != NULL && FD_ISSET(r->fd, &writefds)) {
				rcvr_send_command(r);
			}
			r = r->next;
		}
		/* check to see if we have listeners ready to accept */
		l = listeners;
		while(l) {
			if(l->fd > -1 && FD_ISSET(l->fd, &readfds)) {
				/* accept the incoming connection on the socket */
				struct sockaddr saddr;
				socklen_t sl = (socklen_t)sizeof(struct sockaddr);
				int fd = accept(l->fd, &saddr, &sl);
				if(fd >= 0) {
					char remote[64];
					char *ptr = remote;
					switch(saddr.sa_family) {
						case AF_INET:
							inet_ntop(AF_INET, &((struct sockaddr_in *)&saddr)->sin_addr, ptr, sl);
							break;
						case AF_INET6:
							inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&saddr)->sin6_addr, ptr, sl);
							break;
						case AF_UNIX:
							/* The sun_path field will be empty since this is the remote saddr */
							ptr = "(unix socket)";
							break;
						default:
							ptr = '\0';
					}
					printf("connection opened, source: %s\n", ptr);
					open_connection(fd);
				} else if(fd == -1 && (errno != EAGAIN && errno != EINTR)) {
					perror("accept()");
				}
			}
			l = l->next;
		}
		/* check if we have connections with data ready to read */
		c = connections;
		while(c) {
			if(c->fd > -1 && FD_ISSET(c->fd, &readfds)) {
				int ret = process_input(c);
				/* ret == 0: success */
				/* ret == -1: connection hit EOF
				 * ret == -2: connection closed, failed write
				 */
				if(ret == -1 || ret == -2)
					end_connection(c, 0);
			}
			c = c->next;
		}
	}
	cleanup(EXIT_FAILURE);
}

/* vim: set ts=4 sw=4 noet: */
