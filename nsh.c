#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>

/* ── ANSI colour codes ── */
#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_GREEN "\033[32m"
#define C_BLUE  "\033[34m"
#define C_CYAN  "\033[36m"

#define MAX_LINE      80
#define MAX_COMMANDS  32
#define HIST_INIT_LEN 100
#define READ_END      0
#define WRITE_END     1

typedef struct {
    char **entries;
    int    count;
    int    capacity;
} History;

void hist_init(History *h) {
    h->entries  = (char **)calloc(HIST_INIT_LEN, sizeof(char *));
    h->count    = 0;
    h->capacity = HIST_INIT_LEN;
}

void hist_add(History *h, const char *cmd) {
    if (!cmd || cmd[0] == '\0') return;
    if (h->count >= h->capacity) {
        h->capacity *= 2;
        h->entries = (char **)realloc(h->entries, h->capacity * sizeof(char *));
    }
    h->entries[h->count++] = strdup(cmd);
}

void hist_print(const History *h) {
    int i;
    for (i = 0; i < h->count; i++)
        printf("%4d  %s\n", i + 1, h->entries[i]);
}

void hist_clear(History *h) {
    int i;
    for (i = 0; i < h->count; i++) { free(h->entries[i]); h->entries[i] = NULL; }
    h->count = 0;
    printf("History cleared.\n");
}

void hist_free(History *h) {
    hist_clear(h);
    free(h->entries);
    h->entries = NULL;
}

int read_input(char input_buffer[], History *h) {
    char path[PATH_MAX];
    char hostname[256];
    char *user;

    if (getcwd(path, sizeof(path)) == NULL) strcpy(path, "?");
    if (gethostname(hostname, sizeof(hostname)) != 0) strcpy(hostname, "localhost");
    user = getenv("USER");
    if (!user) user = "user";

    /* coloured prompt: user@host:path$ */
    printf(C_BOLD C_GREEN "%s@%s" C_RESET ":" C_BOLD C_BLUE "%s" C_RESET "$ ",
           user, hostname, path);
    fflush(stdout);

    memset(input_buffer, '\0', MAX_LINE);
    int length = read(STDIN_FILENO, input_buffer, MAX_LINE - 1);
    if (length < 0) { perror("\ncommand-reading failed"); exit(-1); }

    if (length > 0 && input_buffer[length - 1] == '\n')
        input_buffer[--length] = '\0';
    if (length == 0) return 0;

    /* !! - repeat last command */
    if (strncmp(input_buffer, "!!", 2) == 0 && input_buffer[2] == '\0') {
        if (h->count == 0) { fprintf(stderr, "nsh: !!: no previous command\n"); return 0; }
        strncpy(input_buffer, h->entries[h->count - 1], MAX_LINE - 1);
        printf("%s\n", input_buffer);
        return strlen(input_buffer);
    }

    /* !n - repeat n-th command */
    if (input_buffer[0] == '!' && length > 1) {
        int n = atoi(&input_buffer[1]);
        if (n > 0 && n <= h->count) {
            strncpy(input_buffer, h->entries[n - 1], MAX_LINE - 1);
            printf("%s\n", input_buffer);
            return strlen(input_buffer);
        } else {
            fprintf(stderr, "nsh: !%d: event not found\n", n);
            return 0;
        }
    }

    return length;
}

int split_commands(char *buf, char *commands[], int max_cmd) {
    int count = 0, len = strlen(buf), start = 0, i;
    char in_sq = 0, in_dq = 0;
    for (i = 0; i <= len && count < max_cmd; i++) {
        char c = buf[i];
        if (c == '\'' && !in_dq) { in_sq = !in_sq; continue; }
        if (c == '"'  && !in_sq) { in_dq = !in_dq; continue; }
        if ((c == ';' || c == '\0') && !in_sq && !in_dq) {
            buf[i] = '\0';
            while (buf[start] == ' ' || buf[start] == '\t') start++;
            if (buf[start] != '\0') commands[count++] = &buf[start];
            start = i + 1;
        }
    }
    return count;
}

int parse_args(char *cmd, char *args[], int *background) {
    int next = 0, length = strlen(cmd), i = 0;
    while (i < length) {
        while (i < length && (cmd[i] == ' ' || cmd[i] == '\t')) i++;
        if (i >= length) break;
        if (cmd[i] == '&') { *background = 1; cmd[i++] = '\0'; continue; }
        if (cmd[i] == '\'') {
            i++; args[next++] = &cmd[i];
            while (i < length && cmd[i] != '\'') i++;
            cmd[i++] = '\0'; continue;
        }
        if (cmd[i] == '"') {
            i++; args[next++] = &cmd[i];
            while (i < length && cmd[i] != '"') i++;
            cmd[i++] = '\0'; continue;
        }
        args[next++] = &cmd[i];
        while (i < length && cmd[i] != ' ' && cmd[i] != '\t' &&
               cmd[i] != '\'' && cmd[i] != '"' && cmd[i] != '&') i++;
        if (i < length && cmd[i] != '\'' && cmd[i] != '"') cmd[i++] = '\0';
    }
    args[next] = NULL;
    return next;
}

int has_pipe(char **args) {
    int i;
    for (i = 0; args[i] != NULL; i++)
        if (strcmp(args[i], "|") == 0) return 1;
    return 0;
}

void remove_redir_tokens(char **args, int idx) {
    int i;
    for (i = idx; args[i + 2] != NULL; i++) args[i] = args[i + 2];
    args[i] = NULL; args[i + 1] = NULL;
}

