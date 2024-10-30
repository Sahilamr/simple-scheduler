#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "shared_memory.h"

// Function to start the scheduler
void start_scheduler(SharedMemoryData *data, size_t size,int ncpu,int tslice);

#endif // SCHEDULER_H