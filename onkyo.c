/*
 *  onkyo.c - Onkyo receiver communication program
 *
 *  Copyright (c) 2008 Dan McGee <dpmcgee@gmail.com>
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
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h> /* chdir, fork, pipei, setsid */
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>
#include <time.h>

#include "onkyo.h"

/* an enum useful for keeping track of two paired file descriptors */
enum pipehalfs { READ = 0, WRITE = 1 };

typedef struct _conn {
	int fd;
	char *recv_buf;
	char *recv_buf_pos;
} conn;

typedef struct _cmdqueue {
	unsigned long hash;
	char *cmd;
	struct _cmdqueue *next;
} cmdqueue;

/* our serial device and associated dealings */
static int serialdev;
static struct termios serialdev_oldtio;
static cmdqueue *serialdev_cmdqueue;
/** our list of listening sockets/descriptors we accept connections on */
static int listeners[MAX_LISTENERS];
/** our list of open connections we process commands on */
static conn connections[MAX_CONNECTIONS];
/** pipe used for async-safe signal handling in our select */
static int signalpipe[2] = { -1, -1 };

/** power status of receiver */
unsigned int serialdev_power;

/* common messages */
const char * const startup_msg = "OK:onkyocontrol v0.1\n";
const char * const invalid_cmd = "ERROR:Invalid Command\n";
const char * const rcvr_err = "ERROR:Receiver Error\n";

/* forward function declarations */
static void cleanup(int ret) __attribute__ ((noreturn));
static void pipehandler(int signo);
static void realhandler(int signo);
static int open_serial_device(const char *path);
static int open_listener(const char *host, const char *service);
static int open_connection(int fd);
static void end_connection(conn *c);
static int process_input(conn *c);
static void show_status(void);


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
static void cleanup(int ret)
{
	int i;

	/* clear our command queue */
	while(serialdev_cmdqueue) {
		cmdqueue *ptr = serialdev_cmdqueue;
		free(serialdev_cmdqueue->cmd);
		serialdev_cmdqueue = serialdev_cmdqueue->next;
		free(ptr);
	}

	/* reset/close our serial device */
	if(serialdev > -1) {
		/* just ignore possible errors here, can't do anything */
		tcsetattr(serialdev, TCSANOW, &(serialdev_oldtio));
		xclose(serialdev);
		serialdev = -1;
	}

	/* loop through listener descriptors and close them */
	for(i = 0; i < MAX_LISTENERS; i++) {
		if(listeners[i] > -1) {
			xclose(listeners[i]);
			listeners[i] = -1;
		}
	}

	/* loop through connection descriptors and close them */
	for(i = 0; i < MAX_CONNECTIONS; i++) {
		if(connections[i].fd > -1) {
			end_connection(&connections[i]);
		}
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
	int fd, ret;
	struct termios newtio;

	/* reset/close any existing serial device */
	if(serialdev > -1) {
		/* just ignore possible errors here, can't do anything */
		tcsetattr(serialdev, TCSANOW, &(serialdev_oldtio));
		xclose(serialdev);
		serialdev = -1;
	}

	/* Open serial device for reading and writing, but not as controlling
	 * TTY because we don't want to get killed if linenoise sends CTRL-C.
	 */
	fd = xopen(path, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror(path);
		return(-1);
	}

	/* save current serial port settings */
	ret = tcgetattr(fd, &serialdev_oldtio);
	if (ret < 0) {
		perror(path);
		return(-1);
	}

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
	ret = tcflush(fd, TCIOFLUSH);
	if(ret < 0) {
		perror(path);
		return(-1);
	}
	ret = tcsetattr(fd, TCSAFLUSH, &newtio);
	if(ret < 0) {
		perror(path);
		return(-1);
	}

	/* place the device in our global fd */
	serialdev = fd;
	return(fd);
}

#if !HAVE_GETADDRINFO
/**
 * Open a listening socket on the given bind address and port number.
 * Also add it to our global list of listeners.
 * @param host the hostname to bind to; NULL or "any" for all addresses
 * @param service the service name or port number to bind to
 * @return the new socket fd
 */
static int open_listener(const char *host, const char *service)
{
	int i, fd;
	unsigned short port;
	long portval;
	char *pos;
	struct sockaddr_in sin;
	const int trueval = 1;

	/* make sure we have room in our list */
	for(i = 0; i < MAX_LISTENERS && listeners[i] > -1; i++)
		/* no body, find first available spot */;
	if(i == MAX_LISTENERS) {
		fprintf(stderr, "max listeners reached!\n");
		return(-1);
	}

	/* locate our service if it is not a number */
	errno = 0;
	portval = strtol(service, &pos, 10);
	if(errno != 0) {
		struct servent *serv;
		/* not a number, try to parse as a service */
		serv = getservbyname(service, NULL);
		if(serv) {
			/* port is in network byte order already */
			port = serv->s_port;
		} else {
			fprintf(stderr, "service/port could not be parsed\n");
			return(-1);
		}
	} else {
		port = htons((short)portval);
	}

	/* open an inet socket with the default protocol */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror(host);
		return(-1);
	}
	/* set up our connection host, port, etc. */
	memset(&sin, 0, sizeof(struct sockaddr_in));
	sin.sin_family = AF_INET;
	sin.sin_port = port;
	/* determine our resolved host address */
	if(!host || strcmp(host, "any") == 0) {
		sin.sin_addr.s_addr = INADDR_ANY;
	} else {
		struct hostent *he;
		if(!(he = gethostbyname(host))) {
			perror(host);
			return(-1);
		}
		/* assumption- using IPv4, he->h_addrtype = AF_INET */
		memcpy((char *)&sin.sin_addr.s_addr,
				(char *)he->h_addr_list[0], he->h_length);
	}
	/* set the ability to reuse local addresses */
	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&trueval,
				sizeof(int)) < 0) {
		perror("setsockopt()");
		return(-1);
	}
	/* bind to the given address */
	if(bind(fd, (struct sockaddr *)&sin, sizeof(struct sockaddr)) < 0) {
		perror("bind()");
		return(-1);
	}
	/* start listening */
	if(listen(fd, 5) < 0) {
		perror("listen()");
		return(-1);
	}

	/* place the listener in our array */
	listeners[i] = fd;
	return(fd);
}
#else
/**
 * Open a listening socket on the given bind address and port number.
 * Also add it to our global list of listeners.
 * @param host the hostname to bind to; NULL or "any" for all addresses
 * @param service the service name or port number to bind to
 * @return the new socket fd
 */
