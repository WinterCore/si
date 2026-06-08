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

void split_redirect(char *input, char **command, char **redirect) {
    trim_start(&input);
    trim_end(&input);
    size_t len = strlen(input);
    char *p = strrchr(input, '>');

    if (p == NULL) {
        *command = input;
        return;
    }
    
    if (input == p) {
        // Redirect is at pos 0

        *command = NULL;
        *redirect = input;

        return;
    }
    
    *p = '\0';
    
    *command = input;
    trim_end(command);
    
    // printf("Found \n\n", p);

    if (p + 1 >= input + len) { 
        *redirect = NULL;
    } else {
        *redirect = p + 1;
        trim_start(redirect);
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

int main(int argc, char *argv[]) {
    char buffer[1024];
    printf("Sup sucka\n");

    while (true) {
        fprintf(stdout, "> ");
        fflush(stdout);

        if (fgets(buffer, 500, stdin) == NULL) {
            break;
        }

        char *command = NULL, *redirect = NULL;
        split_redirect(buffer, &command, &redirect);

        int redirect_fd = -1;
        
        if (redirect != NULL) {
            redirect_fd = open(redirect, O_WRONLY | O_CREAT, 0644);
            
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
            continue;
        }

        if (pid == 0) {
            char *argv[64];
            int argc = split_args(command, argv, 64);

            if (redirect_fd != -1) {
                dup2(redirect_fd, STDOUT_FILENO);
            }

            if (argc == 0) {
                // No command, user is a $@#$

                fprintf(stderr, "Can you enter an actual command\n");
                return EXIT_FAILURE;
            }
            
            int exec_result = execvp(argv[0], argv);

            if (exec_result == -1) {
                perror("Failed to exec");
                return EXIT_FAILURE;
            }

            // Do things and then return
            return 0;
        }

        wait(NULL);

        // Clean up

        if (redirect_fd != -1) {
            close(redirect_fd);
        }

    }

    return 0;
}
