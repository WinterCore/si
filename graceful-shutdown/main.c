#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct WorkerManagerState {
    int jobs[8];
    int job_count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} WorkerManagerState;

void *thread_worker(void *arg) {
    WorkerManagerState *state = arg;

    pthread_mutex_lock(&state->lock);

    // Keep waiting until queue has items
    while (state->job_count <= 0) {
        pthread_cond_wait(&state->not_empty, &state->lock);
    }

    int job = state->jobs[0];
    state->job_count -= 1;
    if (state->job_count > 0) {
        memmove(state->jobs, state->jobs + 1, state->job_count * sizeof(int));
    }

    pthread_mutex_unlock(&state->lock);

    // Process job
    sleep(2);

    return NULL;
}

void fork_runner(size_t i, int rfd) {
    WorkerManagerState state = {
        .jobs = {},
        .job_count = 0,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .not_empty = PTHREAD_COND_INITIALIZER,
        .not_full = PTHREAD_COND_INITIALIZER,
    };

    pthread_t worker_threads[4] = {};

    for (size_t i = 0; i < 4; i += 1) {
        pthread_create(&worker_threads[i], NULL, thread_worker, &state);
    }
    
    while (true) {
        uint64_t job_id;
        ssize_t bytes_read = read(rfd, &job_id, 4);

        if (bytes_read == -1) {
            perror("read");
        }

        if (bytes_read == 0) {
            exit(EXIT_SUCCESS);
        }
        
        pthread_mutex_lock(&state.lock);

        // Keep waiting until queue has space
        while (state.job_count >= 8) {
            pthread_cond_wait(&state.not_full, &state.lock);
        }

        if (state.job_count < 8) {
            state.jobs[state.job_count] = job_id;
            state.job_count += 1;
            pthread_mutex_unlock(&state.lock);
            continue;
        }
    }
}

int main() {
    pid_t pids[3] = {};
    int pipes[6] = {};

    for (size_t i = 0; i < 3; i += 1) {
        pid_t pid = fork();
        pids[i] = pid;

        int pr = pipe(&pipes[i * 2]);

        if (pid == 0) {
            fork_runner(i, pipes[i * 2]);
            return 0;
        } else if (pid > 0) {
            printf("parent\n");
        } else {
            perror("fork");
        }
    }

    for (size_t i = 0; i < 3; i += 1) {
        pid_t pid = wait(NULL);
        printf("fork %d terminated\n", pid);
    }
    

    return 0;
}