static int open_listener(const char *host, const char *service)
{
	int i, ret, fd = -1;
	struct addrinfo hints;
	struct addrinfo *result, *rp;

	/* make sure we have room in our list */
	for(i = 0; i < MAX_LISTENERS && listeners[i] > -1; i++)
		/* no body, find first available spot */;
	if(i == MAX_LISTENERS) {
		fprintf(stderr, "max listeners reached!\n");
		return(-1);
	}

	/* set up our hints structure with known info */
	memset(&hints, 0, sizeof(struct addrinfo));
	/* allow IPv4 or IPv6 */
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	/* passive flag is ignored if a node is specified in getaddrinfo call */
	hints.ai_flags = AI_PASSIVE;
	hints.ai_protocol = 0;

	/* decide whether to pass a host into our getaddrinfo call. */
	if(!host || strcmp(host, "any") == 0) {
		ret = getaddrinfo(NULL, service, &hints, &result);
	} else {
		ret = getaddrinfo(host, service, &hints, &result);
	}
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
		if (fd == -1)
			continue;

		/* attempt to set the ability to reuse local addresses */
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

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

	/* start listening */
	if(fd != -1 && listen(fd, 5) < 0) {
		perror("listen()");
		fd = -1;
	}

	/* place the listener in our array */
	if(fd != -1) {
		listeners[i] = fd;
	}
	return(fd);
}
#endif /* !HAVE_GETADDRINFO */

/**
 * Establish everything we need for a connection once it has been
 * accepted. This will set up send and receive buffers and start
 * tracking the connection in our array.
 * @param fd the newly opened connections file descriptor
 * @return 0 if initial write was successful, -1 if max connections
 * reached, -2 on write failure (connection is closed for any failure)
 */
