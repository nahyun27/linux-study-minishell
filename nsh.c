#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_LINE 80 /* The maximum length command */
#define READ_END 0  /* The read end of the pipe  */
#define WRITE_END 1 /* The write end of the pipe */

/*
 * Read and parse a command line into args[].
 * Handles:
 *   - whitespace as token delimiters
 *   - single quotes: 'foo bar' → one token, quotes stripped
 *   - double quotes: "foo bar" → one token, quotes stripped
 *   - '&' suffix for background execution
 */
int read_input(char input_buffer[], char *args[], int *background) {
    int length;
    int next;

    printf("nsh> ");
    fflush(stdout);
    length = read(STDIN_FILENO, input_buffer, MAX_LINE);

    if (length < 0) {
        perror("\ncommand-reading failed");
        exit(-1);
    }

    /* null-terminate at newline */
    int len = length;
    int j;
    for (j = 0; j < len; j++) {
        if (input_buffer[j] == '\n') {
            input_buffer[j] = '\0';
            length = j;
            break;
        }
    }

    next = 0;
    int i = 0;

    while (i < length) {
        /* skip whitespace */
        while (i < length && (input_buffer[i] == ' ' || input_buffer[i] == '\t'))
            i++;

        if (i >= length) break;

        /* background flag */
        if (input_buffer[i] == '&') {
            *background = 1;
            input_buffer[i] = '\0';
            i++;
            continue;
        }

        /* single-quoted token: strip quotes, treat content as one token */
        if (input_buffer[i] == '\'') {
            i++; /* skip opening quote */
            args[next++] = &input_buffer[i];
            while (i < length && input_buffer[i] != '\'')
                i++;
            input_buffer[i] = '\0'; /* overwrite closing quote */
            i++;
            continue;
        }

        /* double-quoted token: strip quotes, treat content as one token */
        if (input_buffer[i] == '"') {
            i++; /* skip opening quote */
            args[next++] = &input_buffer[i];
            while (i < length && input_buffer[i] != '"')
                i++;
            input_buffer[i] = '\0'; /* overwrite closing quote */
            i++;
            continue;
        }

        /* normal token: ends at whitespace, quote, or '&' */
        args[next++] = &input_buffer[i];
        while (i < length &&
               input_buffer[i] != ' '  &&
               input_buffer[i] != '\t' &&
               input_buffer[i] != '\'' &&
               input_buffer[i] != '"'  &&
               input_buffer[i] != '&')
            i++;

        if (i < length && input_buffer[i] != '\'' && input_buffer[i] != '"') {
            input_buffer[i] = '\0';
            i++;
        }
    }

    args[next] = NULL;
    return 1;
}

/* return 1 if args contains a pipe symbol '|', 0 otherwise */
int has_pipe(char **args) {
    int i;
    for (i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0)
            return 1;
    }
    return 0;
}

/*
 * Remove args[idx] and args[idx+1] (redirection symbol + filename) by
 * shifting all subsequent pointers left by 2.  This avoids NULL holes
 * so execvp always receives a clean, contiguous argument list.
 */
void remove_redir_tokens(char **args, int idx) {
    int i;
    for (i = idx; args[i + 2] != NULL; i++)
        args[i] = args[i + 2];
    /* terminate the shifted array */
    args[i]     = NULL;
    args[i + 1] = NULL;
}

/*
 * Apply I/O redirection found in args.
 * Handles:  command < file
 *           command > file
 *           command < file1 > file2
 * Redirection tokens and their filenames are shifted out of args in-place,
 * so no NULL holes are left — execvp receives a clean argument list.
 */
void apply_redirection(char **args) {
    int i = 0;
    while (args[i] != NULL) {
        if (strcmp(args[i], "<") == 0) {
            if (args[i + 1] == NULL) {
                fprintf(stderr, "nsh: missing input file\n");
                exit(-1);
            }
            int fd = open(args[i + 1], O_RDONLY, 0);
            if (fd < 0) {
                perror("open failed");
                exit(-1);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
            remove_redir_tokens(args, i);
            /* don't advance i — the slot now holds the next token */

        } else if (strcmp(args[i], ">") == 0) {
            if (args[i + 1] == NULL) {
                fprintf(stderr, "nsh: missing output file\n");
                exit(-1);
            }
            int fd = creat(args[i + 1], 0644);
            if (fd < 0) {
                perror("creat failed");
                exit(-1);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            remove_redir_tokens(args, i);

        } else {
            i++;
        }
    }
}

/*
 * Execute a single command (no pipe).
 * Handles I/O redirection.  Must be called inside a child process.
 */
void execute_single(char **args) {
    apply_redirection(args);  /* shifts tokens out in-place, no holes left */

    if (args[0] == NULL) return;

    if (execvp(args[0], args) != 0) {
        perror("\nexecvp failed");
        exit(-1);
    }
}

/*
 * Recursively execute a (possibly multi-stage) pipeline.
 *
 * Strategy:
 *   - Find the first '|' and split args into left and right.
 *   - Fork a grandchild that writes to the pipe and runs left.
 *   - Current process reads from the pipe and recurses on right.
 *   - Base case (no '|'): execute the command directly with execvp.
 *
 * This function is always called from within a child process, so
 * the final execvp replaces the current process image directly.
 */
void execute_pipe_recursive(char **args) {
    int i;
    for (i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0) {
            /* split: args = left side, right = everything after '|' */
            args[i] = NULL;
            char **right = &args[i + 1];

            int fd[2];
            if (pipe(fd) < 0) {
                perror("\npipe failed");
                exit(-1);
            }

            pid_t pid = fork();
            if (pid < 0) {
                perror("\nfork failed");
                exit(-1);
            }

            if (pid == 0) {
                /* grandchild: write left side output into pipe */
                close(fd[READ_END]);
                dup2(fd[WRITE_END], STDOUT_FILENO);
                close(fd[WRITE_END]);
                execute_single(args);   /* handles redirection + execvp */
                exit(-1);              /* reached only on execvp failure */
            } else {
                /* current process: read from pipe, continue with right side */
                close(fd[WRITE_END]);
                dup2(fd[READ_END], STDIN_FILENO);
                close(fd[READ_END]);
                /* recurse: right side may contain more pipes */
                execute_pipe_recursive(right);
            }
            return; /* unreachable after execute_pipe_recursive */
        }
    }

    /* Base case: no pipe found, execute directly */
    execute_single(args);
}

/*
 * Fork and run a command (single or piped).
 * Parent waits unless background flag is set.
 */
void execute(char **args, const int *background) {
    pid_t pid = fork();

    switch (pid) {
        case -1:
            perror("\nfork failed");
            break;

        case 0:
            /* child process */
            if (has_pipe(args))
                execute_pipe_recursive(args);
            else
                execute_single(args);
            exit(EXIT_SUCCESS);

        default:
            /* parent process */
            if (*background == 0)
                wait(NULL);
    }
}

int main(void) {
    char *args[MAX_LINE / 2 + 1];  /* command line arguments */
    char input_buffer[MAX_LINE];   /* buffer for storing commands */

    int should_run;   /* flag: command was read successfully */
    int background;   /* 1 when command ends with '&' */

    while (1) {
        background = 0;
        should_run = 0;

        should_run = read_input(input_buffer, args, &background);

        if (args[0] != NULL && strcmp(args[0], "exit") == 0)
            return 0;

        if (should_run && args[0] != NULL)
            execute(args, &background);
    }
    return 0;
}
