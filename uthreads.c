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
typedef unsigned long address_t;
static sigset_t sigvtalrm_set;

thread_t threads[MAX_THREAD_NUM];
char stacks[MAX_THREAD_NUM][STACK_SIZE];
int current_thread_id = 0;
static int total_quantums = 0;

//--------------------------------------------------------------------------------------------------//

static void enter_crit_sec()
{
    // block signals
    if (sigprocmask(SIG_BLOCK, &sigvtalrm_set, NULL) < 0)
    {
        fprintf(stderr, "system error: failed to block signal\n");
        exit(1);
    }
}

//--------------------------------------------------------------------------------------------------//

static void exit_crit_sec()
{
    // ublock signals
    if (sigprocmask(SIG_UNBLOCK, &sigvtalrm_set, NULL) < 0)
    {
        fprintf(stderr, "system error: failed to unblock signal\n");
        exit(1);
    }
}

//--------------------------------------------------------------------------------------------------//

address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor %%fs:0x30, %0\n"
                 "rol $0x11 , %0\n"

                 : "=g"(ret)
                 : "0"(addr));
    return ret;
}

//--------------------------------------------------------------------------------------------------//

int uthread_init(int quantum_usecs)
{
    // initialize all threads as unused
    for (int i = 0; i < MAX_THREAD_NUM; i++)
    {
        threads[i].state = THREAD_UNUSED;
    }

    // initia;ize main thread
    threads[0].tid = 0;
    threads[0].state = THREAD_RUNNING;
    threads[0].quantums = 1;
    threads[0].sleep_until = 0;
    threads[0].entry = NULL;
    total_quantums = 1;
    current_thread_id = 0;

    // for critical section
    sigemptyset(&sigvtalrm_set);
    sigaddset(&sigvtalrm_set, SIGVTALRM);

    // Set up signal handler
    struct sigaction sa;
    sa.sa_handler = timer_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // Register the signal handler for virtual timer alarm
    if (sigaction(SIGVTALRM, &sa, NULL) == -1)
    {
        fprintf(stderr, "system error: sigaction failed\n");
        exit(1);
    }

    // Set virtual timer (sends SIGVTALRM every microsec)
    struct itimerval timer;
    // initial expiration time
    timer.it_value.tv_sec = quantum_usecs / 1000000;
    timer.it_value.tv_usec = quantum_usecs % 1000000;
    // repeating interval
    timer.it_interval.tv_sec = quantum_usecs / 1000000;
    timer.it_interval.tv_usec = quantum_usecs % 1000000;

    // Start the virtual timer (only when proccess is running)
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL) == -1)
    {
        fprintf(stderr, "system error: setitimer failed\n");
        exit(1);
    }
    return 0;
}

//--------------------------------------------------------------------------------------------------//

int uthread_spawn(thread_entry_point entry_point)
{
    enter_crit_sec();

    // chech if entry point is null
    if (entry_point == NULL)
    {
        fprintf(stderr, "system error: entry point cannot be NULL\n");
        exit_crit_sec();
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
        exit_crit_sec();
        return -1;
    }

    // Initializes thread
    threads[new_tid].tid = new_tid;
    threads[new_tid].state = THREAD_READY;
    threads[new_tid].quantums = 0;
    threads[new_tid].sleep_until = 0;
    threads[new_tid].entry = entry_point;

    // set up its context
    setup_thread(new_tid, stacks[new_tid], entry_point);

    exit_crit_sec();
    return new_tid;
}

//--------------------------------------------------------------------------------------------------//