static int open_connection(int fd)
{
	int i;
	/* add it to our list */
	for(i = 0; i < MAX_CONNECTIONS && connections[i].fd > -1; i++)
		/* no body, find first available spot */;
	if(i == MAX_CONNECTIONS) {
		fprintf(stderr, "max connections reached!\n");
		xclose(fd);
		return(-1);
	}

	/* attempt an initial status message write */
	if(xwrite(fd, startup_msg, strlen(startup_msg)) == -1) {
		xclose(fd);
		return(-2);
	}

	connections[i].fd = fd;
	connections[i].recv_buf = calloc(BUF_SIZE, sizeof(char));
	connections[i].recv_buf_pos = connections[i].recv_buf;

	return(0);
}

/**
 * End a connection by setting the file descriptor to -1 and freeing
 * all buffers. This method will not check the file descriptor value first
 * to make sure it is valid.
 * @param c the connection to end
 */
static void end_connection(conn *c)
{
	xclose(c->fd);
	c->fd = -1;
	free(c->recv_buf);
	c->recv_buf = NULL;
	c->recv_buf_pos = NULL;
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
static int can_send_command(struct timeval *last, struct timeval *timeoutval)
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
static int process_input(conn *c)
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
			/* We have a newline. This means we should have a full command
			 * and can attempt to interpret it. */
			*c->recv_buf_pos = '\0';
			processret = process_command(c->recv_buf);
			if(processret == -1) {
				/* watch our write for a failure */
				if(xwrite(c->fd, invalid_cmd, strlen(invalid_cmd)) == -1)
					ret = -2;
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

/**
 * Queue a receiver command to be sent when the serial device file descriptor
 * is available for writing. Queueing and sending asynchronously allows
 * the program to backlog many commands at once without blocking on the
 * relatively slow serial device. When queueing, we check if this command
 * is already in the queue- if so, we do not queue it again.
 * @param cmd command to queue, will be freed once it is actually ran
 * @return 0 on queueing success, 1 on queueing skip
 */
int queue_rcvr_command(char *cmd)
{
	cmdqueue *q = malloc(sizeof(cmdqueue));
	q->hash = hash_sdbm(cmd);
	q->cmd = cmd;
	q->next = NULL;

	if(serialdev_cmdqueue == NULL) {
		serialdev_cmdqueue = q;
	} else {
		cmdqueue *ptr = serialdev_cmdqueue;
		for(;;) {
			if(ptr->hash == q->hash) {
				/* command already in our queue, skip second copy */
				free(q);
				free(cmd);
				return(1);
			}
			if(!ptr->next)
				break;
			ptr = ptr->next;
		}
		ptr->next = q;
	}
	return(0);
}

/**
 * Show the current status of our serial devices, listeners, and
 * connections. Print out the file descriptor integers for each thing
 * we are keeping an eye on.
 */
static void show_status(void)
{
	int i;
	printf("serial devices : ");
	printf("%d ", serialdev);
	printf("\nlisteners      : ");
	for(i = 0; i < MAX_LISTENERS; i++) {
		printf("%d ", listeners[i]);
	}
	printf("\nconnections    : ");
	for(i = 0; i < MAX_CONNECTIONS; i++) {
		printf("%d ", connections[i].fd);
	}
	printf("\npower status   : %X", serialdev_power);
	printf("\n  main         : %s", serialdev_power & MAIN_POWER ? "on" : "off");
	printf("\n  zone2        : %s", serialdev_power & ZONE2_POWER ? "on" : "off");
	printf("\n  zone3        : %s", serialdev_power & ZONE3_POWER ? "on" : "off");
	printf("\nreceiver status:\n");
	process_command("status");
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
	int i, retval;
	struct sigaction sa;
	fd_set readfds, writefds;
	struct timeval serialdev_last = { 0, 0 };

	serialdev_power = initial_power_status();

	/* set our file descriptor arrays to -1 */
	serialdev = -1;
	for(i = 0; i < MAX_LISTENERS; i++)
		listeners[i] = -1;
	for(i = 0; i < MAX_CONNECTIONS; i++)
		connections[i].fd = -1;

	/* daemonize if requested */
	if(argc > 1 && strcmp(argv[1], "--daemon") == 0)
		daemonize();

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
	retval = open_serial_device(SERIALDEVICE);
	if(retval == -1)
		cleanup(EXIT_FAILURE);

	/* init our command list */
	init_commands();
	/* init our status processing */
	init_statuses();

	/* open our listener connection */
	retval = open_listener(LISTENHOST, LISTENPORT);
	if(retval == -1)
		cleanup(EXIT_FAILURE);

	/* Terminal settings are all done. Now it is time to watch for input
	 * on our socket and handle it as necessary. We also handle incoming
	 * status messages from the receiver.
	 *
	 * Attempt to keep the crazyness in order:
	 * signalpipe, serialdev, listeners, connections
	 */
	for(;;) {
		int maxfd = -1;
		struct timeval timeoutval;
		struct timeval *timeout = NULL;

		/* get our file descriptor sets set up */
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		/* add our signal pipe file descriptor */
		FD_SET(signalpipe[READ], &readfds);
		maxfd = signalpipe[READ] > maxfd ? signalpipe[READ] : maxfd;
		/* add our serial device */
		if(serialdev > -1) {
			FD_SET(serialdev, &readfds);
			maxfd = serialdev > maxfd ? serialdev : maxfd;
			/* check for write possibility if we have commands in queue */
			if(serialdev_cmdqueue) {
				if(can_send_command(&serialdev_last, &timeoutval)) {
					FD_SET(serialdev, &writefds);
				} else {
					timeout = &timeoutval;
				}
			}
		}
		/* add all of our listeners */
		for(i = 0; i < MAX_LISTENERS; i++) {
			if(listeners[i] > -1) {
				FD_SET(listeners[i], &readfds);
				maxfd = listeners[i] > maxfd ? listeners[i] : maxfd;
			}
		}
		/* add all of our active connections */
		for(i = 0; i < MAX_CONNECTIONS; i++) {
			if(connections[i].fd > -1) {
				FD_SET(connections[i].fd, &readfds);
				maxfd = connections[i].fd > maxfd ? connections[i].fd : maxfd;
			}
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
		/* check if we have a status message on our serial device */
		if(serialdev > -1 && FD_ISSET(serialdev, &readfds)) {
			int len;
			char *msg = process_incoming_message(serialdev);
			len = strlen(msg);
			/* print to stdout and all current open connections */
			printf("%s", msg);
			for(i = 0; i < MAX_CONNECTIONS; i++) {
				if(connections[i].fd > -1) {
					int ret = xwrite(connections[i].fd, msg, len);
					if(ret == -1)
						end_connection(&connections[i]);
				}
			}
			/* check for power messages- update our power state variable */
			serialdev_power = update_power_status(serialdev_power, msg);
			free(msg);
		}
		/* check if we have outgoing messages to send to receiver */
		if(serialdev > -1 && serialdev_cmdqueue != NULL
				&& FD_ISSET(serialdev, &writefds)) {
			cmdqueue *ptr;
			/* Determine whether we should send the command. This depends
			 * on two factors:
			 * 1. If the power is on, always send the command.
			 * 2. If the power is off, send only power commands through.
			 */
			if(serialdev_power ||
					is_power_command(serialdev_cmdqueue->cmd)) {
				int ret = rcvr_send_command(serialdev,
						serialdev_cmdqueue->cmd);
				if(ret != 0) {
					printf("%s", rcvr_err);
				}
				/* set our last sent time */
				gettimeofday(&serialdev_last, NULL);
			}

			/* dequeue the cmd queue item */
			ptr = serialdev_cmdqueue;
			serialdev_cmdqueue = serialdev_cmdqueue->next;
			free(ptr->cmd);
			free(ptr);
		}
		/* check to see if we have listeners ready to accept */
		for(i = 0; i < MAX_LISTENERS; i++) {
			if(listeners[i] > -1
					&& FD_ISSET(listeners[i], &readfds)) {
				/* accept the incoming connection on the socket */
				struct sockaddr saddr;
				socklen_t sl = sizeof(struct sockaddr);
				int fd = accept(listeners[i], &saddr, &sl);
				if(fd >= 0) {
					open_connection(fd);
				} else if(fd == -1 && (errno != EAGAIN && errno != EINTR)) {
					perror("accept()");
				}
			}
		}
		/* check if we have connections with data ready to read */
		for(i = 0; i < MAX_CONNECTIONS; i++) {
			if(connections[i].fd > -1
					&& FD_ISSET(connections[i].fd, &readfds)) {
				int ret = process_input(&connections[i]);
				/* ret == 0: success */
				/* ret == -1: connection hit EOF
				 * ret == -2: connection closed, failed write
				 */
				if(ret == -1 || ret == -2)
					end_connection(&connections[i]);
			}
		}
	}
}

/* vim: set ts=4 sw=4 noet: */
