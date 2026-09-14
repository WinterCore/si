/*
 * Standalone example: receive shutdown while another thread waits for queue
 * space. There is no real queue here; queue_full stays true throughout.
 *
 * Build: cc -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread \
 *            cond_wait_shutdown.c -o /tmp/cond_wait_shutdown
 * Run:   /tmp/cond_wait_shutdown
 * Then press Ctrl-C, or run: kill -TERM <printed PID>
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    bool queue_full;
    bool shutdown_requested;
    sigset_t shutdown_signals;
} State;

/* Pthread functions return an error number directly, rather than setting errno. */
static void check(int error, const char *operation) {
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void *receive_shutdown(void *arg) {
    State *state = arg;
    int received_signal;

    check(sigwait(&state->shutdown_signals, &received_signal), "sigwait");

    /* This is ordinary thread code, NOT an asynchronous signal handler.
     * It can safely use a mutex and condition variable. */
    check(pthread_mutex_lock(&state->lock), "pthread_mutex_lock");
    state->shutdown_requested = true;
    fprintf(stderr, "Received signal %d; waking the reader.\n", received_signal);
    check(pthread_cond_broadcast(&state->not_full), "pthread_cond_broadcast");
    check(pthread_mutex_unlock(&state->lock), "pthread_mutex_unlock");

    return NULL;
}

int main(void) {
    State state = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .not_full = PTHREAD_COND_INITIALIZER,
        .queue_full = true,
        .shutdown_requested = false,
    };

    if (sigemptyset(&state.shutdown_signals) == -1 ||
        sigaddset(&state.shutdown_signals, SIGINT) == -1 ||
        sigaddset(&state.shutdown_signals, SIGTERM) == -1) {
        perror("building signal set");
        return EXIT_FAILURE;
    }

    /* Block BEFORE creating threads; new threads inherit this signal mask.
     * sigwait() will receive these blocked signals synchronously. */
    check(pthread_sigmask(SIG_BLOCK, &state.shutdown_signals, NULL),
          "pthread_sigmask");

    pthread_t shutdown_thread;
    check(pthread_create(&shutdown_thread, NULL, receive_shutdown, &state),
          "pthread_create");

    /* Main acts as the reader that cannot proceed because its queue is full. */
    check(pthread_mutex_lock(&state.lock), "pthread_mutex_lock");
    fprintf(stderr, "PID %ld: queue is full; waiting. Send SIGINT or SIGTERM.\n",
            (long)getpid());

    while (state.queue_full && !state.shutdown_requested) {
        /* Releases the mutex while asleep; reacquires it before returning.
         * Always recheck the state: a wakeup alone does not mean space exists. */
        check(pthread_cond_wait(&state.not_full, &state.lock),
              "pthread_cond_wait");
    }

    if (state.shutdown_requested) {
        fprintf(stderr, "Reader: shutdown requested, even though queue is full.\n");
        /* A real reader would choose its shutdown/draining path here. */
    }
    check(pthread_mutex_unlock(&state.lock), "pthread_mutex_unlock");

    check(pthread_join(shutdown_thread, NULL), "pthread_join");
    check(pthread_cond_destroy(&state.not_full), "pthread_cond_destroy");
    check(pthread_mutex_destroy(&state.lock), "pthread_mutex_destroy");
    return EXIT_SUCCESS;
}
