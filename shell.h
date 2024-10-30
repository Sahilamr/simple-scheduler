#ifndef SHELL_H
#define SHELL_H

#include <time.h>

/* Constants */
#define ARG_MAX_COUNT    1024      /* max number of arguments to a command */
#define HISTORY_MAXITEMS 100       /* max number of elements in the history */

/* Type declarations */
struct command {                  
	int argc;                  /* number of arguments in the command */
	char *name;                /* name of the command */
	char *argv[ARG_MAX_COUNT]; /* the arguments themselves */
};

struct commands {                  
	int cmd_count;             /* number of commands in the pipeline */
	struct command *cmds[];    /* the commands themselves */
};

/* Global variables */
extern char **history;           /* Array to store history of commands */
extern int history_len;          /* Length of the command history */
extern pid_t *pids;              /* Array to store process IDs of commands */
extern time_t *start_times;      /* Array to store start times of commands */
extern double *durations;        /* Array to store execution durations of commands */

/* Function Prototypes */

/* Initializes the history storage */
void init_history(void);                                

/* Adds a command to the history along with its PID and execution duration */
void add_to_history(char *cmd, pid_t pid, double duration);

/* Prints the command history with associated PIDs and durations */
void print_history(void);

/* Launches an external command by forking and executing */
void launch_command(char *cmd,size_t shared_size);

/* Checks if a string input is blank (contains only whitespace) */
int is_blank(char *input);

/* Handles built-in commands such as 'exit', 'history', and 'cd'. 
 * Returns -1 for 'exit', 0 for a successful built-in command execution, 
 * and 1 if the input is not a built-in command.
 */
int handle_builtin(char *input);

void execute_single_command(char *cmd,size_t shared_size);
void execute_piped_commands(char *cmd_parts[], int num_parts);
#endif /* SHELL_H */
