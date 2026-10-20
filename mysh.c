/*
 * mysh - a small Unix shell.
 *
 * Features:
 *   - REPL with quoting ('...' and "..."), variables ($VAR, ${VAR}, $?, $$)
 *   - VAR=value assignments (exported), and NAME=value prefixes: FOO=1 cmd
 *   - builtins: cd (with "cd -"), exit, echo, jobs, fg, bg
 *   - redirection: < > >> N< N> N>> N>&M &> >&
 *   - pipes: |  and  |& (stderr too, per link)
 *   - command lists: ;  &&  ||  and & (anywhere on the line)
 *   - process groups, Ctrl-C / Ctrl-Z, jobs / fg / bg
 *
 * Design note: the tokenizer does NOT expand variables. Expansion happens
 * right before each pipeline runs, so `A=1; echo $A` and `false || echo $?`
 * see up-to-date values.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_TOKENS 256
#define MAX_ARGS   64
#define MAX_CMDS   16      /* stages in one pipeline */
#define MAX_LIST   16      /* pipelines in one command list */
#define MAX_JOBS   64
#define MAX_REDIRS 16

/* Marker bytes passed from the tokenizer to the expander. */
#define SQ_DOLLAR '\x01'   /* a '$' that was inside single quotes: literal */
#define QUOTED    '\x02'   /* first byte of a word that contained quotes   */

/* ---------- data structures ---------- */

typedef struct {
    int dest;            /* fd being redirected (0-9) */
    char *src;           /* filename or fd number; borrowed, never freed here */
    int src_is_fd;       /* N>&M: duplicate fd M */
    int append;          /* >> : O_APPEND */
    int dup_err;         /* &> or >&file: also send fd 2 to the same place */
    int is_input;        /* < family: open read-only */
} Redir;

typedef struct {
    char *argv[MAX_ARGS];   /* NULL-terminated; leading NAME=value words first */
    Redir redirs[MAX_REDIRS];
    int nredirs;
    int nassign;            /* how many leading argv entries are assignments */
    int err_to_next;        /* this stage's stderr goes into the pipe (|&) */
} Cmd;

typedef struct {
    Cmd cmds[MAX_CMDS];
    int n;                  /* number of stages (>= 1 once parsed) */
    int bg;
} Pipeline;

typedef enum { OP_SEMI, OP_AND, OP_OR } ListOp;

typedef struct {
    Pipeline pipes[MAX_LIST];
    ListOp   ops[MAX_LIST]; /* ops[i] sits between pipes[i] and pipes[i+1] */
    int npipes;
} CmdList;

typedef enum { RUNNING, STOPPED, DONE } JobState;

typedef struct {
    int used;
    pid_t pgid;
    pid_t pids[MAX_CMDS];   /* 0 once reaped */
    int npids, live;
    JobState state;
    int fg;                 /* currently in the foreground? */
    int notified;
    int status;             /* exit status of the last stage */
    char cmdline[256];
} Job;

static Job jobs[MAX_JOBS];
static int interactive;
static pid_t shell_pgid;
static int last_status;     /* $? : only changed by foreground work */

/* ---------- small string helpers ---------- */

static void buf_grow(char **buf, size_t *cap, size_t need) {
    if (need <= *cap) return;
    while (*cap < need) *cap *= 2;
    *buf = realloc(*buf, *cap);
}

static void buf_putc(char **buf, size_t *cap, size_t *k, char c) {
    buf_grow(buf, cap, *k + 2);
    (*buf)[(*k)++] = c;
}

static void buf_putstr(char **buf, size_t *cap, size_t *k, const char *s) {
    size_t l = strlen(s);
    buf_grow(buf, cap, *k + l + 1);
    memcpy(*buf + *k, s, l);
    *k += l;
}

static int is_number(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* Is this raw word a NAME=value assignment? (Quoted words never are.) */
static int is_assign(const char *s) {
    if (!s || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (; *s; s++) {
        if (*s == '=') return 1;
        if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    }
    return 0;
}

static void apply_assignment(const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq) return;
    char *name = strndup(kv, (size_t)(eq - kv));
    if (name) { setenv(name, eq + 1, 1); free(name); }
}

/* ---------- job table ---------- */

static Job *job_alloc(void) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (!jobs[i].used) {
            memset(&jobs[i], 0, sizeof jobs[i]);
            jobs[i].used = 1;
            return &jobs[i];
        }
    return NULL;
}

