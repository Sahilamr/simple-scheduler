#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <time.h>
#include "shell.h"
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include "shared_memory.h"
#include <sys/stat.h> // For fstat()
#include "scheduler.h"
#include <libgen.h>

SharedMemoryData *sharedData = NULL; // Shared data structure
size_t shared_size;

char **history;
int history_len = 0;
pid_t *pids;
time_t *start_times;
double *durations;
int ncpu;
int tslice;
int complete=0;
int arrivalTime=0;
volatile sig_atomic_t exit_requested = 0;

#define ARG_MAX_COUNT 1024
#define MAX_BACKGROUND_PROCESSES 100
#define HISTORY_MAXITEMS 100

typedef struct {
    pid_t pid;
    char *cmd;
} BackgroundProcess;

BackgroundProcess background_processes[MAX_BACKGROUND_PROCESSES];
int bg_process_count = 0;

// Function prototypes
void init_shared_memory(SharedMemoryData **sharedData, size_t *shared_size);
void enqueue(SharedMemoryData *sharedData, size_t shared_size, const char *name, int priority);
void print_shared_memory(SharedMemoryData *sharedData, size_t shared_size);
void init_history();
// void clean_shared_memory(SharedMemoryData *sharedData, size_t shared_size);
void add_to_history(char *cmd, pid_t pid, double duration);
void print_history();
void initialize_process_schedule();
void print_job_details();
void check_background_processes();
void launch_command(char *cmd, size_t shared_size);
void execute_single_command(char *cmd, size_t shared_size);
void execute_piped_commands(char *cmd_parts[], int num_parts);
int is_blank(char *input);
int handle_builtin(char *input);

void init_shared_memory(SharedMemoryData **sharedData, size_t *shared_size) {
    // const char* shm_name = "/process_queue";
    int shm_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        exit(1);
    }

    size_t max_processes = 10; // Adjust as needed
    size_t shm_size = max_processes * sizeof(SharedMemoryData);
    *shared_size = max_processes;

    if (ftruncate(shm_fd, shm_size) == -1) {
        perror("ftruncate");
        close(shm_fd);
        exit(1);
    }

    *sharedData = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (*sharedData == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        exit(1);
    }

    close(shm_fd);
}

void cleanup() {
    // Clear the ProcessInfo structures in the table
    if (sharedData != NULL) {
        // Clear the entire table by setting it to zero
        memset(sharedData->table, 0, sizeof(sharedData->table));
        
        // Alternatively, you can free any other allocated resources if needed

        // Unmapping the shared memory
        if (munmap(sharedData, shared_size * sizeof(SharedMemoryData)) == -1) {
            perror("munmap");
            exit(1);
        }
    }

    // Unlinking the shared memory object
    if (shm_unlink(SHARED_MEM_NAME) == -1) {
        perror("shm_unlink");
        exit(1);
    }
    printf("Cleanup completed, shared memory cleared.\n");
}
void enqueue(SharedMemoryData *sharedData, size_t shared_size, const char *name, int priority) {
    for (size_t i = 0; i < shared_size; i++) {
        if (sharedData->table[i].pid == 0) {  // Check if the slot is free
            // Set up the process information
            strncpy(sharedData->table[i].executableName, name, sizeof(sharedData->table[i].executableName) - 1);
            sharedData->table[i].priority = priority;
            sharedData->table[i].pid = getpid();  // Mark as queued
            sharedData->table[i].isRunning = false;
            sharedData->table[i].remaining_time=sharedData->TSLICE;
            sharedData->table[i].completion_time = sharedData->TSLICE; // or another appropriate value
            sharedData->readyQueue.queue[sharedData->readyQueue.submittedProcess] = sharedData->table[i];
            sharedData->readyQueue.submittedProcess++;

            printf("Command added to shared memory: %s with priority %d\n", name, priority);
            break;  // Exit loop after enqueuing
        }
    }
}



void print_shared_memory(SharedMemoryData *sharedData, size_t shared_size) {
    printf("Current processes in shared memory:\n");
    for (size_t i = 0; i < shared_size; i++) {
        if (sharedData[i].pid != 0) { // If PID is set
            printf("Executable: %s, Priority: %d, PID: %d, Running: %s\n",
                   sharedData[i].executableName,
                   sharedData[i].priority,
                   sharedData[i].pid,
                   sharedData[i].isRunning ? "Yes" : "No");
        }
    }
}

