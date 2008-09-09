/* This file contains a list of defines and function defs shared by files
 * in our mini application. */

/* the serial port to connect on */
#define SERIALDEVICE "/dev/ttyS1"

/* characters standard to the start and end of our communication messages */
#define START_SEND "!1"
#define END_SEND "\n"
#define START_RECV "!1"
#define END_RECV ""
#define END_RECV_CHAR 0x1A

/* size to use for all static buffers */
#define BUF_SIZE 64

/* onkyo.c - receiver control functions */
int rcvr_send_command(const char *cmd, char **status);
int rcvr_handle_status(char **status);

/* command.c - user command init/teardown */
void init_commands(void);
void free_commands(void);

/* command.c - command and status processing */
int process_status(int fd);
int process_command(int fd, const char *str);