static int job_id(Job *j) { return (int)(j - jobs) + 1; }

static Job *job_by_pid(pid_t pid, int *idx) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) continue;
        for (int k = 0; k < jobs[i].npids; k++)
            if (jobs[i].pids[k] == pid) { *idx = k; return &jobs[i]; }
    }
    return NULL;
}

/* Parse "%2", "2" or NULL (most recent job). */
static Job *job_lookup(const char *arg) {
    if (arg) {
        if (*arg == '%') arg++;
        int id = atoi(arg);
        if (id < 1 || id > MAX_JOBS || !jobs[id - 1].used) return NULL;
        return &jobs[id - 1];
    }
    for (int i = MAX_JOBS - 1; i >= 0; i--)
        if (jobs[i].used && jobs[i].state != DONE) return &jobs[i];
    return NULL;
}

/* Record a waitpid() result in the job table (never touches $?). */
static void handle_status(pid_t pid, int status) {
    int k;
    Job *j = job_by_pid(pid, &k);
    if (!j) return;
    if (WIFSTOPPED(status)) { j->state = STOPPED; return; }
    j->pids[k] = 0;
    if (k == j->npids - 1)
        j->status = WIFEXITED(status) ? WEXITSTATUS(status)
                                      : 128 + WTERMSIG(status);
    if (WIFSIGNALED(status) && WTERMSIG(status) != SIGINT &&
        WTERMSIG(status) != SIGPIPE)
        fprintf(stderr, "%s\n", strsignal(WTERMSIG(status)));
    if (--j->live == 0) j->state = DONE;
}

/* Non-blocking: collect background children that changed state. */
static void reap_background(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0)
        handle_status(pid, status);
}

static void report_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (!j->used || j->fg) continue;
        if (j->state == DONE) {
            printf("[%d] Done      %s\n", i + 1, j->cmdline);
            j->used = 0;
        } else if (j->state == STOPPED && !j->notified) {
            printf("[%d] Stopped   %s\n", i + 1, j->cmdline);
            j->notified = 1;
        }
    }
}

static void give_terminal(pid_t pgid) {
    if (interactive) tcsetpgrp(STDIN_FILENO, pgid);
}

/* Block until the foreground job finishes or stops. Returns its status. */
static int wait_fg(Job *j) {
    j->fg = 1;
    give_terminal(j->pgid);
    pid_t stopped_pid = 0;
    while (j->state == RUNNING) {
        int status;
        pid_t pid = waitpid(-j->pgid, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_status(pid, status);
        if (WIFSTOPPED(status)) stopped_pid = pid;
    }
    int st = j->status;
    if (j->state == STOPPED) {
        /* Collect stop reports of the other pipeline members so stale
         * notifications don't stop the job again after `fg`. */
        for (int k = 0; k < j->npids; k++) {
            int status;
            if (j->pids[k] && j->pids[k] != stopped_pid &&
                waitpid(j->pids[k], &status, WUNTRACED) > 0)
                handle_status(j->pids[k], status);
        }
        printf("\n[%d] Stopped   %s\n", job_id(j), j->cmdline);
        j->notified = 1;
        st = 128 + SIGTSTP;
    }
    j->fg = 0;
    give_terminal(shell_pgid);
    if (j->state == DONE) j->used = 0;
    return st;
}

/* ---------- builtins ---------- */

static int is_parent_builtin(const char *s) {
    return !strcmp(s, "cd") || !strcmp(s, "exit") || !strcmp(s, "jobs") ||
           !strcmp(s, "fg") || !strcmp(s, "bg");
}

static int builtin_echo(char **argv) {
    int i = 1, newline = 1;
    if (argv[1] && !strcmp(argv[1], "-n")) { newline = 0; i = 2; }
    for (; argv[i]; i++) {
        if (i > (newline ? 1 : 2)) putchar(' ');
        fputs(argv[i], stdout);
    }
    if (newline) putchar('\n');
    fflush(stdout);
    return 0;
}

static int builtin_cd(char **argv) {
    const char *dir = argv[1] ? argv[1] : getenv("HOME");
    int print = 0;
    if (dir && !strcmp(dir, "-")) {
        dir = getenv("OLDPWD");
        print = 1;
    }
    if (!dir) { fprintf(stderr, "cd: %s not set\n", argv[1] ? "OLDPWD" : "HOME"); return 1; }

    char old[4096], cwd[4096];
    int have_old = getcwd(old, sizeof old) != NULL;
    if (chdir(dir) < 0) { fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno)); return 1; }
    if (have_old) setenv("OLDPWD", old, 1);
    if (getcwd(cwd, sizeof cwd)) {
        setenv("PWD", cwd, 1);
        if (print) puts(cwd);
    }
    return 0;
}