void init_history() {
    history = calloc(HISTORY_MAXITEMS, sizeof(char *));
    pids = calloc(HISTORY_MAXITEMS, sizeof(pid_t));
    start_times = calloc(HISTORY_MAXITEMS, sizeof(time_t));
    durations = calloc(HISTORY_MAXITEMS, sizeof(double));
    if (!history || !pids || !start_times || !durations) {
        fprintf(stderr, "error: memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
}

void add_to_history(char *cmd, pid_t pid, double duration) {
    char *line = strdup(cmd);
    if (line == NULL) return;

    if (history_len == HISTORY_MAXITEMS) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (HISTORY_MAXITEMS - 1));
        memmove(pids, pids + 1, sizeof(pid_t) * (HISTORY_MAXITEMS - 1));
        memmove(start_times, start_times + 1, sizeof(time_t) * (HISTORY_MAXITEMS - 1));
        memmove(durations, durations + 1, sizeof(double) * (HISTORY_MAXITEMS - 1));
        history_len--;
    }

    history[history_len] = line;
    pids[history_len] = pid;
    start_times[history_len] = time(NULL);
    durations[history_len] = duration;
    history_len++;
}

void print_history() {
    for (int i = 0; i < history_len; i++) {
        printf("%d %s (pid: %d, duration: %.2f seconds)\n", i, history[i], pids[i], durations[i]);
    }
}

void check_background_processes() {
    for (int i = 0; i < bg_process_count; i++) {
        int status;
        pid_t result = waitpid(background_processes[i].pid, &status, WNOHANG);
        
        if (result == background_processes[i].pid) {
            printf("[Background] PID: %d finished command: %s\n", background_processes[i].pid, background_processes[i].cmd);
            free(background_processes[i].cmd); // Free command string
            // Shift remaining background processes
            for (int j = i; j < bg_process_count - 1; j++) {
                background_processes[j] = background_processes[j + 1];
            }
            bg_process_count--;
            i--; // Adjust index since we shifted
        }
    }
}

void launch_command(char *cmd, size_t shared_size) {
    // Check for pipes in the command
    char *cmd_part = strtok(cmd, "|");
    char *cmd_parts[ARG_MAX_COUNT];
    int num_parts = 0;

    while (cmd_part != NULL) {
        cmd_parts[num_parts++] = cmd_part;
        cmd_part = strtok(NULL, "|");
    }

    // If there's no pipe, just execute the command normally
    if (num_parts == 1) {
        execute_single_command(cmd_parts[0], shared_size);
    } else {
        execute_piped_commands(cmd_parts, num_parts);
    }
    // Check for completed background processes
    check_background_processes();
}


void execute_single_command(char *cmd, size_t shared_size) {
    char *args[ARG_MAX_COUNT];
    int tokenCount = 0;
    int background = 0;
    

    if (strncmp(cmd, "submit", 6) == 0) {
        char *executable_path = strtok(cmd + 7, " ");
        int priority = 1;  // Default priority
        char *priority_arg = strtok(NULL, " ");

        if (priority_arg != NULL) {
            priority = atoi(priority_arg);
        }

        if (executable_path) {
            // Get the executable name only (strip any leading directory components)
            char *executable_name = basename(executable_path);

            // Debug output to check what we are passing to access
            printf("Checking executable: %s\n", executable_name);

            if (access(executable_name, X_OK) == 0) { // Check if the file is executable
                enqueue(sharedData, shared_size, executable_path, priority);
                initialize_process_schedule();
                sharedData->readyQueue.readyQueueSize++;
                sharedData->readyQueue.queue[complete++].arrival_time=arrivalTime;
                printf("%d",arrivalTime);
                arrivalTime++;
                
               
                printf("Submitted command '%s' with priority %d\n \n", executable_name, priority);
            } else {
                fprintf(stderr, "Error: Executable '%s' does not exist or is not accessible.\n  \n", executable_name);
            }
        } else {
            fprintf(stderr, "Error: No executable specified for submit command.\n  \n");
        }
        return;
    }

    // Further command processing (non-submit commands)
    char *token = strtok(cmd, " ");
    while (token != NULL) {
        args[tokenCount++] = token;
        token = strtok(NULL, " ");
    }

    if (tokenCount > 0 && strcmp(args[tokenCount - 1], "&") == 0) {
        background = 1;
        args[tokenCount - 1] = NULL;
    } else {
        args[tokenCount] = NULL;
    }

    pid_t pid = fork();
    if (pid == 0) {
        execvp(args[0], args);
        perror("Execution failed");
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        if (background) {
            background_processes[bg_process_count].pid = pid;
            background_processes[bg_process_count].cmd = strdup(cmd);
            bg_process_count++;
            printf("[Background] Launched process %d: %s\n", pid, cmd);
        } else {
            int status;
            waitpid(pid, &status, 0);
            double duration = difftime(time(NULL), start_times[history_len - 1]);
            add_to_history(cmd, pid, duration);
            printf("Process %d finished: %s\n", pid, cmd);
        }
    } else {
        perror("Fork failed");
    }
}


