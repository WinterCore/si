#include <stdio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

void trim_end(char **input) {
    for (int i = strlen(*input) - 1; i >= 0; i -= 1) {
        char *c = &(*input)[i];
        if (*c == ' ' || *c == '\n') {
            *c = '\0';
        } else {
            break;
        }
    }
}

void trim_start(char **input) {
    while (**input == ' ' || **input == '\n') {
        *input += 1;
    }
}

void split_at(char *input, char **left, char **right, char c, bool reverse) {
    trim_start(&input);
    trim_end(&input);
    size_t len = strlen(input);

    if (len == 0) {
        *left = NULL;
        *right = NULL;
        return;
    }

    char *p = reverse ? strrchr(input, c) : strchr(input, c);

    if (p == NULL) {
        *left = input;
        *right = NULL;
        return;
    }
    
    if (input == p) {
        // Redirect is at pos 0

        *left = NULL;
        *right = input;

        return;
    }
    
    *p = '\0';
    
    *left = input;
    trim_end(left);

    if (p + 1 >= input + len) { 
        *right = NULL;
    } else {
        *right = p + 1;
        trim_start(right);
    }
}

int split_args(char *line, char **argv, int max_args) {
      int argc = 0;
      char *tok = strtok(line, " \t\n");

      while (tok && argc < max_args - 1) {
          argv[argc++] = tok;
          tok = strtok(NULL, " \t\n");
      }

      argv[argc] = NULL;
      return argc;
}

void process_command(char *input) {
    char *command = NULL, *left = NULL, *right = NULL;

    trim_start(&input);
    trim_end(&input);

    if (strcmp("exit", input) == 0) {
        exit(EXIT_SUCCESS);
    }

    char *pipeline[50] = {NULL};

    char *rest = input;

    int pipeline_len = 0;

    // Split pipes first
    while (true) {
        // Redirect is prioritized over pipe
        split_at(rest, &left, &right, '|', false);

        if (left == NULL && right == NULL) {
            break;
        }

        if (left == NULL) {
            continue;
        }

        pipeline[pipeline_len] = left;
        pipeline_len += 1;
        rest = right;
        
        if (right == NULL) {
            break;
        }
    }
    
    pid_t pids[50];
    size_t pid_count = 0;

    int prev_read_end = -1;

    for (size_t i = 0; i < pipeline_len; i += 1) {
        char *command_part = pipeline[i];

        int fildes[2] = {-1, -1};
        
        // If there's a next entry then we need a pipe
        if (i + 1 < pipeline_len) {
            if (pipe(fildes) == -1) {
                perror("Failed to create pipe");
                exit(EXIT_FAILURE);
            }
        }

        // Handle redirects
        char *redirect = NULL;

        split_at(command_part, &command, &redirect, '>', true);

        int redirect_fd = -1;

        if (redirect != NULL) {
            redirect_fd = open(redirect, O_CREAT | O_TRUNC | O_WRONLY, 0644);
            
            if (redirect_fd == -1) {
                perror("Failed to initialize redirect");
                continue;
            }
        }

        pid_t pid = fork();

        if (pid == -1) {
            perror("Failed to fork");

            if (redirect_fd != -1) {
                close(redirect_fd);
            }
            
            // Can just continue here since it's a REPL
            return;
        }

        // Child
        if (pid == 0) {
            if (command == NULL) {
                fprintf(stderr, "Can you enter an actual command\n");
                exit(EXIT_FAILURE);
            }

            char *argv[64];
            int argc = split_args(command, argv, 64);

            // Wire stdout of previous command to stdin of current command
            if (prev_read_end != -1) {
                dup2(prev_read_end, STDIN_FILENO);
            }

            if (redirect_fd != -1) {
                // Redirect stdout to file
                dup2(redirect_fd, STDOUT_FILENO);
            } else if (fildes[1] != -1) {
                // Redirect stdout to pipe for next command
                dup2(fildes[1], STDOUT_FILENO);
            }

            int exec_result = execvp(argv[0], argv);

            if (exec_result == -1) {
                fprintf(stderr, "\n%s\n", argv[0]);
                perror("Failed to exec");
                exit(EXIT_FAILURE);
            }

            // Do things and then return
            exit(EXIT_SUCCESS);
        }

        // Store pid so we can wait on it later
        pids[pid_count] = pid;
        pid_count += 1;

        // Close read end of previous pipe if exists, it's not needed anymore cuz we just wired it to the stdin of the current command
        if (prev_read_end != -1) {
            close(prev_read_end);
        }

        prev_read_end = fildes[0];

        // Close stdout of current pipe if exists, it's not needed anymore cuz we already wired it
        if (fildes[1] != -1) {
            close(fildes[1]);
        }
    }

    while (pid_count-- > 0) {
        if (waitpid(pids[pid_count], NULL, 0) == -1) {
            // Probably don't care about failed forks here
            perror("waitpid failed");
        }
    }
}

int main(int argc, char *argv[]) {
    char buffer[1024];
    printf("Sup sucka\n");

    while (true) {
        fprintf(stdout, "> ");
        fflush(stdout);

        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            break;
        }

        process_command(buffer);

    }

    return 0;
}