static int builtin_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used && jobs[i].state != DONE)
            printf("[%d] %-9s %s\n", i + 1,
                   jobs[i].state == STOPPED ? "Stopped" : "Running",
                   jobs[i].cmdline);
    return 0;
}

static int builtin_fg(char **argv) {
    Job *j = job_lookup(argv[1]);
    if (!j) { fprintf(stderr, "fg: no such job\n"); return 1; }
    printf("%s\n", j->cmdline);
    if (j->state == STOPPED) {
        j->state = RUNNING;
        j->notified = 0;
        kill(-j->pgid, SIGCONT);
    }
    return wait_fg(j);
}

static int builtin_bg(char **argv) {
    Job *j = job_lookup(argv[1]);
    if (!j) { fprintf(stderr, "bg: no such job\n"); return 1; }
    if (j->state == STOPPED) {
        j->state = RUNNING;
        j->notified = 0;
        kill(-j->pgid, SIGCONT);
    }
    printf("[%d] %s &\n", job_id(j), j->cmdline);
    return 0;
}

/* Runs cd/exit/jobs/fg/bg in the shell itself. Sets *quit for `exit`. */
static int run_parent_builtin(char **argv, int *quit) {
    const char *name = argv[0];
    if (!strcmp(name, "exit")) {
        *quit = 1;
        return argv[1] ? atoi(argv[1]) : last_status;
    }
    if (!strcmp(name, "cd"))   return builtin_cd(argv);
    if (!strcmp(name, "jobs")) return builtin_jobs();
    if (!strcmp(name, "fg"))   return builtin_fg(argv);
    return builtin_bg(argv);
}

/* ---------- lexing ---------- */

static void free_tokens(char **tok, int n) {
    for (int i = 0; i < n; i++) free(tok[i]);
}

/* Split a line into words and operators. Quotes are removed here, but
 * variables are NOT expanded (see expand_word). Two marker bytes record what
 * the expander needs to know: SQ_DOLLAR for '$' inside single quotes, and a
 * leading QUOTED byte for words that contained any quotes. */
static int tokenize(const char *s, char **tok, int *isop) {
    int n = 0;
    while (*s) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (n >= MAX_TOKENS - 1) { fprintf(stderr, "mysh: too many tokens\n"); goto fail; }

        char c = *s;
        size_t oplen = 0;
        if (isdigit((unsigned char)c) && (s[1] == '<' || s[1] == '>')) {
            /* N<  N>  N>>  N>&  */
            oplen = (s[1] == '>' && (s[2] == '>' || s[2] == '&')) ? 3 : 2;
        } else if ((c == '&' && (s[1] == '&' || s[1] == '>')) ||
                   (c == '|' && (s[1] == '|' || s[1] == '&')) ||
                   (c == '>' && (s[1] == '&' || s[1] == '>'))) {
            oplen = 2;                              /* && &> || |& >& >> */
        } else if (strchr("|<>&;", c)) {
            oplen = 1;
        }
        if (oplen) {
            tok[n] = strndup(s, oplen);
            isop[n++] = 1;
            s += oplen;
            continue;
        }

        char *buf = malloc(16);
        size_t cap = 16, k = 0;
        char quote = 0;
        int had_quote = 0;
        while (*s && (quote || (!isspace((unsigned char)*s) && !strchr("|<>&;", *s)))) {
            char ch = *s;
            if (quote) {
                if (ch == quote) quote = 0;
                else buf_putc(&buf, &cap, &k, (quote == '\'' && ch == '$') ? SQ_DOLLAR : ch);
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
                had_quote = 1;
            } else {
                buf_putc(&buf, &cap, &k, ch);
            }
            s++;
        }
        if (quote) {
            free(buf);
            fprintf(stderr, "mysh: unterminated quote\n");
            goto fail;
        }
        buf[k] = '\0';
        if (had_quote) {
            buf = realloc(buf, k + 2);
            memmove(buf + 1, buf, k + 1);
            buf[0] = QUOTED;
        }
        tok[n] = buf;
        isop[n++] = 0;
    }
    return n;
