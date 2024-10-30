#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include "shared_memory.h"

// Global variables for shared data and configuration
extern SharedMemoryData *sharedData;
extern size_t shared_size;
volatile sig_atomic_t running = 1;
int NCPU; // Number of CPUs
int TSLICE; // Time slice in milliseconds
int completedProcesses = 0; // Track the number of successfully completed processes

// Function declarations
void handle_child_termination(int sig);
void print_job_details();
void add_to_ready_queue(size_t index);
size_t get_from_ready_queue();
void initialize_process_schedule();
void print_submitted_processes();
void start_scheduler(SharedMemoryData *data, size_t size, int ncpu, int t_slice);

// Function to add a process index to the ready queue
void add_to_ready_queue(size_t index) {
    if (sharedData->readyQueue.readyQueueSize < MAX_PROCESSES) {
        sharedData->readyQueue.queue[sharedData->readyQueue.readyQueueSize].index = index;
        sharedData->readyQueue.readyQueueSize++;
    } else {
        fprintf(stderr, "Ready queue is full. Cannot add more processes.\n");
    }
}

// Function to get a process index from the ready queue
size_t get_from_ready_queue() {
    if (sharedData->readyQueue.readyQueueSize == 0) return (size_t)-1;

    size_t index = sharedData->readyQueue.queue[0].index;

    // Shift all processes in the ready queue one position forward
    for (int i = 1; i < sharedData->readyQueue.readyQueueSize; i++) {
        sharedData->readyQueue.queue[i - 1] = sharedData->readyQueue.queue[i];
    }
    sharedData->readyQueue.readyQueueSize--;

    return index;
}

// Initialize the ready queue with NCPU processes initially
void initialize_process_schedule() {
    int count = 0;
    for (size_t i = 0; i < shared_size && count < NCPU; i++) {
        if (sharedData->table[i].pid > 0) {  // Valid process
            add_to_ready_queue(i);

            count++;
            if(sharedData->table[i].isRunning==false){
                sharedData->table[i].pid=-1;
            }
        }
    }
    sharedData->readyQueue.submittedProcess=count;
}



// Signal handler for child processes
void signal_handler(int signum) {
    if (signum == SIGSTOP) {
        // Child process should handle stopping logic if needed
        printf("Process %d paused.\n", getpid());
        pause();  // Wait for signals (e.g., SIGCONT) to continue
    } else if (signum == SIGCONT) {
        // Child process resumes execution
        printf("Process %d resumed.\n", getpid());
    }
}

