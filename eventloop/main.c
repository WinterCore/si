#include <stdio.h>
#include <pthread.h>
#include <inttypes.h>
#include <poll.h>

#define WORKER_COUNT 2
#define JOB_QUEUE_CAPACITY 4

typedef struct SharedWorkerState {
    uint64_t jobs[JOB_QUEUE_CAPACITY];
    size_t job_count;
    pthread_mutex_t jobs_mutex;
    pthread_cond_t cond_has_jobs;
} SharedWorkerState;

void *worker_runner(void *arg) {
    fprintf(stderr, "Worker created!\n");

    return NULL;
}


int main() {
    pthread_t workers[WORKER_COUNT] = {};
    
    SharedWorkerState sws = {0};
    pthread_mutex_init(&sws.jobs_mutex, NULL);
    pthread_cond_init(&sws.cond_has_jobs, NULL);

    struct pollfd fds[] = {
        {},
    };

    for (size_t i = 0; i < WORKER_COUNT; i += 1) {
        if (pthread_create(&workers[i], NULL, worker_runner, &sws) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    
    for (size_t i = 0; i < WORKER_COUNT; i += 1) {
        if (pthread_join(workers[i], NULL) != 0) {
            perror("pthread_join");
            return 1;
        }
    }

    return 0;
}
