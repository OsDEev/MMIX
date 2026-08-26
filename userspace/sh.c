/*
 * MyUnix shell (/bin/sh).
 *
 * Supports:
 *   - builtins: echo, help, exit, pid, kill, ppid
 *   - pipelines:      cmd1 args | cmd2 args [| cmd3 ...]
 *   - redirections:   cmd > file, cmd >> file, cmd < file
 */
#include "libc.h"

#define LINE_MAX_LEN 256
#define MAX_ARGS 16
#define MAX_STAGES 4

struct redirect {
    const char *file;
    int type; /* 0:none 1:> 2:>> 3:< */
};

struct stage {
    char *argv[MAX_ARGS];
    struct redirect redir_in;
    struct redirect redir_out;
};

/* --- line editing --------------------------------------------------------- */

static void readline(char *buf, size_t max) {
    size_t n = 0;
    for (;;) {
        char c;
        long r = read(0, &c, 1);
        if (r <= 0) continue;

        if (c == '\n') {
            buf[n] = '\0';
            return;
        }
        if (c == '\b' || c == 0x7F) {
            if (n > 0) {
                n--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        if (c == '\r') continue;
        if (c < 32) continue;

        if (n + 1 < max) buf[n++] = c;
    }
}

/* --- parsing --------------------------------------------------------------- */

static void tokenize(char *s, char **argv, int max_argc) {
    int argc = 0;
    while (*s && argc < max_argc - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') break;
        argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = '\0';
    }
    argv[argc] = NULL;
}

/* Split a stage's argv into command + redirection specs. */
static void parse_redirects(char **argv, struct stage *st) {
    st->redir_in.type = st->redir_out.type = 0;
    st->redir_in.file = st->redir_out.file = 0;

    int out = 0;
    for (int i = 0; argv[i]; i++) {
        if (strcmp(argv[i], ">") == 0 && argv[i + 1]) {
            st->redir_out.type = 1;
            st->redir_out.file = argv[++i];
        } else if (strcmp(argv[i], ">>") == 0 && argv[i + 1]) {
            st->redir_out.type = 2;
            st->redir_out.file = argv[++i];
        } else if (strcmp(argv[i], "<") == 0 && argv[i + 1]) {
            st->redir_in.type = 3;
            st->redir_in.file = argv[++i];
        } else {
            argv[out++] = argv[i];
        }
    }
    argv[out] = NULL;
}

static int split_pipeline(char *line, char *stages[MAX_STAGES]) {
    int n = 0;
    char *p = line;
    stages[n++] = p;
    while (*p && n < MAX_STAGES) {
        if (*p == '|') {
            *p = '\0';
            stages[n++] = p + 1;
        }
        p++;
    }
    return n;
}

/* --- execution -------------------------------------------------------------- */

static void apply_redirects(struct stage *st) {
    if (st->redir_in.type == 3) {
        int fd = open(st->redir_in.file);
        if (fd < 0) {
            print("sh: cannot open ");
            print(st->redir_in.file);
            print("\n");
            sys_exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (st->redir_out.type == 1) {
        int fd = open_create(st->redir_out.file);
        if (fd < 0) {
            print("sh: cannot create ");
            print(st->redir_out.file);
            print("\n");
            sys_exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    if (st->redir_out.type == 2) {
        int fd = open_append(st->redir_out.file);
        if (fd < 0) {
            print("sh: cannot open ");
            print(st->redir_out.file);
            print("\n");
            sys_exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
}

/* minimal atoi (kept local to avoid pulling more libc in) */
static int atoi_stub(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void exec_external(struct stage *st);

static int run_builtin(char **argv) {
    if (strcmp(argv[0], "exit") == 0) {
        sys_exit(0);
    } else if (strcmp(argv[0], "help") == 0) {
        print("\033[1;92mMMix commands\033[0m\n");
        print("\033[90m-------------------------------------------------------------------------\033[0m\n");
        print("\033[1;96msystem\033[0m  fetch  free  date  uptime  ps     clear   reboot\n");
        print("\033[1;96mfiles\033[0m   cat    ls    wc    grep    pipes  echo >  echo >>  cmd < file\n");
        print("\033[1;96mshell\033[0m   pid  ppid  whoami  id  kill <pid> <sig>   sleep <s>   exit\n");
        print("\033[1;96mpriv\033[0m    root   rudo <cmd>   panic   (privilege escalation)\n");
        print("\033[1;96mgraphics\033[0m desktop  gfx\n");
        print("\033[1;96mdemo\033[0m     busy\n");
        print("\033[90m-------------------------------------------------------------------------\033[0m\n");
        print("try: \033[1;33mcat /etc/unit.conf | grep exec | wc -l\033[0m\n");
        return 1;
    } else if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; argv[i]; i++) {
            print(argv[i]);
            if (argv[i + 1]) print(" ");
        }
        print("\n");
        return 1;
    } else if (strcmp(argv[0], "pid") == 0) {
        print("pid: ");
        print_num(getpid());
        print(" ppid: ");
        print_num(getppid());
        print("\n");
        return 1;
    } else if (strcmp(argv[0], "ppid") == 0) {
        print_num(getppid());
        print("\n");
        return 1;
    } else if (strcmp(argv[0], "kill") == 0 && argv[1] && argv[2]) {
        kill(atoi_stub(argv[1]), atoi_stub(argv[2]));
        return 1;
    } else if (strcmp(argv[0], "whoami") == 0) {
        int uid = getuid();
        if (uid == 0) print("root\n");
        else { print("user("); print_num(uid); print(")\n"); }
        return 1;
    } else if (strcmp(argv[0], "id") == 0) {
        int uid = getuid();
        int gid = getpgid(0);
        print("uid="); print_num(uid);
        print(" gid="); print_num(gid);
        if (uid == 0) print(" (root)");
        print("\n");
        return 1;
    } else if (strcmp(argv[0], "root") == 0) {
        if (getuid() == 0) {
            print("already root\n");
            return 1;
        }
        print("requesting root access...\n");
        int rc = rudo_request(RUDO_OP_SETUID);
        if (rc == 0) {
            print("\033[1;92mYou are now root.\033[0m\n");
        } else {
            print("\033[1;91mAccess denied.\033[0m\n");
        }
        return 1;
    } else if (strcmp(argv[0], "rudo") == 0) {
        if (argv[1] == NULL) { print("usage: rudo <command>\n"); return 1; }
        struct stage st = {{0}};
        int j = 0;
        for (int i = 1; argv[i] && j < MAX_ARGS - 1; i++) st.argv[j++] = argv[i];
        st.argv[j] = NULL;
        if (getuid() == 0) {
            exec_external(&st);
        }
        print("requesting root for: ");
        print(argv[1]);
        print("\n");
        long pid = sys_fork();
        if (pid == 0) {
            int rc = rudo_request(RUDO_OP_SETUID);
            if (rc != 0) {
                print("\033[1;91mrudo: access denied\033[0m\n");
                sys_exit(1);
            }
            exec_external(&st);
        }
        int st2 = 0;
        waitpid((int)pid, &st2);
        return 1;
    }
    return 0;
}

static void exec_external(struct stage *st) {
    apply_redirects(st);

    const char *cmd = st->argv[0];
    char path[LINE_MAX_LEN];

    if (cmd[0] == '/') {
        execve(cmd, st->argv);
    } else {
        memcpy(path, "/bin/", 5);
        memcpy(path + 5, cmd, strlen(cmd) + 1);
        execve(path, st->argv);
    }

    print("sh: command not found: ");
    print(cmd);
    print("\n");
    sys_exit(127);
}

static void run_pipeline(char *line) {
    char *raw[MAX_STAGES];
    int nstages = split_pipeline(line, raw);

    struct stage stages[MAX_STAGES];
    for (int i = 0; i < nstages; i++) {
        tokenize(raw[i], stages[i].argv, MAX_ARGS);
        parse_redirects(stages[i].argv, &stages[i]);
    }

    /* Single stage: builtins run in-process -- but only without
     * redirections; with redirects the builtin runs in a child so the
     * dup2'd fds apply. */
    if (nstages == 1) {
        if (stages[0].argv[0] == NULL) return;
        if (stages[0].redir_in.type == 0 && stages[0].redir_out.type == 0) {
            if (run_builtin(stages[0].argv)) return;
        }
    }

    int pipes[MAX_STAGES - 1][2];
    for (int i = 0; i < nstages - 1; i++) {
        if (pipe(pipes[i]) != 0) {
            print("sh: pipe failed\n");
            return;
        }
    }

    int pids[MAX_STAGES];
    for (int i = 0; i < nstages; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            setpgid(0, 0);
            /* child i: wire pipe ends */
            if (i > 0) dup2(pipes[i - 1][0], 0);
            if (i < nstages - 1) dup2(pipes[i][1], 1);

            for (int j = 0; j < nstages - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if (stages[i].argv[0] == NULL) sys_exit(0);
            apply_redirects(&stages[i]);
            if (run_builtin(stages[i].argv)) sys_exit(0);
            exec_external(&stages[i]);
        }
        pids[i] = (int)pid;
        setpgid((int)pid, (int)pid); /* parent-side, best effort */
    }

    /* foreground = the pipeline; Ctrl+C goes to it, not to us */
    tcsetpgrp(pids[nstages - 1]);

    /* parent: close all pipe ends so writers see EOF */
    for (int i = 0; i < nstages - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    for (int i = 0; i < nstages; i++) {
        int st = 0;
        waitpid(pids[i], &st);
    }

    tcsetpgrp(getpgid(0)); /* foreground back to the shell */
}

int main(void) {
    char line[LINE_MAX_LEN];

    signal(SIGINT, SIG_IGN);
    setpgid(0, 0);
    tcsetpgrp(getpgid(0));
    setuid(1000); /* the shell is unprivileged; root stays with init/rudod */

    print("\n\033[1;36mMMix 0.5.0\033[0m shell. Type \033[1;32mhelp\033[0m for commands.\n");

    for (;;) {
        print("\033[1;35m$\033[0m ");
        readline(line, sizeof(line));
        if (line[0] == '\0') continue;
        run_pipeline(line);
    }
}
