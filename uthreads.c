#define _GNU_SOURCE
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <stdbool.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include "uthreads.h"
#ifdef __x86_64__
#define JB_SP 6
#define JB_PC 7
#else
#error "Unsupported architecture"
#endif
//#define translate_address(x) ((address_t)(x))  
typedef unsigned long address_t;

//--------------------------------------------------------------------------------------------------//
static address_t translate_address(address_t addr)
{
    address_t ret;
    __asm__ volatile("xor %%fs:0x30, %0\n"
                 "rol $0x11, %0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}

thread_t threads[MAX_THREAD_NUM];
char stacks[MAX_THREAD_NUM][STACK_SIZE];
int current_thread_id = 0;
static int total_quantums = 0;

/**
 * @brief Initializes the user-level thread library.
 *
 * This function must be called before any other thread library function.
 * It sets up internal data structures, initializes the main thread (tid == 0) as running,
 * and configures the timer for quantum management. The main thread uses the process's regular stack.
 *
 * @param quantum_usecs Length of a quantum in microseconds (must be positive).
 * @return 0 on success; -1 on error (e.g., if quantum_usecs is non-positive).
 */
int uthread_init(int quantum_usecs)
{
    // initialize all threads as unused
    for (int i = 0; i < MAX_THREAD_NUM; i++)
    {
        threads[i].state = THREAD_UNUSED;
    }

    // main thread
    threads[0].tid = 0;
    threads[0].state = THREAD_RUNNING;
    threads[0].quantums = 1;
    threads[0].sleep_until = 0;
    threads[0].entry = NULL;
    total_quantums = 1;
    current_thread_id = 0;

    return 0;
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Creates a new thread.
 *
 * Allocates a new TCB and separate stack for the thread (using a char array).
 * The thread is added to the end of the READY queue.
 * Calling this function with a NULL entry_point or exceeding MAX_THREAD_NUM is an error.
 *
 * @param entry_point Pointer to the thread’s entry function (must not be NULL).
 * @return On success, returns the new thread’s ID; on failure, returns -1.
 */

int uthread_spawn(thread_entry_point entry_point)
{
    if (entry_point == NULL)
    {
        fprintf(stderr, "system error: entry point cannot be NULL\n");
        return -1;
    }

    // Find available non-negative thread ID
    int new_tid = -1;
    for (int i = 0; i < MAX_THREAD_NUM; i++)
    {
        if (threads[i].state == THREAD_UNUSED)
        {
            new_tid = i;
            break;
        }
    }

    // if we reach max threads
    if (new_tid == -1)
    {
        fprintf(stderr, "system error: max threads reached\n");
        return -1;
    }

    // Initialize new thread
    threads[new_tid].tid = new_tid;
    threads[new_tid].state = THREAD_READY;
    threads[new_tid].quantums = 0;
    threads[new_tid].sleep_until = 0;
    threads[new_tid].entry = entry_point;

    return new_tid;
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Terminates a thread.
 *
 * Terminates the thread with the specified tid and releases all resources allocated for it.
 * The thread is removed from all scheduling structures. If no thread with the given tid exists,
 * it is considered an error. Terminating the main thread (tid == 0) will terminate the entire process
 * (after releasing allocated resources).
 *
 * @param tid Thread ID to terminate.
 * @return 0 on success; -1 on error. (Note: if a thread terminates itself or if the main thread terminates,
 * the function does not return.)
 */
int uthread_terminate(int tid)
{

    if (threads[tid].state == THREAD_UNUSED)
    {
        fprintf(stderr, "system error: thread doesn't exist\n");
        return -1;
    }

    // If terminating main thread (tid == 0), terminate entire process
    if (tid == 0)
    {
        // Release resources for all threads first
        for (int i = 0; i < MAX_THREAD_NUM; i++)
        {
            threads[i].state = THREAD_UNUSED;
        }
        exit(1);
    }

    // Release all resources allocated for this thread
    threads[tid].tid = -1;
    threads[tid].state = THREAD_TERMINATED; // Thread unused?
    threads[tid].quantums = 0;
    threads[tid].sleep_until = 0;
    threads[tid].entry = NULL; // Not entry_point

    return 0;
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Blocks a thread.
 *
 * Moves the thread with the given tid into the BLOCKED state.
 * A blocked thread may later be resumed using uthread_resume.
 * It is an error to block a thread that does not exist or to block the main thread (tid == 0).
 * Blocking a thread that is already BLOCKED is a no-op.
 *
 * @param tid Thread ID to block.
 * @return 0 on success; -1 on error.
 */
int uthread_block(int tid)
{
    if (threads[tid].state == THREAD_UNUSED)
    {
        fprintf(stderr, "system error: thread doesn't exist\n");
        return -1;
    }
    else if (threads[tid].tid == 0)
    {
        fprintf(stderr, "system error: cannot block main thread\n");
        return -1;
    }
    threads[tid].state = THREAD_BLOCKED;

    return 0;
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Resumes a blocked thread.
 *
 * Moves a thread from the BLOCKED state to the READY state.
 * If the thread is already in RUNNING or READY state, this call has no effect.
 * It is an error if no thread with the given tid exists.
 *
 * @param tid Thread ID to resume.
 * @return 0 on success; -1 on error.
 */
int uthread_resume(int tid)
{
    if (threads[tid].state == THREAD_BLOCKED)
    {
        threads[tid].state = THREAD_READY;
    }
    else if (threads[tid].state == THREAD_READY || threads[tid].state == THREAD_RUNNING)
    {
        return 0;
    }
    else if (threads[tid].state == THREAD_UNUSED)
    {
        fprintf(stderr, "system error: thread doesn't exist\n");
        return -1;
    }

    return 0;
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Puts the running thread to sleep.
 *
 * Blocks the currently running thread for a specified number of quantums.
 * The current quantum is not counted; sleeping begins with the next quantum.
 * After the sleep period expires, the thread is moved to the end of the READY queue.
 * It is an error for the main thread (tid == 0) to call this function.
 *
 * @param num_quantums Number of quantums to sleep.
 * @return 0 on success; -1 on error.
 */
int uthread_sleep(int num_quantums)
{
    if (current_thread_id == 0)
    {
        fprintf(stderr, "system error: cannot put main thread to sleep\n");
        return -1;
    }
    threads[current_thread_id].sleep_until = uthread_get_total_quantums() + num_quantums;
    threads[current_thread_id].state = THREAD_BLOCKED;
    return 0;
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Returns the calling thread's ID.
 *
 * @return The thread ID of the calling thread.
 */
int uthread_get_tid()
{
    return threads[current_thread_id].tid;
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Returns the total number of quantums since the library was initialized.
 *
 * The count starts at 1 immediately after uthread_init. Every new quantum, regardless of cause,
 * increments this counter.
 *
 * @return Total number of quantums.
 */
int uthread_get_total_quantums()
{

    return total_quantums;
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Returns the number of quantums the thread with the specified tid has run.
 *
 * For a thread in the RUNNING state, the current quantum is included.
 * An error is retursned if no thread with the given tid exists.
 *
 * @param tid Thread ID.
 * @return Number of quantums for the specified thread; -1 on error.
 */
int uthread_get_quantums(int tid)
{
    if (threads[tid].state == THREAD_UNUSED)
    {
        fprintf(stderr, "system error: thread doesn't exist\n");
        return -1;
    }
    else if (threads[tid].state == THREAD_RUNNING)
    {
        return threads[tid].quantums + total_quantums;
    }
    else
    {
        return threads[tid].quantums;
    }
}
//--------------------------------------------------------------------------------------------------//
/* ===================================================================== */
/*              Internal Helper Functions and Structures                 */
/* ===================================================================== */
/*
 * The following declarations are intended for internal use by the thread library.
 * They provide guidance on how to structure your implementation using sigsetjmp/siglongjmp
 * and manually managed stacks.
 */
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Scheduler: Selects the next thread to run.
 *
 * This function examines the READY queue and selects the next thread for execution.
 * It handles state transitions and triggers a context switch.
 */
void schedule_next(void)
{

    int next_tid = -1;
    for (int i = 1; i < MAX_THREAD_NUM; i++)
    {
        int check_tid = (current_thread_id + i) % MAX_THREAD_NUM;
        if (threads[check_tid].state == THREAD_READY)
        {
            next_tid = check_tid;
            break;
        }
    }

    if (next_tid == -1)
    {
        return;
    }
    int prev_tid = current_thread_id;
    current_thread_id = next_tid;
    threads[next_tid].state = THREAD_RUNNING;
    context_switch(&threads[prev_tid], &threads[next_tid]);
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Context switch helper.
 *
 * Uses sigsetjmp and siglongjmp to save the current thread's context and restore the context of the next thread.
 *
 * @param current Pointer to the current thread's TCB.
 * @param next Pointer to the next thread's TCB.
 */
void context_switch(thread_t *current, thread_t *next)
{
    // Save current thread's context
    int ret_val = sigsetjmp(current->env, 1);
    
    if (ret_val == 0) {
        // First time - jump to next thread
        siglongjmp(next->env, 1);
    }
    // When we return here (ret_val != 0), this thread is being resumed
}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Timer signal handler.
 *
 * Registered as the handler for timer signals, this function updates global quantum counters
 * and initiates a scheduling decision when a quantum expires.
 *
 * @param signum The signal number (e.g., SIGVTALRM).
 */
void timer_handler(int signum)
{
    // updates global quantum counters
    total_quantums++;
    
    // Increments current thread's quantum count
    threads[current_thread_id].quantums++;
    
    // Current thread's quantum expired - move to READY
    threads[current_thread_id].state = THREAD_READY;
    
    // Schedule next thread
    schedule_next();}
//--------------------------------------------------------------------------------------------------//
/**
 * @brief Initializes a thread's jump buffer.
 *
 * Sets up the thread’s jump buffer for context switching by manually assigning the stack pointer
 * and program counter using sigsetjmp/siglongjmp techniques. You may need to perform architecture-specific
 * address translation (see provided reference implementation) when initializing the context.
 *
 * @param tid Thread ID.
 * @param stack Pointer to the thread's allocated stack (a char array).
 * @param entry_point Pointer to the thread's entry function.
 */
void setup_thread(int tid, char *stack, thread_entry_point entry_point)
{
    address_t sp = (address_t)(stack + STACK_SIZE - sizeof(address_t));
    address_t pc = (address_t)(entry_point);

    /*
    threads[tid].env->__jmpbuf[JB_SP] = translate_address(sp);
    threads[tid].env->__jmpbuf[JB_PC] = translate_address(pc);
    sigemptyset(&threads[tid].env->__saved_mask);
     threads[tid].env[0].__jmpbuf[JB_SP] = translate_address(sp);
    threads[tid].env[0].__jmpbuf[JB_PC] = translate_address(pc);

    // Clears the signal mask
    sigemptyset(&threads[tid].env[0].__saved_mask);
    */

    // Saves the current context 
    sigsetjmp(threads[tid].env, 1);

    // Sets the stack pointer and the program counter
   threads[tid].env->__jmpbuf[JB_SP] = translate_address(sp);
    threads[tid].env->__jmpbuf[JB_PC] = translate_address(pc);
    sigemptyset(&threads[tid].env->__saved_mask);
}

//---------------------------------------------End of File--------------------------------------------------------//