fail:
    free_tokens(tok, n);
    return -1;
}

/* ---------- parsing ---------- */

/* Describe a redirection operator like ">", "2>>", "2>&", "&>", "<". */
static int add_redir(Cmd *c, const char *op, char *target) {
    if (c->nredirs >= MAX_REDIRS) { fprintf(stderr, "mysh: too many redirections\n"); return -1; }
    Redir *r = &c->redirs[c->nredirs++];
    memset(r, 0, sizeof *r);
    r->src = target;                       /* borrowed from the token array */

    int fd = -1;
    if (isdigit((unsigned char)*op)) fd = *op++ - '0';

    if (!strcmp(op, "&>")) {               /* stdout and stderr to a file */
        r->dest = 1;
        r->dup_err = 1;
    } else if (*op == '<') {
        r->dest = fd >= 0 ? fd : 0;
        r->is_input = 1;
    } else {                               /* '>' family */
        r->dest = fd >= 0 ? fd : 1;
        if (op[1] == '>') {
            r->append = 1;
        } else if (op[1] == '&') {
            if (is_number(target + (*target == QUOTED))) r->src_is_fd = 1;
            else if (fd < 0) r->dup_err = 1;   /* >&file : both streams */
        }
    }
    return 0;
}

static Pipeline *new_pipeline(CmdList *cl, ListOp op) {
    if (cl->npipes >= MAX_LIST) { fprintf(stderr, "mysh: too many commands\n"); return NULL; }
    cl->ops[cl->npipes - 1] = op;
    Pipeline *p = &cl->pipes[cl->npipes++];
    p->n = 1;
    return p;
}

/* Build a CmdList from tokens. argv/redir pointers borrow from tok[]. */
static int parse_list(char **tok, int *isop, int n, CmdList *cl) {
    memset(cl, 0, sizeof *cl);
    cl->npipes = 1;
    Pipeline *cur = &cl->pipes[0];
    cur->n = 1;
    Cmd *c = &cur->cmds[0];
    int argc = 0;

    for (int i = 0; i < n; i++) {
        if (!isop[i]) {
            if (argc >= MAX_ARGS - 1) { fprintf(stderr, "mysh: too many arguments\n"); return -1; }
            c->argv[argc++] = tok[i];
            continue;
        }
        const char *op = tok[i];

        if (!strcmp(op, "|") || !strcmp(op, "|&")) {
            if (argc == 0) goto syntax;
            if (cur->n >= MAX_CMDS) { fprintf(stderr, "mysh: pipeline too long\n"); return -1; }
            if (op[1] == '&') c->err_to_next = 1;
            c = &cur->cmds[cur->n++];
            argc = 0;
        } else if (!strcmp(op, "&&") || !strcmp(op, "||") ||
                   !strcmp(op, ";")  || !strcmp(op, "&")) {
            if (argc == 0) goto syntax;
            int last = (i == n - 1);
            if (op[0] == '&' && op[1] == '\0') cur->bg = 1;
            if (last) {
                if (op[1] != '\0' && op[0] != ';') goto syntax;   /* trailing && or || */
                continue;                                         /* trailing ; or & is fine */
            }
            ListOp lo = !strcmp(op, "&&") ? OP_AND : !strcmp(op, "||") ? OP_OR : OP_SEMI;
            cur = new_pipeline(cl, lo);
            if (!cur) return -1;
            c = &cur->cmds[0];
            argc = 0;
        } else {                                      /* a redirection operator */
            if (i + 1 >= n || isop[i + 1]) goto syntax;
            if (add_redir(c, op, tok[++i]) < 0) return -1;
        }
        continue;
    syntax:
        fprintf(stderr, "mysh: syntax error near '%s'\n", op);
        return -1;
    }
    if (argc == 0) { fprintf(stderr, "mysh: syntax error: missing command\n"); return -1; }
    return 0;
}

/* ---------- expansion (runs right before a pipeline executes) ---------- */

