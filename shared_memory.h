// shared_memory.h
#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <sys/types.h>
#include <stdbool.h>
#include <sys/time.h>
#include <semaphore.h>

#define MAX_PROCESSES 100

#define MAX_NAME_LENGTH 256 // Maximum length for executable names
typedef struct {
    size_t index;
    int priority; 
    pid_t pid;                // Process ID
    char executableName[256]; // Name of the executable
    bool isRunning;           // Is the process currently running
    int completion_time;      // Total completion time in milliseconds
    int wait_time;            // Wait time in milliseconds
    time_t arrival_time;      // When the process was added
    time_t start_time;        // When the process started executing
    time_t end_time;          // When the process finished executing
    int readyQueueSize;
    int remaining_time;
    time_t lastPausedTime;    // When the process was last paused
    // Add any other fields as needed
} ProcessInfo;

typedef struct {
    struct {
        ProcessInfo queue[MAX_PROCESSES];
        int readyQueueSize;
        int submittedProcess;
    } readyQueue;
    char executableName[MAX_NAME_LENGTH];  // Name of the executable
    int priority;              // Priority of the process
    pid_t pid;                 // PID of the process
    bool isRunning;            // Status of the process (running or not)
    int completion_time;       // Start time of the process
    struct timeval endTime;    // End time of the process
    int submittedProcess;
    ProcessInfo table[MAX_PROCESSES]; 
    int NCPU;
    time_t TSLICE;
    sem_t mutex;
} SharedMemoryData;

#define SHARED_MEM_NAME "/executablename"

#endif // SHARED_MEMORY_H
