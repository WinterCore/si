#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/eventfd.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>

#define WORKER_COUNT 2
#define JOB_QUEUE_CAPACITY 4

int jobs_finished_eventfd;

typedef struct SharedWorkerState {
    uint64_t jobs[JOB_QUEUE_CAPACITY];
    size_t job_count;
    pthread_mutex_t jobs_mutex;
    pthread_cond_t cond_has_jobs;
    bool shutdown_requested;
} SharedWorkerState;

void *worker_runner(void *arg) {
    SharedWorkerState *sws = arg;

    fprintf(stderr, "Worker created!\n");

    pthread_mutex_lock(&sws->jobs_mutex);

    while (true) {
        // Wait while the queue is empty
        while (sws->job_count == 0 && !sws->shutdown_requested) {
            pthread_cond_wait(&sws->cond_has_jobs, &sws->jobs_mutex);
        }
        
        if (sws->shutdown_requested) {
            pthread_mutex_unlock(&sws->jobs_mutex);
            break;
        }

        // Pull job and process it
        // we don't care about the job number so
        // we can just remove it from the array
        memmove(sws->jobs, sws->jobs + 1, sws->job_count * sizeof(uint64_t));
        sws->job_count -= 1;
        pthread_mutex_unlock(&sws->jobs_mutex);

        // Simulate processing a job
        struct timespec ts = { .tv_sec = 2, .tv_nsec = (rand() % 1000) * 1000000L };
        nanosleep(&ts, NULL);

        // Send signal to parent that the job is finished
        uint64_t one = 1;
        int n = write(jobs_finished_eventfd, &one, sizeof(one));

        if (n == -1) {
            perror("eventfd write failed");
        }
    }

    return NULL;
}

int main() {
    pthread_t workers[WORKER_COUNT] = {};

    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        perror("pthread_sigmask");
        exit(EXIT_FAILURE);
    }

    int sfd = signalfd(-1, &mask, 0);
    
    if (sfd < 0) {
        perror("signalfd");
        exit(EXIT_FAILURE);
    }
    
    SharedWorkerState sws = {0};
    pthread_mutex_init(&sws.jobs_mutex, NULL);
    pthread_cond_init(&sws.cond_has_jobs, NULL);
    sws.shutdown_requested = false;

    jobs_finished_eventfd = eventfd(0, 0);

    size_t jobs_finished_total = 0;
    size_t jobs_unfinished_total = 0;

    // Make stdin non-blocking
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    struct pollfd pollfds[] = {
        (struct pollfd) { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 },
        (struct pollfd) { .fd = jobs_finished_eventfd, .events = POLLIN, .revents = 0 },
        (struct pollfd) { .fd = sfd, .events = POLLIN, .revents = 0 },
    };

    for (size_t i = 0; i < WORKER_COUNT; i += 1) {
        if (pthread_create(&workers[i], NULL, worker_runner, &sws) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    char buffer[1024] = {0};
    size_t job = 0;

    bool shutdown_initiated = false;
    size_t post_shutdown_jobs_remaining = 0;

    while (true) {
        int n = poll(pollfds, sizeof(pollfds) / sizeof(pollfds[0]), -1);

        if (n < 0) {
            perror("poll");
            continue;
        }

        if (n == 0) {
            fprintf(stderr, "Poll has 0 events for some reason\n");
        }

        // Stdin jobs coming in
        if (pollfds[0].revents & POLLIN) {
            pthread_mutex_lock(&sws.jobs_mutex);
            int queue_space_remaining = JOB_QUEUE_CAPACITY - sws.job_count;

            // If the queue has no space then we should stop listening for POLLIN on stdin
            if (queue_space_remaining == 0) {
                pollfds[0].events = 0;
                pthread_mutex_unlock(&sws.jobs_mutex);
                continue;
            }

            int n = read(pollfds[0].fd, buffer, queue_space_remaining);

            if (n == 0) {
                // EOF received: perform graceful shutdown
                goto shutdown;
            }

            if (n < 0) {
                // No data available
                if (errno == EAGAIN) {
                    continue;
                }

                perror("stdin read");
                continue;
            }
            
            // Push jobs
            for (size_t i = 0; i < n; i += 1) {
                sws.jobs[sws.job_count] = job;
                sws.job_count += 1;
                job += 1;
            }
            
            fprintf(stderr, "Pushed %d jobs to the queue...\n", n);
            pthread_cond_broadcast(&sws.cond_has_jobs);

            pthread_mutex_unlock(&sws.jobs_mutex);
        }
        
        // Workers reporting finished jobs
        if (pollfds[1].revents & POLLIN) {
            // We could lock the job queue here and check how much space we have but it doesn't really matter
            // we just need to re-enable POLLIN for stdin and the next iteration of the loop will take care
            // of populating the job queue
            uint64_t jobs_finished;
            int n = read(jobs_finished_eventfd, &jobs_finished, sizeof(uint64_t));
            
            if (n == -1) { 
                // Technically should never happen
                perror("eventfd read");
                continue;
            }

            jobs_finished_total += jobs_finished;
            fprintf(stderr, "Workers finished %zu jobs\n", jobs_finished);
            
            if (! shutdown_initiated) {
                pollfds[0].events = POLLIN;
            }

            if (shutdown_initiated) {
                post_shutdown_jobs_remaining -= jobs_finished;

                if (post_shutdown_jobs_remaining <= 0) {
                    break;
                }
            }
        }

        if ((pollfds[2].revents & POLLIN) && ! shutdown_initiated) {
shutdown:
            // Shutdown was initiated
            shutdown_initiated = true;
            pollfds[0].events = 0;
            fprintf(stderr, "---------------------------------\n");
            fprintf(stderr, "Shutdown initiated...\n\tRemaining jobs in queue: %zu\n", post_shutdown_jobs_remaining);

            pthread_mutex_lock(&sws.jobs_mutex);
            post_shutdown_jobs_remaining = sws.job_count;
            pthread_mutex_unlock(&sws.jobs_mutex);

            // Drain stdin and close it
            while (true) {
                int n = read(pollfds[0].fd, buffer, sizeof(buffer));

                if (n == 0) {
                    break;
                }

                if (n < 0) {
                    // No data available, this probably means that we
                    // can safely close stdin
                    if (errno == EAGAIN) {
                        break;
                    }

                    perror("stdin read");
                    exit(EXIT_FAILURE);
                }
                
                jobs_unfinished_total += n;

            }
            fprintf(stderr, "\tDrained %zu unfinished jobs from stdin\n", jobs_unfinished_total);

            close(STDIN_FILENO);

            // No jobs in queue then we can safely break
            if (post_shutdown_jobs_remaining == 0) {
                break;
            }
        }
    }

    pthread_mutex_lock(&sws.jobs_mutex);
    sws.shutdown_requested = true;
    pthread_mutex_unlock(&sws.jobs_mutex);
    pthread_cond_broadcast(&sws.cond_has_jobs);
    
    fprintf(stderr, "Waiting for workers to shut down\n");
    for (size_t i = 0; i < WORKER_COUNT; i += 1) {
        if (pthread_join(workers[i], NULL) != 0) {
            perror("pthread_join");
            return 1;
        }
    }

    printf("Summary:\n\tFinished jobs: %zu\n\tUnfinished jobs: %zu", jobs_finished_total, jobs_unfinished_total);

    return 0;
}