/* Expand the '$...' at *pp. Returns malloc'd text and advances *pp. */
static char *expand_dollar(const char **pp) {
    const char *p = *pp + 1;

    if (*p == '{') {
        const char *end = strchr(p, '}');
        if (end) {
            char *name = strndup(p + 1, (size_t)(end - p - 1));
            const char *v = name ? getenv(name) : NULL;
            char *val = strdup(v ? v : "");
            free(name);
            *pp = end + 1;
            return val;
        }
        *pp = p;                          /* unterminated "${": literal '$' */
        return strdup("$");
    }
    if (*p == '?' || *p == '$') {
        char b[16];
        snprintf(b, sizeof b, "%d", *p == '?' ? last_status : (int)getpid());
        *pp = p + 1;
        return strdup(b);
    }
    if (isdigit((unsigned char)*p) || *p == '@' || *p == '*') {
        *pp = p + 1;                      /* positional params: not supported */
        return strdup("");
    }
    if (*p == '#') {
        *pp = p + 1;
        return strdup("0");
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
        const char *start = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        char *name = strndup(start, (size_t)(p - start));
        const char *v = name ? getenv(name) : NULL;
        char *val = strdup(v ? v : "");
        free(name);
        *pp = p;
        return val;
    }
    *pp = p;                              /* lone '$': literal */
    return strdup("$");
}

/* Expand one raw word. *vanish is set when an unquoted word expanded to
 * nothing (so `echo $UNSET foo` passes one argument, like a real shell). */
static char *expand_word(const char *w, int *vanish) {
    int quoted = 0;
    if (*w == QUOTED) { quoted = 1; w++; }
    char *buf = malloc(16);
    size_t cap = 16, k = 0;
    while (*w) {
        if (*w == SQ_DOLLAR) {
            buf_putc(&buf, &cap, &k, '$');
            w++;
        } else if (*w == '$') {
            char *e = expand_dollar(&w);
            buf_putstr(&buf, &cap, &k, e);
            free(e);
        } else {
            buf_putc(&buf, &cap, &k, *w++);
        }
    }
    buf[k] = '\0';
    *vanish = (!quoted && k == 0);
    return buf;
}

/* Expand every word and redirection target of a pipeline in place.
 * New strings are recorded in tmp[] so the caller can free them. */
static int expand_pipeline(Pipeline *p, char **tmp, int *ntmp) {
    for (int ci = 0; ci < p->n; ci++) {
        Cmd *c = &p->cmds[ci];

        int na = 0;                       /* decide on the RAW words */
        while (c->argv[na] && is_assign(c->argv[na])) na++;
        c->nassign = na;

        int w = 0;
        for (int r = 0; c->argv[r]; r++) {
            int vanish;
            char *e = expand_word(c->argv[r], &vanish);
            if (vanish) { free(e); continue; }
            tmp[(*ntmp)++] = e;
            c->argv[w++] = e;
        }
        c->argv[w] = NULL;

        for (int i = 0; i < c->nredirs; i++) {
            int vanish;
            char *e = expand_word(c->redirs[i].src, &vanish);
            if (vanish) {
                free(e);
                fprintf(stderr, "mysh: ambiguous redirect\n");
                return -1;
            }
            tmp[(*ntmp)++] = e;
            c->redirs[i].src = e;
        }
    }
    return 0;
}

/* ---------- execution ---------- */

static void reset_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

/* Apply a command's redirections, in order. Child only. */
static void apply_redirs(Cmd *c) {
    for (int i = 0; i < c->nredirs; i++) {
        Redir *r = &c->redirs[i];
        if (r->src_is_fd) {
            int from = atoi(r->src);
            if (from != r->dest && dup2(from, r->dest) < 0) {
                fprintf(stderr, "mysh: %d: %s\n", from, strerror(errno));
                _exit(1);
            }
            continue;
        }
        int flags = r->is_input ? O_RDONLY
                                : O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC);
        int fd = open(r->src, flags, 0644);
        if (fd < 0) { fprintf(stderr, "mysh: %s: %s\n", r->src, strerror(errno)); _exit(1); }
        if (fd != r->dest) {              /* open() may already return dest */
            if (dup2(fd, r->dest) < 0) { perror("mysh: dup2"); _exit(1); }
            close(fd);
        }
        if (r->dup_err && dup2(r->dest, STDERR_FILENO) < 0) { perror("mysh: dup2"); _exit(1); }
    }
}

