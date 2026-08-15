#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

    pthread_t worker_threads[4] = {};

    for (int i = 0; i < 4; i += 1) {
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
        fprintf(stderr, "Fork %d: Pushed job %llu to queue\n", fork_i, job_id);

        state.jobs[state.job_count] = job_id;
        state.job_count += 1;
        pthread_cond_signal(&state.not_empty);
        pthread_mutex_unlock(&state.lock);
    }
}

int main() {
    pid_t pids[3] = {};
    int pipes[6] = {};

    for (int i = 0; i < 3; i += 1) {
        int pr = pipe(&pipes[i * 2]);

        pid_t pid = fork();
        pids[i] = pid;


        if (pid == 0) {
            fork_runner(i, pipes[i * 2]);
            return 0;
        } else if (pid > 0) {
            printf("parent\n");
        } else {
            perror("fork");
        }
    }

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000000 };

    FILE *df = fopen("dispatch.log", "w");
    if (df == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    char buffer[1024];

    int job = 0;
    int process_index = 0;

    // Dispatch jobs
    while (true) {
        int wfd = pipes[process_index * 2 + 1];        
        ssize_t pipe_write_result = write(wfd, &job, sizeof(int));

        if (pipe_write_result < sizeof(int)) {
            fprintf(stderr, "Failed to send job through pipe!\n");
            exit(EXIT_FAILURE);
        }

        int bytes_written = snprintf(buffer, sizeof(buffer), "Dispatched job %d to process %d\n", job, process_index);
        int result = fwrite(buffer, 1, bytes_written, df);
        if (result < bytes_written) {
            perror("fwrite");
            exit(EXIT_FAILURE);
        }
        buffer[bytes_written] = '\0';
        fprintf(stderr, "%s", buffer);
        fflush(df);
        nanosleep(&ts, NULL);

        job += 1;
        process_index += 1;
        if (process_index >= 3) {
            process_index = 0;
        }
    }

    for (size_t i = 0; i < 3; i += 1) {
        pid_t pid = wait(NULL);
        printf("fork %d terminated\n", pid);
    }
    

    return 0;
}
