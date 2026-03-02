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
#include <termios.h>

/* ── ANSI colour codes ── */
#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_RED    "\033[31m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_BLUE   "\033[34m"
#define C_CYAN   "\033[36m"

#define MAX_LINE      80
#define MAX_COMMANDS  32
#define HIST_INIT_LEN 100
#define READ_END      0
#define WRITE_END     1

/* ─────────────────────────────────────────────
 * History
 * ───────────────────────────────────────────── */

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
    /* skip duplicate of last entry */
    if (h->count > 0 && strcmp(h->entries[h->count - 1], cmd) == 0) return;
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

/* ─────────────────────────────────────────────
 * Banners
 * ───────────────────────────────────────────── */

void print_welcome(void) {
    printf(C_CYAN C_BOLD);
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║                                                      ║\n");
    printf("║          Welcome to nsh — Nahyun Shell  (˶ᵔ ᵕ ᵔ˶)    ║\n");
    printf("║     Type a command, or 'exit' / 'quit' to leave.     ║\n");
    printf("║                                                      ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf(C_RESET "\n");
}

void print_farewell(void) {
    printf("\n" C_CYAN C_BOLD);
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║                                                      ║\n");
    printf("║          Bye! See you again~  ૮ ˶ᵔ ᵕ ᵔ˶ ა            ║\n");
    printf("║                                                      ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf(C_RESET "\n");
}

/* ─────────────────────────────────────────────
 * Prompt builder
 * ───────────────────────────────────────────── */

void build_prompt(char *out, int size) {
    char path[PATH_MAX];
    char host[256];
    char *user;

    if (getcwd(path, sizeof(path)) == NULL) strcpy(path, "?");
    if (gethostname(host, sizeof(host)) != 0) strcpy(host, "localhost");
    /* strip domain from hostname */
    char *dot = strchr(host, '.');
    if (dot) *dot = '\0';

    user = getenv("USER");
    if (!user) user = getenv("LOGNAME");
    if (!user) user = "user";

    snprintf(out, size,
             C_BOLD C_GREEN "%s@%s" C_RESET ":"
             C_BOLD C_BLUE  "%s"   C_RESET "$ ",
             user, host, path);
}

/* ─────────────────────────────────────────────
 * Raw-mode line reader
 * Supports:
 *   - Printable chars + Backspace
 *   - UP / DOWN  : history navigation
 *   - LEFT / RIGHT: cursor movement within line
 *   - Ctrl+C     : clear current line
 *   - Ctrl+D     : EOF (exit shell)
 * ───────────────────────────────────────────── */

int read_line(char *buf, int maxlen, History *h) {
    struct termios raw, orig;
    char   prompt[512];
    char   saved[MAX_LINE]; /* saved partial input when browsing history */
    int    len      = 0;    /* current line length */
    int    cursor   = 0;    /* cursor position (0 = start) */
    int    hist_pos;        /* position in history (-1 = none browsed) */

    memset(buf,   0, maxlen);
    memset(saved, 0, sizeof(saved));
    hist_pos = h->count;   /* one past the last entry = "new input" */

    build_prompt(prompt, sizeof(prompt));
    printf("%s", prompt);
    fflush(stdout);

    /* enter raw mode */
    tcgetattr(STDIN_FILENO, &orig);
    raw = orig;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    while (1) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) {
            /* EOF */
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            printf("\n");
            strcpy(buf, "exit");
            return 4;
        }

        /* ── Enter ── */
        if (c == '\n' || c == '\r') {
            printf("\n");
            fflush(stdout);
            break;
        }

        /* ── Ctrl+C : clear line ── */
        if (c == 3) {
            printf("^C\n");
            fflush(stdout);
            buf[0] = '\0';
            len = cursor = 0;
            hist_pos = h->count;
            build_prompt(prompt, sizeof(prompt));
            printf("%s", prompt);
            fflush(stdout);
            continue;
        }

        /* ── Ctrl+D on empty line : exit ── */
        if (c == 4 && len == 0) {
            printf("\n");
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            strcpy(buf, "exit");
            return 4;
        }

        /* ── Backspace / DEL ── */
        if (c == 127 || c == 8) {
            if (cursor > 0) {
                /* remove char before cursor */
                memmove(buf + cursor - 1, buf + cursor, len - cursor);
                len--;
                cursor--;
                buf[len] = '\0';
                /* redraw from cursor position */
                printf("\033[1D");              /* move left 1 */
                printf("\033[K%s", buf + cursor); /* clear to EOL, reprint tail */
                if (len > cursor)
                    printf("\033[%dD", len - cursor); /* reposition cursor */
                fflush(stdout);
            }
            continue;
        }

        /* ── Escape sequence (arrow keys etc.) ── */
        if (c == '\033') {
            unsigned char seq[3] = {0};
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (seq[0] != '[') continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;

            if (seq[1] == 'A') {
                /* ── UP : go back in history ── */
                if (hist_pos > 0) {
                    if (hist_pos == h->count)          /* save what user typed */
                        strncpy(saved, buf, MAX_LINE - 1);
                    hist_pos--;
                    strncpy(buf, h->entries[hist_pos], maxlen - 1);
                    len = cursor = strlen(buf);
                    build_prompt(prompt, sizeof(prompt));
                    printf("\r\033[K%s%s", prompt, buf);
                    fflush(stdout);
                }

            } else if (seq[1] == 'B') {
                /* ── DOWN : go forward in history ── */
                if (hist_pos < h->count) {
                    hist_pos++;
                    if (hist_pos == h->count)
                        strncpy(buf, saved, maxlen - 1);  /* restore saved */
                    else
                        strncpy(buf, h->entries[hist_pos], maxlen - 1);
                    len = cursor = strlen(buf);
                    build_prompt(prompt, sizeof(prompt));
                    printf("\r\033[K%s%s", prompt, buf);
                    fflush(stdout);
                }

            } else if (seq[1] == 'C') {
                /* ── RIGHT : move cursor right ── */
                if (cursor < len) {
                    cursor++;
                    printf("\033[C");
                    fflush(stdout);
                }

            } else if (seq[1] == 'D') {
                /* ── LEFT : move cursor left ── */
                if (cursor > 0) {
                    cursor--;
                    printf("\033[D");
                    fflush(stdout);
                }
            }
            continue;
        }

        /* ── Printable character : insert at cursor ── */
        if (c >= 32 && c < 127 && len < maxlen - 1) {
            memmove(buf + cursor + 1, buf + cursor, len - cursor);
            buf[cursor] = c;
            len++;
            buf[len] = '\0';
            /* print char + tail, reposition cursor */
            printf("\033[K%s", buf + cursor);
            cursor++;
            if (cursor < len)
                printf("\033[%dD", len - cursor);
            fflush(stdout);
        }
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    buf[len] = '\0';
    return len;
}

/* ─────────────────────────────────────────────
 * Command input (wraps read_line + history expansion)
 * ───────────────────────────────────────────── */

int read_input(char *buf, History *h) {
    memset(buf, 0, MAX_LINE);
    int len = read_line(buf, MAX_LINE, h);
    if (len == 0 && buf[0] == '\0') return 0;

    /* !! — repeat last command */
    if (strcmp(buf, "!!") == 0) {
        if (h->count == 0) {
            fprintf(stderr, "nsh: !!: no previous command\n");
            return 0;
        }
        strncpy(buf, h->entries[h->count - 1], MAX_LINE - 1);
        printf("%s\n", buf);
        return strlen(buf);
    }

    /* !n — repeat n-th command */
    if (buf[0] == '!' && buf[1] != '\0') {
        int n = atoi(&buf[1]);
        if (n > 0 && n <= h->count) {
            strncpy(buf, h->entries[n - 1], MAX_LINE - 1);
            printf("%s\n", buf);
            return strlen(buf);
        } else {
            fprintf(stderr, "nsh: !%d: event not found\n", n);
            return 0;
        }
    }

    return strlen(buf);
}

/* ─────────────────────────────────────────────
 * Command splitting on ';'
 * (respects single/double quotes)
 * ───────────────────────────────────────────── */

int split_commands(char *buf, char *cmds[], int max_cmd) {
    int  count = 0, len = strlen(buf), start = 0, i;
    char in_sq = 0, in_dq = 0;

    for (i = 0; i <= len && count < max_cmd; i++) {
        char c = buf[i];
        if      (c == '\'' && !in_dq) { in_sq = !in_sq; continue; }
        else if (c == '"'  && !in_sq) { in_dq = !in_dq; continue; }

        if ((c == ';' || c == '\0') && !in_sq && !in_dq) {
            buf[i] = '\0';
            while (buf[start] == ' ' || buf[start] == '\t') start++;
            if (buf[start] != '\0') cmds[count++] = &buf[start];
            start = i + 1;
        }
    }
    return count;
}

/* ─────────────────────────────────────────────
 * Argument parsing
 * Handles:
 *   - whitespace delimiters
 *   - single quotes  : ' ... '  (double quotes inside are literal)
 *   - double quotes  : " ... "  (single quotes inside are literal)
 *   - nested example : "it's ok"  or  'say "hi"'
 *   - background flag: &
 * ───────────────────────────────────────────── */

int parse_args(char *cmd, char *args[], int *background) {
    int next = 0, length = strlen(cmd), i = 0;

    while (i < length) {
        /* skip whitespace */
        while (i < length && (cmd[i] == ' ' || cmd[i] == '\t')) i++;
        if (i >= length) break;

        /* background flag */
        if (cmd[i] == '&') { *background = 1; cmd[i++] = '\0'; continue; }

        /* single-quoted token: double quotes inside are literal */
        if (cmd[i] == '\'') {
            i++;
            args[next++] = &cmd[i];
            while (i < length && cmd[i] != '\'') i++;
            if (i < length) cmd[i++] = '\0';
            continue;
        }

        /* double-quoted token: single quotes inside are literal */
        if (cmd[i] == '"') {
            i++;
            args[next++] = &cmd[i];
            while (i < length && cmd[i] != '"') i++;
            if (i < length) cmd[i++] = '\0';
            continue;
        }

        /* normal token */
        args[next++] = &cmd[i];
        while (i < length &&
               cmd[i] != ' ' && cmd[i] != '\t' &&
               cmd[i] != '\'' && cmd[i] != '"' && cmd[i] != '&')
            i++;
        if (i < length && cmd[i] != '\'' && cmd[i] != '"')
            cmd[i++] = '\0';
    }

    args[next] = NULL;
    return next;
}

/* ─────────────────────────────────────────────
 * Redirection helpers
 * ───────────────────────────────────────────── */

int has_pipe(char **args) {
    int i;
    for (i = 0; args[i]; i++)
        if (strcmp(args[i], "|") == 0) return 1;
    return 0;
}

void remove_redir_tokens(char **args, int idx) {
    int i;
    for (i = idx; args[i + 2] != NULL; i++) args[i] = args[i + 2];
    args[i] = args[i + 1] = NULL;
}

/*
 * Apply I/O redirection in-place.
 * Supported: <   >   >>   2>
 * Tokens are shift-removed so execvp gets a clean args[].
 */
void apply_redirection(char **args) {
    int i = 0;
    while (args[i] != NULL) {
        if (strcmp(args[i], "<") == 0) {
            if (!args[i+1]) { fprintf(stderr,"nsh: missing file\n"); exit(-1); }
            int fd = open(args[i+1], O_RDONLY);
            if (fd < 0) { perror("open"); exit(-1); }
            dup2(fd, STDIN_FILENO); close(fd);
            remove_redir_tokens(args, i);
        } else if (strcmp(args[i], ">>") == 0) {
            if (!args[i+1]) { fprintf(stderr,"nsh: missing file\n"); exit(-1); }
            int fd = open(args[i+1], O_CREAT|O_WRONLY|O_APPEND, 0644);
            if (fd < 0) { perror("open"); exit(-1); }
            dup2(fd, STDOUT_FILENO); close(fd);
            remove_redir_tokens(args, i);
        } else if (strcmp(args[i], "2>") == 0) {
            if (!args[i+1]) { fprintf(stderr,"nsh: missing file\n"); exit(-1); }
            int fd = open(args[i+1], O_CREAT|O_WRONLY|O_TRUNC, 0644);
            if (fd < 0) { perror("open"); exit(-1); }
            dup2(fd, STDERR_FILENO); close(fd);
            remove_redir_tokens(args, i);
        } else if (strcmp(args[i], ">") == 0) {
            if (!args[i+1]) { fprintf(stderr,"nsh: missing file\n"); exit(-1); }
            int fd = open(args[i+1], O_CREAT|O_WRONLY|O_TRUNC, 0644);
            if (fd < 0) { perror("open"); exit(-1); }
            dup2(fd, STDOUT_FILENO); close(fd);
            remove_redir_tokens(args, i);
        } else {
            i++;
        }
    }
}

/* ─────────────────────────────────────────────
 * Execution
 * ───────────────────────────────────────────── */

void execute_single(char **args) {
    apply_redirection(args);
    if (args[0] == NULL) return;
    if (execvp(args[0], args) != 0) {
        fprintf(stderr, "nsh: %s: %s\n", args[0], strerror(errno));
        exit(-1);
    }
}

/*
 * Recursively execute a multi-stage pipeline.
 * Base case (no '|'): execute_single().
 */
void execute_pipe_recursive(char **args) {
    int i;
    for (i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0) {
            args[i] = NULL;
            char **right = &args[i + 1];
            int fd[2];
            if (pipe(fd) < 0) { perror("pipe"); exit(-1); }
            pid_t pid = fork();
            if (pid < 0)  { perror("fork"); exit(-1); }
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

/*
 * Handle built-in commands in the parent process.
 * Returns 1 if handled, 0 otherwise.
 */
int handle_builtin(char **args, History *h) {
    if (!args[0]) return 0;

    if (strcmp(args[0], "cd") == 0) {
        const char *t = args[1] ? args[1] : getenv("HOME");
        if (!t) t = "/";
        if (chdir(t) < 0)
            fprintf(stderr, "nsh: cd: %s: %s\n", t, strerror(errno));
        return 1;
    }

    if (strcmp(args[0], "pwd") == 0) {
        char p[PATH_MAX];
        if (getcwd(p, sizeof(p))) printf("%s\n", p);
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
    if (!args[0]) return;
    if (handle_builtin(args, h)) return;

    pid_t pid = fork();
    switch (pid) {
        case -1: perror("fork"); break;
        case 0:
            /* restore default signal handlers in child */
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            if (has_pipe(args)) execute_pipe_recursive(args);
            else                execute_single(args);
            exit(EXIT_SUCCESS);
        default:
            if (!*background)
                waitpid(pid, NULL, 0);
    }
}

/* ─────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────── */

int main(void) {
    char  input_buffer[MAX_LINE];
    char  cmd_copy[MAX_LINE];
    char *commands[MAX_COMMANDS];
    char *args[MAX_LINE / 2 + 1];
    int   background, cmd_count, i;
    History hist;

    hist_init(&hist);

    /* ignore SIGINT/SIGQUIT in parent shell */
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    print_welcome();

    while (1) {
        /* reap finished background children */
        pid_t zp;
        while ((zp = waitpid(-1, NULL, WNOHANG)) > 0)
            printf(C_YELLOW "[done] pid %d\n" C_RESET, zp);

        background = 0;

        int len = read_input(input_buffer, &hist);
        if (len == 0) continue;

        /* exit / quit */
        if (strcmp(input_buffer, "exit") == 0 ||
            strcmp(input_buffer, "quit") == 0) {
            hist_free(&hist);
            print_farewell();
            break;
        }

        /* save to history */
        hist_add(&hist, input_buffer);

        /* split on ';' */
        strncpy(cmd_copy, input_buffer, MAX_LINE - 1);
        cmd_copy[MAX_LINE - 1] = '\0';
        cmd_count = split_commands(cmd_copy, commands, MAX_COMMANDS);

        /* execute each command */
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