void start_scheduler(SharedMemoryData *data, size_t size, int ncpu, int t_slice) {
    printf("Starting round-robin scheduler...\n");

    sharedData = data;
    shared_size = size;
    NCPU = ncpu;
    TSLICE = t_slice;

    initialize_process_schedule();

    printf("Ready Queue Size after initialization: %d\n", sharedData->readyQueue.readyQueueSize);

    while (completedProcesses < sharedData->readyQueue.submittedProcess) {
        int anyProcessScheduled = 0;
        int status;  // Declare the status variable here

        for (int i = 0; i < NCPU; i++) {
            size_t index = get_from_ready_queue();
            if (sharedData->readyQueue.readyQueueSize == 0) {
                printf("No processes in the ready queue.\n");
                break;
            }

            if (index >= shared_size) {
                fprintf(stderr, "Invalid index: %zu\n", index);
                continue;
            }

            ProcessInfo *process = &sharedData->table[index];

            // Check if process needs to be started or resumed
            if (process->pid == -1) {  // Only fork if the process has not started
                process->pid = getpid();
                process->isRunning = true;
                printf("Forking for process: %s\n", process->executableName);
                pid_t pid = fork();

                if (pid < 0) {
                    perror("Fork failed");
                    continue;
                }

                if (pid == 0) {  // Child process
                    // Set up signal handler
                    signal(SIGSTOP, signal_handler);
                    signal(SIGCONT, signal_handler);
                    
                    process->start_time = time(NULL);
                    char *args[2];  // Adjust the size based on the number of arguments
                    args[0] = process->executableName; // First argument is the program name
                    args[1] = NULL; // Null-terminated array

                    printf("Executing: %s\n", args[0]);
                    execvp(args[0], args);
                    perror("Execution failed");
                    exit(EXIT_FAILURE);
                } else {
                    process->pid = pid;  // Store child PID in the parent process
                    printf("Child PID for %s is %d\n", process->executableName, process->pid);
                }
            }

            if (process->pid > 0 && process->isRunning) {  // Process is already created and assigned a PID
                anyProcessScheduled = 1;

                // Resume the process
                kill(process->pid, SIGCONT);  // Send SIGCONT to the child process

                // Update the wait time before the process runs
                time_t currentTime = time(NULL);
                process->wait_time += currentTime - process->lastPausedTime;

                // Let the process run for the specified time slice
                struct timespec ts;
                ts.tv_sec = TSLICE / 1000;
                ts.tv_nsec = (TSLICE % 1000) * 1000000;
                nanosleep(&ts, NULL);

                // After the time slice, pause the process if still running
                kill(process->pid, SIGSTOP);  // Send SIGSTOP to the child process
                process->lastPausedTime = currentTime;

                // Check if process completed within the time slice
                pid_t result = waitpid(process->pid, &status, WNOHANG);
                if (result == -1) {
                    perror("waitpid failed");
                    continue;
                } else if (result == 0) {
                    // The process is still running; re-queue it
                    printf("Process %s is still running, re-queuing.\n", process->executableName);
                    process->remaining_time -= TSLICE;
                    add_to_ready_queue(index);
                } else if (result > 0) {
                    if (WIFEXITED(status)) {
                        printf("Process %s exited normally with status %d\n", process->executableName, WEXITSTATUS(status));
                        completedProcesses++;
                        process->end_time = time(NULL); // Set completion time
                        // process->pid = -1; // You can reset this if needed
                    } else {
                        // If it was stopped or terminated, consider re-queuing as needed
                        printf("Process %s did not exit normally, re-queuing.\n", process->executableName);
                        process->remaining_time -= TSLICE;
                        add_to_ready_queue(index);
                    }
                }
            }
        }

        if (!anyProcessScheduled) {
            printf("No processes could be scheduled in this cycle.\n");
            break;
        }
    }

    if (completedProcesses == sharedData->readyQueue.submittedProcess) {
        printf("All processes completed successfully.\n");
    } else {
        printf("Scheduler finished but not all processes were completed.\n");
    }
}




// Function to handle child termination
void handle_child_termination(int sig) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t i = 0; i < shared_size; i++) {
            if (sharedData->table[i].pid == pid) {
                sharedData->table[i].isRunning = false;
                sharedData->table[i].completion_time += TSLICE;
                sharedData->table[i].end_time = time(NULL);
                double execTime = difftime(sharedData->table[i].end_time, sharedData->table[i].start_time);
                printf("Execution Time for process %s (PID: %d): %.2f seconds\n", sharedData->table[i].executableName, pid, execTime);
                completedProcesses++;
                printf("Process %s (PID: %d) completed.\n", sharedData->table[i].executableName, pid);
                break;
            }
        }
    }
}

// Function to print job details after scheduling
void print_job_details() {
    printf("\nJob Details:\n");
    printf("--------------------------------------------------------------------------------------------------\n");
    printf("| Name              | PID     | Completion Time  | Wait Time |   Arrival Time  |\n");
    printf("--------------------------------------------------------------------------------------------------\n");
    int totalWaitTime = 0;
    int totalCompletionTime = 0;
    for (size_t i = 0; i < shared_size; i++) {
        if (sharedData->table[i].pid > 0) {
            ProcessInfo *process = &sharedData->table[i];
            int waitTime = process->wait_time;
            int completionTime = process->completion_time;
            int arrivalTime = (int)process->arrival_time;
            totalWaitTime += waitTime;
            totalCompletionTime += completionTime;
            printf("| %-16s | %-7d | %-15d | %-9d |  %9d  |\n", process->executableName, process->pid, completionTime, waitTime, arrivalTime);
        }
    }
    printf("--------------------------------------------------------------------------------------------------\n");
    if (completedProcesses > 0) {
        float avgWaitTime = (float)totalWaitTime / completedProcesses;
        float avgCompletionTime = (float)totalCompletionTime / completedProcesses;
        printf("Average Wait Time: %.2f ms\n", avgWaitTime);
        printf("Average Completion Time: %.2f ms\n", avgCompletionTime);
    } else {
        printf("No processes completed.\n");
    }
}
