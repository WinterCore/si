#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>

#define JOB_GENERATION_INTERVAL 100
#define NUM_FORKS 3
#define NUM_WORKERS 5
// 1 shutdown fd + one write pipe per fork
#define NUM_POLL_FDS (NUM_FORKS + 1)

// TODO: Dead fork detection & dead worker detection
// TODO: 10 second shutdown timeout

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct WorkerManagerState {
    FILE *lf;
    int fork_i;
    int jobs[8];
    int job_count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} WorkerManagerState;

void *thread_worker(void *arg) {
    WorkerManagerState *state = arg;

    while (true) {
        pthread_mutex_lock(&state->lock);

        // Keep waiting until queue has items
        while (state->job_count <= 0) {
            pthread_cond_wait(&state->not_empty, &state->lock);
        }

        int job = state->jobs[0];
        // Shutdown signal
        if (job == -1) {
            // Do not consume signal so that it can be used by other workers
            // unlock and exit
            pthread_mutex_unlock(&state->lock);
            return NULL;
        }

        state->job_count -= 1;
        if (state->job_count > 0) {
            memmove(state->jobs, state->jobs + 1, state->job_count * sizeof(int));
        }

        pthread_cond_signal(&state->not_full);
        pthread_mutex_unlock(&state->lock);

        // Process job
        sleep(3);
        
        char buffer[1024];
        // Done processing job
        int bytes_written = snprintf(buffer, sizeof(buffer), "Fork %d: Finished processing job %d\n", state->fork_i, job);
        // Dispatch job
        int result = fwrite(buffer, 1, bytes_written, state->lf);
        if (result < bytes_written) {
            perror("fwrite");
            exit(EXIT_FAILURE);
        }
        buffer[bytes_written] = '\0';
        fprintf(stderr, "%s", buffer);
    }

    return NULL;
}

void fork_runner(int fork_i, int rfd) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "fork-%d.log", fork_i + 1);

    // Open in append mode so that fwrite is atomic and we can write from multiple threads at the same time
    FILE *lf = fopen(buffer, "a");
    
    if (lf == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    // Enable line buffering, so output is flushed when a newline is written.
    setvbuf(lf, NULL, _IOLBF, 0);

    WorkerManagerState state = {
        .fork_i = fork_i,
        .lf = lf,
        .jobs = {},
        .job_count = 0,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .not_empty = PTHREAD_COND_INITIALIZER,
        .not_full = PTHREAD_COND_INITIALIZER,
    };

    pthread_t worker_threads[NUM_WORKERS] = {};

    // Launch worker threads
    for (int i = 0; i < NUM_WORKERS; i += 1) {
        pthread_create(&worker_threads[i], NULL, thread_worker, &state);
    }
 
    while (true) {
        int64_t job_id;
        ssize_t bytes_read = read(rfd, &job_id, sizeof(int64_t));

        if (bytes_read == -1) {
            perror("read");
            exit(EXIT_FAILURE);
        }

        // Shutdown signal EOF
        if (bytes_read == 0) {
            fprintf(stderr, "Shutting down fork %d\n", fork_i);
            job_id = -1;
        }
        
        pthread_mutex_lock(&state.lock);

        // Keep waiting until queue has space
        while (state.job_count >= 8) {
            pthread_cond_wait(&state.not_full, &state.lock);
        }
        
        if (job_id == -1) {
            fprintf(stderr, "Fork %d: Pushed kill pill to queue\n", fork_i);
        } else {
            fprintf(stderr, "Fork %d: Pushed job %ld to queue\n", fork_i, job_id);
        }

        state.jobs[state.job_count] = job_id;
        state.job_count += 1;
        if (job_id == -1) {
            pthread_cond_broadcast(&state.not_empty);
            pthread_mutex_unlock(&state.lock);
            break;
        }

        pthread_cond_signal(&state.not_empty);
        pthread_mutex_unlock(&state.lock);
    }

    for (int i = 0; i < NUM_WORKERS; i += 1) {
        pthread_join(worker_threads[i], NULL);
        fprintf(stderr, "Fork %d: Worker %d exited!\n", fork_i, i);
    }

    exit(EXIT_SUCCESS);
}

pid_t pids[NUM_FORKS] = {};

void *shutdown_thread_runner(void *arg) {
    int *shutdown_wpipe = arg;
    sigset_t set;
    int sig;

    // 1. Build the set containing both signals
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);

    // 2. Block both signals process-wide (or for this thread)
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    printf("Waiting for SIGINT or SIGTERM...\n");

    int n = 0;

    while (true) {
        // 3. Wait synchronously for whichever arrives first
        sigwait(&set, &sig);

        n += 1;

        // First term signal is graceful shutdown 
        if (n == 1) {
            bool signal = true;
            int bytes_written = write(*shutdown_wpipe, &signal, 1);

            if (bytes_written < 1) {
                perror("write shutdown signal");
            }
        }

        // Second term signal is kill everything with fire
        if (n == 2) {
            for (int i = 0; i < NUM_FORKS; i += 1) {
                if (kill(pids[i], SIGKILL) == -1) {
                    perror("kill failed");
                } else {
                    fprintf(stderr, "Killed fork %d and all of its workers!\n", i);
                }
            }

            break;
        }
    }
        
    return NULL;
}