void apply_redirection(char **args) {
    int i = 0;
    while (args[i] != NULL) {
        if (strcmp(args[i], "<") == 0) {
            if (!args[i+1]) { fprintf(stderr,"nsh: missing input file\n"); exit(-1); }
            int fd = open(args[i+1], O_RDONLY);
            if (fd < 0) { perror("open"); exit(-1); }
            dup2(fd, STDIN_FILENO); close(fd);
            remove_redir_tokens(args, i);
        } else if (strcmp(args[i], ">>") == 0) {
            if (!args[i+1]) { fprintf(stderr,"nsh: missing output file\n"); exit(-1); }
            int fd = open(args[i+1], O_CREAT|O_WRONLY|O_APPEND, 0644);
            if (fd < 0) { perror("open"); exit(-1); }
            dup2(fd, STDOUT_FILENO); close(fd);
            remove_redir_tokens(args, i);
        } else if (strcmp(args[i], "2>") == 0) {
            if (!args[i+1]) { fprintf(stderr,"nsh: missing error file\n"); exit(-1); }
            int fd = open(args[i+1], O_CREAT|O_WRONLY|O_TRUNC, 0644);
            if (fd < 0) { perror("open"); exit(-1); }
            dup2(fd, STDERR_FILENO); close(fd);
            remove_redir_tokens(args, i);
        } else if (strcmp(args[i], ">") == 0) {
            if (!args[i+1]) { fprintf(stderr,"nsh: missing output file\n"); exit(-1); }
            int fd = open(args[i+1], O_CREAT|O_WRONLY|O_TRUNC, 0644);
            if (fd < 0) { perror("open"); exit(-1); }
            dup2(fd, STDOUT_FILENO); close(fd);
            remove_redir_tokens(args, i);
        } else {
            i++;
        }
    }
}

void execute_single(char **args) {
    apply_redirection(args);
    if (args[0] == NULL) return;
    if (execvp(args[0], args) != 0) {
        fprintf(stderr, "nsh: %s: %s\n", args[0], strerror(errno));
        exit(-1);
    }
}

void execute_pipe_recursive(char **args) {
    int i;
    for (i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0) {
            args[i] = NULL;
            char **right = &args[i + 1];
            int fd[2];
            if (pipe(fd) < 0) { perror("pipe"); exit(-1); }
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); exit(-1); }
            if (pid == 0) {
                close(fd[READ_END]);
                dup2(fd[WRITE_END], STDOUT_FILENO);
                close(fd[WRITE_END]);
                execute_single(args);
                exit(-1);
            } else {
                close(fd[WRITE_END]);
                dup2(fd[READ_END], STDIN_FILENO);
                close(fd[READ_END]);
                execute_pipe_recursive(right);
            }
            return;
        }
    }
    execute_single(args);
}

int handle_builtin(char **args, History *h) {
    if (args[0] == NULL) return 0;

    if (strcmp(args[0], "cd") == 0) {
        const char *target = args[1] ? args[1] : getenv("HOME");
        if (!target) target = "/";
        if (chdir(target) < 0)
            fprintf(stderr, "nsh: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    if (strcmp(args[0], "pwd") == 0) {
        char path[PATH_MAX];
        if (getcwd(path, sizeof(path))) printf("%s\n", path);
        else perror("pwd");
        return 1;
    }

    if (strcmp(args[0], "history") == 0) {
        if (args[1] && strcmp(args[1], "-c") == 0) hist_clear(h);
        else hist_print(h);
        return 1;
    }

    return 0;
}

void execute(char **args, int *background, History *h) {
    if (args[0] == NULL) return;
    if (handle_builtin(args, h)) return;

    pid_t pid = fork();
    switch (pid) {
        case -1: perror("fork"); break;
        case 0:
            /* restore default signal handlers so Ctrl+C kills child */
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            if (has_pipe(args)) execute_pipe_recursive(args);
            else execute_single(args);
            exit(EXIT_SUCCESS);
        default:
            if (!*background) waitpid(pid, NULL, 0);
    }
}

int main(void) {
    char  input_buffer[MAX_LINE];
    char  cmd_copy[MAX_LINE];
    char *commands[MAX_COMMANDS];
    char *args[MAX_LINE / 2 + 1];
    int   background, cmd_count, i;
    History hist;

    hist_init(&hist);

    /*
     * Step 6 — Signal handling
     * Parent shell ignores SIGINT (Ctrl+C) and SIGQUIT (Ctrl+\).
     * Child processes inherit the default handlers (reset before exec),
     * so Ctrl+C kills only the foreground child, not the shell itself.
     */
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    while (1) {
        /*
         * Step 6 — Zombie reaping
         * Before each prompt, collect any finished background children
         * with WNOHANG so they don't accumulate as zombies.
         */
        pid_t zp;
        while ((zp = waitpid(-1, NULL, WNOHANG)) > 0)
            printf("\n[done] pid %d\n", zp);

        background = 0;

        int len = read_input(input_buffer, &hist);
        if (len == 0) continue;

        if (strcmp(input_buffer, "exit") == 0 || strcmp(input_buffer, "quit") == 0) {
            hist_free(&hist);
            break;
        }

        hist_add(&hist, input_buffer);

        strncpy(cmd_copy, input_buffer, MAX_LINE - 1);
        cmd_copy[MAX_LINE - 1] = '\0';
        cmd_count = split_commands(cmd_copy, commands, MAX_COMMANDS);

        for (i = 0; i < cmd_count; i++) {
            background = 0;
            char seg[MAX_LINE];
            strncpy(seg, commands[i], MAX_LINE - 1);
            seg[MAX_LINE - 1] = '\0';
            if (parse_args(seg, args, &background) == 0) continue;
            execute(args, &background, &hist);
        }
    }

    return 0;
}