static int run_pipeline(Pipeline *p, const char *line) {
    Job *j = job_alloc();
    if (!j) { fprintf(stderr, "mysh: too many jobs\n"); return 1; }
    snprintf(j->cmdline, sizeof j->cmdline, "%s", line);
    j->state = RUNNING;

    int prev_read = -1;
    for (int i = 0; i < p->n; i++) {
        int fds[2] = { -1, -1 };
        if (i < p->n - 1 && pipe(fds) < 0) { perror("pipe"); break; }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
            break;
        }

        if (pid == 0) {                                   /* ---- child ---- */
            Cmd *cc = &p->cmds[i];
            setpgid(0, j->pgid);                          /* 0 => own pid for 1st */
            if (!p->bg && i == 0) give_terminal(getpid());
            reset_signals();

            if (prev_read >= 0) { dup2(prev_read, STDIN_FILENO); close(prev_read); }
            if (fds[1] >= 0)    { dup2(fds[1], STDOUT_FILENO);   close(fds[1]); }
            if (fds[0] >= 0) close(fds[0]);               /* unused read end */
            if (cc->err_to_next && fds[1] >= 0)           /* |& : only for this link */
                dup2(STDOUT_FILENO, STDERR_FILENO);

            apply_redirs(cc);                             /* explicit redirects win */

            for (int a = 0; a < cc->nassign; a++) apply_assignment(cc->argv[a]);
            char **eff = cc->argv + cc->nassign;
            if (!eff[0]) _exit(0);                        /* only assignments */
            if (!strcmp(eff[0], "echo")) _exit(builtin_echo(eff));

            execvp(eff[0], eff);
            fprintf(stderr, "mysh: %s: %s\n", eff[0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        /* ---- parent ---- */
        if (i == 0) j->pgid = pid;
        setpgid(pid, j->pgid);                            /* also in child: avoids race */
        j->pids[j->npids++] = pid;
        j->live++;
        if (prev_read >= 0) close(prev_read);             /* parent must not hold pipe ends */
        if (fds[1] >= 0) close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read >= 0) close(prev_read);

    if (j->npids == 0) { j->used = 0; return 1; }

    if (p->bg) {
        printf("[%d] %d\n", job_id(j), j->pgid);
        return 0;
    }
    return wait_fg(j);
}

/* Run one expanded pipeline; returns its exit status. */
static int run_one(Pipeline *p, const char *line, int *quit) {
    Cmd *c = &p->cmds[0];
    if (p->n == 1 && !p->bg) {
        char **eff = c->argv + c->nassign;
        if (!eff[0]) {                                    /* VAR=value (standalone) */
            for (int a = 0; a < c->nassign; a++) apply_assignment(c->argv[a]);
            return 0;
        }
        if (is_parent_builtin(eff[0])) return run_parent_builtin(eff, quit);
        if (!strcmp(eff[0], "echo") && !c->nredirs && !c->nassign)
            return builtin_echo(eff);                     /* plain echo: no fork */
    }
    return run_pipeline(p, line);
}

/* ---------- main loop ---------- */

int main(void) {
    interactive = isatty(STDIN_FILENO);
    if (interactive) {
        shell_pgid = getpid();
        setpgid(shell_pgid, shell_pgid);
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        signal(SIGINT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
    }

    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        reap_background();
        report_jobs();

        if (interactive) { printf("mysh> "); fflush(stdout); }
        ssize_t len = getline(&line, &cap, stdin);
        if (len < 0) { if (interactive) putchar('\n'); break; }   /* EOF / Ctrl-D */
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        char *tok[MAX_TOKENS];
        int isop[MAX_TOKENS];
        int n = tokenize(line, tok, isop);
        if (n <= 0) continue;                         /* error or blank line */

        CmdList cl;
        int quit = 0;
        if (parse_list(tok, isop, n, &cl) == 0) {
            int status = last_status;
            for (int i = 0; i < cl.npipes && !quit; i++) {
                if (i > 0) {                          /* && / || : skip ONE command */
                    ListOp op = cl.ops[i - 1];
                    if ((op == OP_AND && status != 0) || (op == OP_OR && status == 0))
                        continue;
                }
                char *tmp[MAX_TOKENS];
                int ntmp = 0;
                if (expand_pipeline(&cl.pipes[i], tmp, &ntmp) < 0)
                    status = 1;
                else
                    status = run_one(&cl.pipes[i], line, &quit);
                for (int t = 0; t < ntmp; t++) free(tmp[t]);
                last_status = status;
            }
        } else {
            last_status = 2;                          /* syntax error */
        }
        free_tokens(tok, n);
        if (quit) break;
    }
    free(line);
    return last_status;
}