int uthread_terminate(int tid)
{
    enter_crit_sec();
    // error if thread is unused
    if (threads[tid].state == THREAD_UNUSED)
    {
        fprintf(stderr, "system error: thread doesn't exist\n");
        exit_crit_sec();
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
    if (tid == current_thread_id)
    {
        schedule_next();
        // Should never reach here, but just in case:
    }
    exit_crit_sec();
    return 0;
}

//--------------------------------------------------------------------------------------------------//

static void thread_wrapper(void)
{
    int tid = current_thread_id;
    thread_entry_point func = threads[tid].entry;

    // Call the actual thread function
    func();

    // When function returns, terminate the thread (this won't return)
    uthread_terminate(tid);
}

//--------------------------------------------------------------------------------------------------//

int uthread_block(int tid)
{
    enter_crit_sec();
    // error if thread is unused
    if (threads[tid].state == THREAD_UNUSED)
    {
        fprintf(stderr, "system error: thread doesn't exist\n");
        exit_crit_sec();
        return -1;
    }
    // check if its the main thread
    else if (threads[tid].tid == 0)
    {
        fprintf(stderr, "system error: cannot block main thread\n");
        exit_crit_sec();
        return -1;
    }
    // if not unused or main thread, block it!
    threads[tid].state = THREAD_BLOCKED;

    exit_crit_sec();
    return 0;
}

//--------------------------------------------------------------------------------------------------//

int uthread_resume(int tid)
{
    enter_crit_sec();
    // putlocked thread in ready state
    if (threads[tid].state == THREAD_BLOCKED)
    {
        threads[tid].state = THREAD_READY;
    }
    else if (threads[tid].state == THREAD_READY || threads[tid].state == THREAD_RUNNING)
    {
        exit_crit_sec();
        return 0;
    }
    // error if thread is unused
    else if (threads[tid].state == THREAD_UNUSED)
    {
        fprintf(stderr, "system error: thread doesn't exist\n");
        exit_crit_sec();
        return -1;
    }

    exit_crit_sec();
    return 0;
}

//--------------------------------------------------------------------------------------------------//

int uthread_sleep(int num_quantums)
{
    enter_crit_sec();
    // error if main thread
    if (current_thread_id == 0)
    {
        fprintf(stderr, "system error: cannot put main thread to sleep\n");
        exit_crit_sec();
        return -1;
    }
    // sleep & block :))
    threads[current_thread_id].sleep_until = uthread_get_total_quantums() + num_quantums;
    threads[current_thread_id].state = THREAD_BLOCKED;
    exit_crit_sec();
    return 0;
}

//--------------------------------------------------------------------------------------------------//

int uthread_get_tid()
{
    return threads[current_thread_id].tid;
}

//--------------------------------------------------------------------------------------------------//

int uthread_get_total_quantums()
{
    return total_quantums;
}

//--------------------------------------------------------------------------------------------------//

int uthread_get_quantums(int tid)
{
    // error if thread is unused
    if (threads[tid].state == THREAD_UNUSED)
    {
        fprintf(stderr, "system error: thread doesn't exist\n");
        return -1;
    }

    // if the thread is running retrun currents + 1 (Before incrament)
    else if (threads[tid].state == THREAD_RUNNING)
    {
        // current quantam + 1 instead
        return threads[tid].quantums + 1;
    }
    // return without +1 (already incramented)
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

void schedule_next(void)
{
    enter_crit_sec();
    int next_tid = -1;
    for (int i = 1; i < MAX_THREAD_NUM; i++)
    {
        // round robbin
        int check_tid = (current_thread_id + i) % MAX_THREAD_NUM;
        if (threads[check_tid].state == THREAD_READY)
        {
            next_tid = check_tid;
            break;
        }
    }

    // if thread is not in a ready state return
    if (next_tid == -1)
    {
        exit_crit_sec();
        return;
    }

    // scheduule next
    int prev_tid = current_thread_id;
    current_thread_id = next_tid;
    threads[next_tid].state = THREAD_RUNNING;

    exit_crit_sec();
    context_switch(&threads[prev_tid], &threads[next_tid]);
}

//--------------------------------------------------------------------------------------------------//

void context_switch(thread_t *current, thread_t *next)
{
    // Save current thread context
    int ret_val = sigsetjmp(current->env, 1);

    if (ret_val == 0)
    {
        // First time --> jump to next thread
        siglongjmp(next->env, 1);
    }
    // When we return here (ret_val != 0), this thread is being resumed
}

//--------------------------------------------------------------------------------------------------//

void timer_handler(int signum)
{
    enter_crit_sec();
    // updates global quantum counters
    total_quantums++;

    // Increments current thread's quantum count
    threads[current_thread_id].quantums++;

    // If current thread's quantum expired --> move to READY
    threads[current_thread_id].state = THREAD_READY;

    // Schedule next
    schedule_next();

    exit_crit_sec();
}

//--------------------------------------------------------------------------------------------------//

void setup_thread(int tid, char *stack, thread_entry_point entry_point)
{
    address_t sp = (address_t)(stack + STACK_SIZE - sizeof(address_t));
    address_t pc = (address_t)(thread_wrapper);

    // Saves the current context
    sigsetjmp(threads[tid].env, 1);

    // Sets the stack pointer and the program counter
    threads[tid].env->__jmpbuf[JB_SP] = translate_address(sp);
    threads[tid].env->__jmpbuf[JB_PC] = translate_address(pc);
    sigemptyset(&threads[tid].env->__saved_mask);
}

//---------------------------------------------End of File--------------------------------------------------------//
