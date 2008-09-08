
/* This file contains a list of defines and function defs shared by files
 * in our mini application. */

/* the serial port to connect on */
#define SERIALDEVICE "/dev/ttyS1"

/* TODO not sure if we will need these */
#define END_SEND '\r'
#define END_RECV 0x1A

/* size to use for all static buffers */
#define BUF_SIZE 256

/* receiver control functions */
int rcvr_send_command(const char *cmd, char **status);
int rcvr_handle_status(char **status);

/* user command init/teardown */
void init_commands(void);
void free_commands(void);

/* command and status processing */
int process_status(int fd);
int process_command(int fd, const char *str);
