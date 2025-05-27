#include <stdio.h>
#include "uthreads.h"

// Keep a global pointer to the thread function for testing
thread_entry_point stored_entry = NULL;

// A simple thread function
void thread_function() {
    int tid = uthread_get_tid();
    printf("Hello from thread %d\n", tid);
}

int main() {
    // Initialize the thread system
    if (uthread_init(1000000) == -1) {
        fprintf(stderr, "Failed to initialize uthreads\n");
        return 1;
    }

    printf("Main thread ID: %d\n", uthread_get_tid());
    printf("Total quantums so far: %d\n", uthread_get_total_quantums());

    // Spawn a thread and manually store the function
    int tid1 = uthread_spawn(thread_function);
    if (tid1 == -1) {
        fprintf(stderr, "Failed to spawn thread\n");
        return 1;
    }

    printf("Spawned thread with ID: %d\n", tid1);

    // Manually call the thread function (simulating the scheduler)
    schedule_next();

    // Terminate the thread
    if (uthread_terminate(tid1) == -1) {
        fprintf(stderr, "Failed to terminate thread\n");
        return 1;
    }

    printf("Terminated thread with ID: %d\n", tid1);

    return 0;
}