int main() {
    int wpipes[NUM_FORKS] = {};
    int fildes[2] = {};
    int shutdown_fildes[2];

    signal(SIGPIPE, SIG_IGN);

    // Block
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);

    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (int i = 0; i < NUM_FORKS; i += 1) {
        int pr = pipe(fildes);

        if (pr != 0) {
            perror("pipe");
            exit(EXIT_FAILURE);
        }

        wpipes[i] = fildes[1];

        pid_t pid = fork();
        pids[i] = pid;

        if (pid == 0) {
            // Close write end since it's only needed on the parent
            for (int j = 0; j <= i; j += 1) close(wpipes[j]);

            // Child
            fork_runner(i, fildes[0]);

            return 0;
        } else if (pid > 0) {
            // Close read end since it's only needed on the child
            close(fildes[0]);
            printf("parent\n");
        } else {
            perror("fork");
            exit(EXIT_FAILURE);
        }
    }

    FILE *df = fopen("dispatch.log", "w");
    if (df == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    char buffer[1024];

    int64_t job = 0;
    int turn = 0;
 
    // Launch shutdown thread
    int pr = pipe(shutdown_fildes);

    if (pr != 0) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    pthread_t shutdown_thread_id;

    if (pthread_create(&shutdown_thread_id, NULL, shutdown_thread_runner, &shutdown_fildes[1]) != 0) {
        perror("pthread_create");
        exit(EXIT_FAILURE);
    }

    // Need to poll on multiple file descriptors in case
    struct pollfd fds[NUM_POLL_FDS];

    fds[0].fd = shutdown_fildes[0]; fds[0].events = POLLIN;
    for (int i = 0; i < NUM_FORKS; i += 1) {
        fds[i + 1].fd = wpipes[i];
        fds[i + 1].events = POLLOUT;
    }

    int64_t deadline = now_ms() + JOB_GENERATION_INTERVAL;
    int64_t wait_remaining = JOB_GENERATION_INTERVAL;

    int64_t start = now_ms();

    // Dispatch jobs
    while (true) {
        int n = poll(fds, NUM_POLL_FDS, wait_remaining < 0 ? -1 : wait_remaining);
        int64_t now = now_ms();

        wait_remaining = deadline - now;
 
        if (wait_remaining < 0) {
            for (int i = 1; i < NUM_POLL_FDS; i += 1) {
                if (fds[i].fd != -1) {
                    fds[i].events = POLLOUT;
                }
            }
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            // Real error
            break;
        }

        if (n == 0) {
            continue;
        }

        // Check shutdown signal first
        if (fds[0].revents & POLLIN) {
            // Shutdown signal received
            break;
        }

        int wfd = -1;
        int process = -1;

        for (int i = 1; i < NUM_POLL_FDS; i += 1) {
            // Dead forks detection
            if (fds[i].revents & POLLHUP || fds[i].revents & POLLERR) {
                fprintf(stderr, "Fork %d has died unexpectedly!", i);
                fds[i].fd = -1;
                fds[i].events = 0;
            }
        }

        // Pick the first pipe that has space but prioritize turns
        for (int i = 0; i < NUM_FORKS; i += 1) {
            int fork_idx = (turn + i) % NUM_FORKS;

            if (fds[fork_idx + 1].fd == -1) {
                continue;
            }

            if (fds[fork_idx + 1].revents & POLLOUT) {
                wfd = fds[fork_idx + 1].fd;
                process = fork_idx;
                break;
            }
        }

        if (wfd == -1) {
            continue;
        }

        // printf("Wfd: %d\npoll: %d\nwait_remaining: %ld\n", wfd, n, wait_remaining);

        ssize_t pipe_bytes_written = write(wfd, &job, sizeof(int64_t));

        if (pipe_bytes_written < 0) {
            if (errno == EPIPE) {
                for (int i = 1; i < NUM_POLL_FDS; i += 1) {
                    if (fds[i].fd == wfd) {
                        fds[i].fd = -1;
                        fds[i].events = 0;
                        printf("Fork %d has died unexpectedly!", i);
                    }
                }
                
                continue;
            }

            perror("Failed to send job through pipe");
            exit(EXIT_FAILURE);
        }

        int bytes_written = snprintf(buffer, sizeof(buffer), "Dispatched job %lu to process %d\n", job, process);
        int result = fwrite(buffer, 1, bytes_written, df);
        if (result < bytes_written) {
            perror("fwrite");
            exit(EXIT_FAILURE);
        }
        buffer[bytes_written] = '\0';
        fprintf(stderr, "%s", buffer);
        fflush(df);

        job += 1;
        turn += 1;
        if (turn >= NUM_FORKS) {
            turn = 0;
        }

        deadline = deadline + JOB_GENERATION_INTERVAL;
        wait_remaining = deadline - now_ms();
        
        /*
        if (start + 3000 < now_ms()) {
            break;
        }
        */

        if (wait_remaining < 0) {
            continue;
        }

        // Remove listeners for all fork fds
        for (int i = 1; i < NUM_POLL_FDS; i += 1) {
            if (fds[i].fd != -1) {
                fds[i].events = 0;
            }
        }
    }
    
    // Shutdown was initiatied

    fprintf(stderr, "Shutting down...\n");

    // Clean up pipes
    for (int i = 0; i < NUM_FORKS; i += 1) {
        close(wpipes[i]);
    }

    // Wait for forks to terminate
    for (size_t i = 0; i < NUM_FORKS; i += 1) {
        pid_t pid = wait(NULL);
        printf("Fork %zu terminated\n", i);
    }
    
    return 0;
}