void execute_piped_commands(char *cmd_parts[], int num_parts) {
    int pipe_fds[2 * (num_parts - 1)];
    for (int i = 0; i < num_parts - 1; i++) {
        if (pipe(pipe_fds + i * 2) < 0) {
            perror("Pipe creation failed");
            return;
        }
    }

    for (int i = 0; i < num_parts; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            if (i > 0) {
                dup2(pipe_fds[(i - 1) * 2], STDIN_FILENO);
            }
            if (i < num_parts - 1) {
                dup2(pipe_fds[i * 2 + 1], STDOUT_FILENO);
            }
            for (int j = 0; j < 2 * (num_parts - 1); j++) {
                close(pipe_fds[j]);
            }
            char *args[ARG_MAX_COUNT];
            int tokenCount = 0;

            char *token = strtok(cmd_parts[i], " ");
            while (token != NULL) {
                args[tokenCount++] = token;
                token = strtok(NULL, " ");
            }
            args[tokenCount] = NULL; // Null terminate the args

            execvp(args[0], args);
            perror("Execution failed");
            exit(EXIT_FAILURE);
        } else if (pid < 0) {
            perror("Fork failed");
        }
    }

    for (int i = 0; i < 2 * (num_parts - 1); i++) {
        close(pipe_fds[i]);
    }

    for (int i = 0; i < num_parts; i++) {
        wait(NULL);
    }
}

int is_blank(char *input) {
    while (*input) {
        if (!isspace(*input)) {
            return 0; // Not blank
        }
        input++;
    }
    return 1; // Blank
}
int handle_builtin(char *input) {
    if (strcmp(input, "exit") == 0) {
 
        free(history);
        free(pids);
        free(start_times);
        free(durations);
        cleanup();
        exit(0);
    }
    if (strcmp(input, "history") == 0) {
        print_history();
        return 1;
    }
    if (strcmp(input, "clean") == 0) { // New command to clean shared memory
        cleanup();
        return 1;
    }
    return 0;
}
void sigint_handler(int signo) {
    if (signo == SIGINT) {
        printf("\n  \nReceived SIGINT signal. Starting scheduler...\n \n");
        
        start_scheduler(sharedData, shared_size, ncpu, tslice);// Use the global shared_size
        print_job_details();
   
        // sleep(10);
        // signal(SIGINT,SIG_IGN);
    }
}
// Main shell loop
int main(int argc, char *argv[]) {
    printf("inside shell\n");
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ncpu> <tslice>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Parse command line arguments
    ncpu = atoi(argv[1]);
    tslice = atoi(argv[2]);

    if (ncpu <= 0 || tslice <= 0) {
        fprintf(stderr, "Error: ncpu and tslice must be positive integers.\n");
        return EXIT_FAILURE;
    }

    // Set up SIGINT handler for clean shutdown
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // Initialize shared memory
    init_shared_memory(&sharedData, &shared_size);
    sharedData->NCPU=ncpu;
    sharedData->TSLICE=tslice;
    // Fork a new process to run the scheduler
    pid_t pid = fork();
    if (pid < 0) {
        perror("Fork failed");
        return EXIT_FAILURE;
    } else if (pid == 0) {
        // Child process: Run the scheduler
    
        initialize_process_schedule();
        exit(EXIT_SUCCESS); // Ensure child process exits after scheduler
    } else {
        // Parent process: Run the shell
        init_history();

        char input[1024];
        while (!exit_requested) {
            printf("shell> ");
            if (fgets(input, sizeof(input), stdin) == NULL) break;
            input[strcspn(input, "\n")] = 0; // Remove newline

            if (is_blank(input)) continue;
            if (handle_builtin(input)) break;

            launch_command(input, shared_size); // Send command to scheduler
        }

        // Wait for the scheduler to finish (optional, if you want synchronous exit)
        waitpid(pid, NULL, 0);

        // Clean up
        print_job_details();
        cleanup();
        printf("Exiting shell\n");
    }

    return 0;
}