/* shell.c — interactive shell for TigxOS V1.1.3 */

#include "libc.h"
#include "version.h"

#define LINE_MAX 256
#define MAX_ARGS 32
#define ARGS_SLOTS (MAX_ARGS + 1)  /* room for NULL terminator */
#define HIST_MAX 16

/* Must match kernel keyboard.h special codes */
#define KEY_LEFT   ((char)0x80)
#define KEY_RIGHT  ((char)0x81)
#define KEY_UP     ((char)0x82)
#define KEY_DOWN   ((char)0x83)
#define KEY_HOME   ((char)0x84)
#define KEY_END    ((char)0x85)
#define KEY_DELETE ((char)0x86)

static int last_status = 0;  /* $? — status of last command */

/* Command history ring (newest at hist_count-1 when not full) */
static char hist[HIST_MAX][LINE_MAX];
static int hist_count = 0;   /* entries stored 0..HIST_MAX */
static int hist_start = 0;   /* index of oldest in ring */

static void shell_puts(const char *s) {
    if (!s) return;
    write(STDOUT_FILENO, s, strlen(s));
}

#define MAX_JOBS 8
static int job_pids[MAX_JOBS];
static int job_count = 0;
static int fg_pid = -1; /* current foreground job, if any */

static void jobs_add(int pid) {
    if (pid <= 0 || job_count >= MAX_JOBS)
        return;
    job_pids[job_count++] = pid;
}

static void jobs_remove(int pid) {
    int w = 0;
    for (int i = 0; i < job_count; i++) {
        if (job_pids[i] != pid)
            job_pids[w++] = job_pids[i];
    }
    job_count = w;
}

/* Non-blocking-ish: wait only returns when child is dead; we call after
 * spawn of short tools. For Done notices, reap known job pids that exited.
 * process_wait blocks, so we only reap when we know a child finished or
 * when the user runs `jobs`/`wait`. Kernel keeps DEAD children until wait. */
/* Reap finished background jobs without blocking; print Done notices. */
static void jobs_reap_done(void) {
    int i = 0;
    while (i < job_count) {
        int pid = job_pids[i];
        int st = wait_noblock(pid);
        if (st == -2) {
            i++; /* still running */
            continue;
        }
        /* exited or gone */
        jobs_remove(pid);
        if (st >= 0)
            printf("[%d] Done\n", pid);
        /* else vanished — still drop from table */
        /* jobs_remove shifts; do not increment i */
    }
}

static int jobs_wait_one(int pid) {
    int st = wait(pid);
    jobs_remove(pid);
    return (st < 0) ? 1 : (st & 0xff);
}

static void cmd_jobs(void) {
    jobs_reap_done();
    if (job_count == 0) {
        shell_puts("jobs: none\n");
        return;
    }
    for (int i = 0; i < job_count; i++)
        printf("[%d] Running pid %d\n", i + 1, job_pids[i]);
}

static int cmd_wait_builtin(int argc, char **argv) {
    if (argc >= 2) {
        int pid = 0;
        const char *s = argv[1];
        while (*s >= '0' && *s <= '9') {
            pid = pid * 10 + (*s - '0');
            s++;
        }
        if (pid <= 0) {
            shell_puts("wait: bad pid\n");
            return 1;
        }
        int st = jobs_wait_one(pid);
        return st;
    }
    /* wait all background jobs */
    int last = 0;
    while (job_count > 0) {
        int pid = job_pids[0];
        last = jobs_wait_one(pid);
        printf("[%d] Done\n", pid);
    }
    return last;
}

static int cmd_fg(int argc, char **argv) {
    int pid = -1;
    if (argc >= 2) {
        const char *s = argv[1];
        int n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            s++;
        }
        /* treat as job number 1..N or raw pid */
        if (n >= 1 && n <= job_count)
            pid = job_pids[n - 1];
        else
            pid = n;
    } else if (job_count > 0) {
        pid = job_pids[job_count - 1];
    }
    if (pid <= 0) {
        shell_puts("fg: no job\n");
        return 1;
    }
    fg_pid = pid;
    setfg(0);
    setfg(pid);
    int st = jobs_wait_one(pid);
    setfg(0);
    fg_pid = -1;
    printf("[%d] Done\n", pid);
    return st;
}


static void prompt(void) {
    char cwd[64];
    if (getcwd(cwd, sizeof(cwd)) == 0)
        shell_puts(cwd);
    else
        shell_puts("?");
    shell_puts("> ");
}

static void hist_add(const char *line) {
    if (!line || !line[0])
        return;
    /* skip duplicate of most recent */
    if (hist_count > 0) {
        int last = (hist_start + hist_count - 1) % HIST_MAX;
        if (strcmp(hist[last], line) == 0)
            return;
    }
    if (hist_count < HIST_MAX) {
        int slot = (hist_start + hist_count) % HIST_MAX;
        int i = 0;
        while (line[i] && i < LINE_MAX - 1) {
            hist[slot][i] = line[i];
            i++;
        }
        hist[slot][i] = 0;
        hist_count++;
    } else {
        int slot = hist_start;
        int i = 0;
        while (line[i] && i < LINE_MAX - 1) {
            hist[slot][i] = line[i];
            i++;
        }
        hist[slot][i] = 0;
        hist_start = (hist_start + 1) % HIST_MAX;
    }
}

static const char *hist_get(int logical) {
    /* logical 0 = oldest, hist_count-1 = newest */
    if (logical < 0 || logical >= hist_count)
        return 0;
    return hist[(hist_start + logical) % HIST_MAX];
}

/* Redraw from cursor to end of line, then put cursor back at pos. */
static void redraw_from(const char *buf, int len, int pos) {
    int i;
    for (i = pos; i < len; i++)
        write(STDOUT_FILENO, &buf[i], 1);
    write(STDOUT_FILENO, " ", 1); /* clear leftover tail char */
    /* back up: (len - pos) + 1 spaces printed past pos */
    for (i = 0; i < (len - pos) + 1; i++)
        write(STDOUT_FILENO, "\b", 1);
}

/*
 * Line editor: left/right/home/end, backspace/delete, mid-line insert,
 * up/down history. Returns length; buf is NUL-terminated.
 */
static int readline(char *buf, int max) {
    int len = 0;
    int pos = 0;
    int hist_nav = hist_count; /* one past newest = "current draft" */
    char draft[LINE_MAX];
    draft[0] = 0;

    buf[0] = 0;

    for (;;) {
        char c;
        long r = read(STDIN_FILENO, &c, 1);
        if (r < 0)
            return -1;
        if (r == 0)
            break;

        if (c == '\r' || c == '\n') {
            write(STDOUT_FILENO, "\n", 1);
            buf[len] = 0;
            return len;
        }

        /* --- navigation / editing specials --- */
        if (c == KEY_LEFT) {
            if (pos > 0) {
                pos--;
                write(STDOUT_FILENO, "\b", 1);
            }
            continue;
        }
        if (c == KEY_RIGHT) {
            if (pos < len) {
                write(STDOUT_FILENO, &buf[pos], 1);
                pos++;
            }
            continue;
        }
        if (c == KEY_HOME) {
            while (pos > 0) {
                pos--;
                write(STDOUT_FILENO, "\b", 1);
            }
            continue;
        }
        if (c == KEY_END) {
            while (pos < len) {
                write(STDOUT_FILENO, &buf[pos], 1);
                pos++;
            }
            continue;
        }
        if (c == KEY_DELETE) {
            if (pos < len) {
                for (int i = pos; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                len--;
                buf[len] = 0;
                redraw_from(buf, len, pos);
            }
            continue;
        }
        if (c == KEY_UP) {
            if (hist_count == 0)
                continue;
            if (hist_nav == hist_count) {
                /* save draft */
                int i = 0;
                while (i < len && i < LINE_MAX - 1) {
                    draft[i] = buf[i];
                    i++;
                }
                draft[i] = 0;
            }
            if (hist_nav > 0)
                hist_nav--;
            else
                continue;
            /* clear current display */
            while (pos > 0) {
                pos--;
                write(STDOUT_FILENO, "\b", 1);
            }
            for (int i = 0; i < len; i++)
                write(STDOUT_FILENO, " ", 1);
            for (int i = 0; i < len; i++)
                write(STDOUT_FILENO, "\b", 1);
            const char *h = hist_get(hist_nav);
            len = 0;
            if (h) {
                while (h[len] && len + 1 < max) {
                    buf[len] = h[len];
                    len++;
                }
            }
            buf[len] = 0;
            write(STDOUT_FILENO, buf, (size_t)len);
            pos = len;
            continue;
        }
        if (c == KEY_DOWN) {
            if (hist_nav >= hist_count)
                continue;
            hist_nav++;
            while (pos > 0) {
                pos--;
                write(STDOUT_FILENO, "\b", 1);
            }
            for (int i = 0; i < len; i++)
                write(STDOUT_FILENO, " ", 1);
            for (int i = 0; i < len; i++)
                write(STDOUT_FILENO, "\b", 1);
            if (hist_nav >= hist_count) {
                /* restore draft */
                len = 0;
                while (draft[len] && len + 1 < max) {
                    buf[len] = draft[len];
                    len++;
                }
                buf[len] = 0;
            } else {
                const char *h = hist_get(hist_nav);
                len = 0;
                if (h) {
                    while (h[len] && len + 1 < max) {
                        buf[len] = h[len];
                        len++;
                    }
                }
                buf[len] = 0;
            }
            write(STDOUT_FILENO, buf, (size_t)len);
            pos = len;
            continue;
        }

        /* backspace */
        if (c == 8 || c == 127) {
            if (pos > 0) {
                for (int i = pos - 1; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                len--;
                pos--;
                buf[len] = 0;
                write(STDOUT_FILENO, "\b", 1);
                redraw_from(buf, len, pos);
            }
            continue;
        }

        /* ignore other non-printable (except we already handled specials) */
        if ((unsigned char)c < 32 || (unsigned char)c >= 0x80)
            continue;

        /* insert character at pos */
        if (len + 1 >= max)
            continue;
        for (int i = len; i > pos; i--)
            buf[i] = buf[i - 1];
        buf[pos] = c;
        len++;
        buf[len] = 0;
        /* print c and the rest */
        write(STDOUT_FILENO, &c, 1);
        pos++;
        if (pos < len)
            redraw_from(buf, len, pos);
    }
    buf[len] = 0;
    return len;
}

/* ---- variables ---- */
#define MAX_VARS 16
#define VAR_NAME 24
#define VAR_VAL  64

struct shell_var {
    char name[VAR_NAME];
    char value[VAR_VAL];
    int  exported;
    int  used;
};
static struct shell_var vars[MAX_VARS];

static int var_find(const char *name) {
    for (int i = 0; i < MAX_VARS; i++)
        if (vars[i].used && strcmp(vars[i].name, name) == 0)
            return i;
    return -1;
}

static int var_set(const char *name, const char *value, int exported) {
    if (!name || !name[0])
        return -1;
    int i = var_find(name);
    if (i < 0) {
        for (i = 0; i < MAX_VARS; i++)
            if (!vars[i].used)
                break;
        if (i >= MAX_VARS)
            return -1;
        vars[i].used = 1;
        vars[i].exported = 0;
        int n = 0;
        while (name[n] && n < VAR_NAME - 1) {
            vars[i].name[n] = name[n];
            n++;
        }
        vars[i].name[n] = 0;
    }
    if (value) {
        int n = 0;
        while (value[n] && n < VAR_VAL - 1) {
            vars[i].value[n] = value[n];
            n++;
        }
        vars[i].value[n] = 0;
    }
    if (exported)
        vars[i].exported = 1;
    return 0;
}

static const char *var_get(const char *name) {
    int i = var_find(name);
    return (i >= 0) ? vars[i].value : 0;
}

static void var_unset(const char *name) {
    int i = var_find(name);
    if (i >= 0) {
        vars[i].used = 0;
        vars[i].name[0] = 0;
        vars[i].value[0] = 0;
        vars[i].exported = 0;
    }
}

/* Decimal of last_status into static buf */
static char status_buf[16];
static const char *status_str(void) {
    unsigned v = (unsigned)last_status & 0xffu;
    char tmp[8];
    int n = 0;
    if (v == 0)
        tmp[n++] = '0';
    else {
        while (v > 0 && n < 6) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    int i = 0;
    while (n > 0)
        status_buf[i++] = tmp[--n];
    status_buf[i] = 0;
    return status_buf;
}

/* Expansion scratch for tokens (survives until next command) */
#define EXP_POOL (LINE_MAX * 2)
static char exp_pool[EXP_POOL];
static int exp_used;

static char *exp_alloc(int n) {
    if (exp_used + n > EXP_POOL)
        return 0;
    char *p = exp_pool + exp_used;
    exp_used += n;
    return p;
}

/* Expand $VAR / $? / ${VAR} into dst; returns bytes written (no NUL counted in ret beyond). */
static int expand_into(char *dst, int dstmax, const char *src, int allow_vars) {
    int o = 0;
    while (*src && o + 1 < dstmax) {
        /* Escapes first so \$ is literal $ */
        if (*src == '\\' && src[1]) {
            src++;
            char e = *src++;
            if (e == 'n')
                dst[o++] = '\n';
            else if (e == 't')
                dst[o++] = '\t';
            else
                dst[o++] = e;
            continue;
        }
        if (allow_vars && *src == '$') {
            src++;
            if (*src == '?') {
                src++;
                const char *st = status_str();
                while (*st && o + 1 < dstmax)
                    dst[o++] = *st++;
                continue;
            }
            char name[VAR_NAME];
            int ni = 0;
            if (*src == '{') {
                src++;
                while (*src && *src != '}' && ni < VAR_NAME - 1)
                    name[ni++] = *src++;
                if (*src != '}') {
                    dst[0] = 0;
                    return -3; /* unclosed ${ */
                }
                src++;
            } else {
                while ((*src >= 'A' && *src <= 'Z') ||
                       (*src >= 'a' && *src <= 'z') ||
                       (*src >= '0' && *src <= '9') ||
                       *src == '_') {
                    if (ni < VAR_NAME - 1)
                        name[ni++] = *src;
                    src++;
                }
            }
            name[ni] = 0;
            const char *val = name[0] ? var_get(name) : 0;
            if (val) {
                while (*val && o + 1 < dstmax)
                    dst[o++] = *val++;
            }
            continue;
        }
        dst[o++] = *src++;
    }
    dst[o] = 0;
    return o;
}

/*
 * Tokenize with quotes:
 *  - whitespace separates tokens outside quotes
 *  - '...' literal (no $ expansion, no escapes except \')
 *  - "..." with $ expansion and \ escapes
 *  - bare words with $ expansion and \ escapes
 * Writes expanded tokens into exp_pool; argv[] points into it.
 * Returns argc, or -1 unclosed quote, -2 pool full, -3 unclosed ${
 */
static int tokenize(const char *line, char **argv, int max) {
    exp_used = 0;
    int argc = 0;
    const char *p = line;

    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        char tmp[LINE_MAX];
        int ti = 0;
        int allow_vars = 1;

        if (*p == '\'') {
            allow_vars = 0;
            p++;
            while (*p && *p != '\'') {
                if (ti + 1 < LINE_MAX)
                    tmp[ti++] = *p;
                p++;
            }
            if (*p != '\'')
                return -1; /* unclosed */
            p++;
            tmp[ti] = 0;
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    /* Keep \$ so expand_into yields literal $ (not expansion).
                     * Resolve \n \t here; pass other escapes through as \X. */
                    char e = p[1];
                    if (e == '$') {
                        if (ti + 2 < LINE_MAX) {
                            tmp[ti++] = '\\';
                            tmp[ti++] = '$';
                        }
                        p += 2;
                    } else if (e == 'n' || e == 't') {
                        if (ti + 1 < LINE_MAX)
                            tmp[ti++] = (e == 'n') ? '\n' : '\t';
                        p += 2;
                    } else {
                        if (ti + 2 < LINE_MAX) {
                            tmp[ti++] = '\\';
                            tmp[ti++] = e;
                        }
                        p += 2;
                    }
                } else {
                    if (ti + 1 < LINE_MAX)
                        tmp[ti++] = *p;
                    p++;
                }
            }
            if (*p != '"')
                return -1; /* unclosed " */
            p++;
            tmp[ti] = 0;
        } else {
            while (*p && *p != ' ' && *p != '\t') {
                if (*p == '\\' && p[1]) {
                    p++;
                    if (ti + 1 < LINE_MAX)
                        tmp[ti++] = *p;
                    p++;
                } else if (*p == '\'' || *p == '"') {
                    /* stop at quote so "a"b works as two passes — keep simple: include as start */
                    break;
                } else {
                    if (ti + 1 < LINE_MAX)
                        tmp[ti++] = *p;
                    p++;
                }
            }
            tmp[ti] = 0;
        }

        char expanded[LINE_MAX];
        if (allow_vars) {
            int er = expand_into(expanded, LINE_MAX, tmp, 1);
            if (er < 0)
                return er; /* -3 unclosed ${ */
        } else {
            int i = 0;
            while (tmp[i] && i < LINE_MAX - 1) {
                expanded[i] = tmp[i];
                i++;
            }
            expanded[i] = 0;
        }

        int need = 0;
        while (expanded[need])
            need++;
        need++;
        char *slot = exp_alloc(need);
        if (!slot)
            return -2; /* expansion pool full */
        for (int i = 0; i < need; i++)
            slot[i] = expanded[i];
        argv[argc++] = slot;
    }
    argv[argc] = 0;
    return argc;
}

static void cmd_help(void) {
    shell_puts("Built-ins: help/?  exit/quit [N]  pid  true  false\n");
    shell_puts("           set NAME=value | set NAME value | set\n");
    shell_puts("           export NAME[=value]  unset NAME  env\n");
    shell_puts("           (exported vars reach child programs via getenv)\n");
    shell_puts("           cd [DIR]  pwd  sh FILE  jobs  fg [N]  wait [PID]  kill PID\n");
    shell_puts("Lists:     cmd ; cmd   cmd && cmd   cmd || cmd   cmd &\n");
    shell_puts("Jobs:      cmd &   jobs   fg [job|pid]   wait [pid]\n");
    shell_puts("Signals:   Ctrl-C kills foreground job (SIGINT)\n");
    shell_puts("Redir:     cmd > file   cmd >> file   cmd < file\n");
    shell_puts("Pipe:      cmd1 | cmd2   (concurrent, real pipes)\n");
    shell_puts("Edit:      arrows  Home/End  Del  Bksp  Up/Down history\n");
    shell_puts("Scripts:   # comment   sh script.file\n");
    shell_puts("Programs:  hello echo loop args cat rm ls wc writefile lang65 exitn\n");
    shell_puts("Paths:     relative use cwd; absolute start with / ; PATH=bin:.\n");
    shell_puts("Layout:    programs live as bin/NAME.elf (PATH search finds them)\n");
}

/* Build "NAME=value" envp from exported shell vars for child processes. */
static char env_buf[MAX_VARS][VAR_NAME + VAR_VAL + 2];
static char *env_ptrs[MAX_VARS + 1];

static char **build_envp(void) {
    int n = 0;
    for (int i = 0; i < MAX_VARS && n < MAX_VARS; i++) {
        if (!vars[i].used || !vars[i].exported)
            continue;
        int j = 0;
        const char *nm = vars[i].name;
        while (*nm && j < VAR_NAME - 1)
            env_buf[n][j++] = *nm++;
        env_buf[n][j++] = '=';
        const char *vl = vars[i].value;
        while (*vl && j < VAR_NAME + VAR_VAL)
            env_buf[n][j++] = *vl++;
        env_buf[n][j] = 0;
        env_ptrs[n] = env_buf[n];
        n++;
    }
    env_ptrs[n] = 0;
    return env_ptrs;
}

/* Append ".elf" if missing; returns 1 if mutated. */
static int ensure_elf_suffix(char *buf, int len, int cap) {
    if (len >= 4 && buf[len - 4] == '.' && buf[len - 3] == 'e' &&
        buf[len - 2] == 'l' && buf[len - 1] == 'f')
        return 0;
    if (len + 4 >= cap)
        return 0;
    buf[len++] = '.';
    buf[len++] = 'e';
    buf[len++] = 'l';
    buf[len++] = 'f';
    buf[len] = 0;
    return 1;
}

/* Try spawn name, then name.elf. */
static int try_spawn_one(const char *name, char **argv, char **envp) {
    int pid = spawn_env(name, argv, envp);
    if (pid > 0)
        return pid;
    char path[48];
    int i = 0;
    while (name[i] && i < 40) {
        path[i] = name[i];
        i++;
    }
    path[i] = 0;
    if (ensure_elf_suffix(path, i, (int)sizeof(path)))
        pid = spawn_env(path, argv, envp);
    return pid;
}

/*
 * PATH search (v1.1.3): if name contains no '/', walk PATH components.
 * Default PATH is "bin:." (set at shell start if unset).
 * Each component is joined with '/' + name (and name.elf).
 */
static int try_spawn(const char *name, char **argv) {
    char **envp = build_envp();
    int has_slash = 0;
    for (const char *p = name; *p; p++)
        if (*p == '/') { has_slash = 1; break; }

    if (has_slash)
        return try_spawn_one(name, argv, envp);

    /*
     * PATH search only (v1.1.3 fix): do NOT try cwd first unless "." is in PATH.
     * Components are tried in order. Over-long components are skipped (consumed).
     */
    /* NULL = unset → default; empty string = explicitly empty (no search). */
    const char *pathenv = var_get("PATH");
    if (!pathenv)
        pathenv = "bin:.";
    if (!pathenv[0])
        return -1; /* PATH= disables bare-name search */

    char component[40];
    const char *p = pathenv;
    while (*p) {
        int ci = 0;
        int overflow = 0;
        while (*p && *p != ':') {
            if (ci < (int)sizeof(component) - 1)
                component[ci++] = *p;
            else
                overflow = 1;
            p++;
        }
        component[ci] = 0;
        if (*p == ':')
            p++;
        if (overflow)
            continue; /* reject oversized component; pointer already advanced */
        if (ci == 0)
            continue; /* empty component — skip */

        /* "." means search cwd (bare name only) */
        if (ci == 1 && component[0] == '.') {
            int pid = try_spawn_one(name, argv, envp);
            if (pid > 0)
                return pid;
            continue;
        }

        char candidate[48];
        int n = 0;
        for (int i = 0; component[i] && n < 36; i++)
            candidate[n++] = component[i];
        if (n > 0 && candidate[n - 1] != '/')
            candidate[n++] = '/';
        for (int i = 0; name[i] && n < 40; i++)
            candidate[n++] = name[i];
        candidate[n] = 0;

        /* Prefer standard layout form: dir/NAME.elf */
        char with_elf[48];
        int el = 0;
        while (candidate[el] && el < 40) {
            with_elf[el] = candidate[el];
            el++;
        }
        with_elf[el] = 0;
        if (ensure_elf_suffix(with_elf, el, (int)sizeof(with_elf))) {
            int pid = spawn_env(with_elf, argv, envp);
            if (pid > 0)
                return pid;
        }
        int pid = try_spawn_one(candidate, argv, envp);
        if (pid > 0)
            return pid;
    }
    return -1;
}

static int parse_exit_code(int argc, char **argv) {
    if (argc < 2)
        return 0;
    const char *s = argv[1];
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (!*s)
        return 0;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 255)
            v = 255;
        s++;
    }
    if (neg)
        return (-v) & 0xff;
    return v & 0xff;
}

static int parse_redirs(char **argv, int *argc_io,
                        char **out_path, int *append,
                        char **in_path) {
    *out_path = 0;
    *append = 0;
    *in_path = 0;
    int argc = *argc_io;
    int w = 0;
    for (int i = 0; i < argc; i++) {
        char *t = argv[i];
        if (t[0] == '>' && t[1] == '>' && t[2] == 0) {
            if (i + 1 >= argc || *out_path)
                return -1;
            *out_path = argv[++i];
            *append = 1;
            continue;
        }
        if (t[0] == '>' && t[1] == 0) {
            if (i + 1 >= argc || *out_path)
                return -1;
            *out_path = argv[++i];
            *append = 0;
            continue;
        }
        if (t[0] == '<' && t[1] == 0) {
            if (i + 1 >= argc || *in_path)
                return -1;
            *in_path = argv[++i];
            continue;
        }
        argv[w++] = t;
    }
    argv[w] = 0;
    *argc_io = w;
    return 0;
}

static int apply_redirs(char *out_path, int append, char *in_path,
                        int *saved_out, int *saved_in) {
    *saved_out = -1;
    *saved_in = -1;
    int out_fd = -1, in_fd = -1;

    if (out_path) {
        *saved_out = dup(STDOUT_FILENO);
        if (*saved_out < 0)
            goto fail;
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        out_fd = open(out_path, flags);
        if (out_fd < 0) {
            shell_puts("redir: cannot open ");
            shell_puts(out_path);
            shell_puts("\n");
            goto fail;
        }
        if (dup2(out_fd, STDOUT_FILENO) < 0)
            goto fail;
        close(out_fd);
        out_fd = -1;
    }
    if (in_path) {
        *saved_in = dup(STDIN_FILENO);
        if (*saved_in < 0)
            goto fail;
        in_fd = open(in_path, O_RDONLY);
        if (in_fd < 0) {
            shell_puts("redir: cannot open ");
            shell_puts(in_path);
            shell_puts("\n");
            goto fail;
        }
        if (dup2(in_fd, STDIN_FILENO) < 0)
            goto fail;
        close(in_fd);
        in_fd = -1;
    }
    return 0;
fail:
    if (out_fd >= 0)
        close(out_fd);
    if (in_fd >= 0)
        close(in_fd);
    if (*saved_out >= 0) {
        dup2(*saved_out, STDOUT_FILENO);
        close(*saved_out);
        *saved_out = -1;
    }
    if (*saved_in >= 0) {
        dup2(*saved_in, STDIN_FILENO);
        close(*saved_in);
        *saved_in = -1;
    }
    return -1;
}

static void restore_redirs(int saved_out, int saved_in) {
    if (saved_out >= 0) {
        dup2(saved_out, STDOUT_FILENO);
        close(saved_out);
    }
    if (saved_in >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }
}


/* Concurrent pipeline: spawn all stages with real pipes (AS enables this).
 * Stage 0 writes to pipe0; stage 1 reads pipe0 writes pipe1; ... last reads. */
static int run_program(char **argv, char *out_path, int append, char *in_path);

static int run_pipeline(char **argv, int argc, int background) {
    enum { MAX_STAGES = 4, STAGE_ARGS = 8 };
    char *stage[MAX_STAGES][STAGE_ARGS];
    char *stage_out[MAX_STAGES];
    char *stage_in[MAX_STAGES];
    int stage_app[MAX_STAGES];
    int stage_argc[MAX_STAGES];
    int nstages = 0;
    int i = 0;

    /* Reject leading pipe */
    if (argc > 0 && argv[0][0] == '|' && argv[0][1] == 0) {
        shell_puts("syntax: leading |\n");
        return 1;
    }

    while (i < argc && nstages < MAX_STAGES) {
        int a = 0;
        while (i < argc && !(argv[i][0] == '|' && argv[i][1] == 0)) {
            if (a + 1 >= STAGE_ARGS)
                return 1;
            stage[nstages][a++] = argv[i++];
        }
        if (a == 0) {
            /* empty stage: consecutive | or trailing | */
            shell_puts("syntax: empty pipeline stage\n");
            return 1;
        }
        stage[nstages][a] = 0;
        stage_argc[nstages] = a;
        stage_out[nstages] = 0;
        stage_in[nstages] = 0;
        stage_app[nstages] = 0;
        /* Per-stage redirections */
        if (parse_redirs(stage[nstages], &stage_argc[nstages],
                         &stage_out[nstages], &stage_app[nstages],
                         &stage_in[nstages]) != 0) {
            shell_puts("redir: syntax error in pipeline stage\n");
            return 1;
        }
        if (stage_argc[nstages] == 0) {
            shell_puts("redir: missing command in pipeline stage\n");
            return 1;
        }
        nstages++;
        if (i < argc && argv[i][0] == '|' && argv[i][1] == 0) {
            i++;
            if (i >= argc) {
                shell_puts("syntax: trailing |\n");
                return 1;
            }
        }
    }
    if (i < argc || nstages < 2)
        return 1;

    int pfd[MAX_STAGES][2];
    int n_pipes = nstages - 1;
    for (int p = 0; p < n_pipes; p++) {
        if (pipe(pfd[p]) != 0) {
            shell_puts("pipe: failed\n");
            for (int q = 0; q < p; q++) {
                close(pfd[q][0]);
                close(pfd[q][1]);
            }
            return 1;
        }
    }

    int pids[MAX_STAGES];
    for (int s = 0; s < nstages; s++)
        pids[s] = -1;

    int saved_in = dup(STDIN_FILENO);
    if (saved_in < 0) {
        shell_puts("pipe: dup failed\n");
        for (int p = 0; p < n_pipes; p++) {
            close(pfd[p][0]);
            close(pfd[p][1]);
        }
        return 1;
    }
    int saved_out = dup(STDOUT_FILENO);
    if (saved_out < 0) {
        shell_puts("pipe: dup failed\n");
        close(saved_in);
        for (int p = 0; p < n_pipes; p++) {
            close(pfd[p][0]);
            close(pfd[p][1]);
        }
        return 1;
    }

    for (int s = 0; s < nstages; s++) {
        /* wire stdin */
        if (stage_in[s]) {
            /* explicit < overrides pipe */
            int ifd = open(stage_in[s], O_RDONLY);
            if (ifd < 0 || dup2(ifd, STDIN_FILENO) < 0) {
                if (ifd >= 0) close(ifd);
                shell_puts("pipe: open in failed\n");
                goto fail;
            }
            close(ifd);
        } else if (s > 0) {
            if (dup2(pfd[s - 1][0], STDIN_FILENO) < 0) {
                shell_puts("pipe: dup2 in failed\n");
                goto fail;
            }
        } else {
            dup2(saved_in, STDIN_FILENO);
        }
        /* wire stdout */
        if (stage_out[s]) {
            int flags = O_WRONLY | O_CREAT | (stage_app[s] ? O_APPEND : O_TRUNC);
            int ofd = open(stage_out[s], flags);
            if (ofd < 0 || dup2(ofd, STDOUT_FILENO) < 0) {
                if (ofd >= 0) close(ofd);
                shell_puts("pipe: open out failed\n");
                goto fail;
            }
            close(ofd);
        } else if (s < nstages - 1) {
            if (dup2(pfd[s][1], STDOUT_FILENO) < 0) {
                shell_puts("pipe: dup2 out failed\n");
                goto fail;
            }
        } else {
            dup2(saved_out, STDOUT_FILENO);
        }

        int pid = try_spawn(stage[s][0], stage[s]);
        if (pid < 0) {
            shell_puts(stage[s][0]);
            shell_puts(": not found\n");
            pids[s] = -1;
            goto fail;
        }
        pids[s] = pid;
    }

    for (int p = 0; p < n_pipes; p++) {
        close(pfd[p][0]);
        close(pfd[p][1]);
    }
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);

    if (background) {
        for (int s = 0; s < nstages; s++) {
            if (pids[s] > 0) {
                jobs_add(pids[s]);
                printf("[%d] %d\n", job_count, pids[s]);
            }
        }
        return 0;
    }

    setfg(0);
    for (int s = 0; s < nstages; s++) {
        if (pids[s] > 0)
            setfg(pids[s]);
    }
    int last = 0;
    for (int s = 0; s < nstages; s++) {
        if (pids[s] > 0) {
            int st = wait(pids[s]);
            last = (st < 0) ? 1 : (st & 0xff);
        }
    }
    setfg(0);
    return last;

fail:
    /* Unblock producers: close all pipe ends so writers get EPIPE/EOF */
    for (int p = 0; p < n_pipes; p++) {
        close(pfd[p][0]);
        close(pfd[p][1]);
    }
    if (saved_in >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }
    if (saved_out >= 0) {
        dup2(saved_out, STDOUT_FILENO);
        close(saved_out);
    }
    /* Kill survivors so we do not wait forever on a blocked writer */
    for (int s = 0; s < nstages; s++) {
        if (pids[s] > 0)
            kill(pids[s]);
    }
    for (int s = 0; s < nstages; s++) {
        if (pids[s] > 0)
            wait(pids[s]);
    }
    return 1;
}

static int run_program(char **argv, char *out_path, int append, char *in_path) {
    int saved_out = -1, saved_in = -1;
    int rc = 1;

    if (out_path || in_path) {
        if (apply_redirs(out_path, append, in_path, &saved_out, &saved_in) != 0) {
            restore_redirs(saved_out, saved_in);
            return 1;
        }
    }

    int pid = try_spawn(argv[0], argv);
    if (pid < 0) {
        restore_redirs(saved_out, saved_in);
        shell_puts(argv[0]);
        shell_puts(": not found\n");
        return 127;
    }
    setfg(0);
    setfg(pid);
    int st = wait(pid);
    setfg(0);
    rc = (st < 0) ? 1 : (st & 0xff);
    restore_redirs(saved_out, saved_in);
    return rc;
}

/* Parse NAME=value from a single token; returns 1 if form matched. */
static int parse_name_eq_value(const char *s, char *name, int nmax,
                               char *val, int vmax) {
    int i = 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return 0;
    while (s[i] && s[i] != '=' && i < nmax - 1) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
        name[i] = c;
        i++;
    }
    if (s[i] != '=')
        return 0;
    name[i] = 0;
    i++;
    int j = 0;
    while (s[i] && j < vmax - 1)
        val[j++] = s[i++];
    val[j] = 0;
    return 1;
}

static int cmd_set(int argc, char **argv) {
    if (argc == 1) {
        for (int i = 0; i < MAX_VARS; i++) {
            if (!vars[i].used)
                continue;
            shell_puts(vars[i].name);
            shell_puts("=");
            shell_puts(vars[i].value);
            if (vars[i].exported)
                shell_puts(" [export]");
            shell_puts("\n");
        }
        return 0;
    }
    char name[VAR_NAME], val[VAR_VAL];
    if (argc >= 2 && parse_name_eq_value(argv[1], name, VAR_NAME, val, VAR_VAL)) {
        if (var_set(name, val, 0) != 0) {
            shell_puts("set: table full\n");
            return 1;
        }
        return 0;
    }
    if (argc >= 3) {
        if (var_set(argv[1], argv[2], 0) != 0) {
            shell_puts("set: table full\n");
            return 1;
        }
        return 0;
    }
    shell_puts("set: usage: set | set NAME=value | set NAME value\n");
    return 1;
}

static int cmd_export(int argc, char **argv) {
    if (argc < 2) {
        for (int i = 0; i < MAX_VARS; i++) {
            if (!vars[i].used || !vars[i].exported)
                continue;
            shell_puts("export ");
            shell_puts(vars[i].name);
            shell_puts("=");
            shell_puts(vars[i].value);
            shell_puts("\n");
        }
        return 0;
    }
    char name[VAR_NAME], val[VAR_VAL];
    if (parse_name_eq_value(argv[1], name, VAR_NAME, val, VAR_VAL)) {
        if (var_set(name, val, 1) != 0) {
            shell_puts("export: table full\n");
            return 1;
        }
        return 0;
    }
    /* export NAME — mark existing or create empty exported */
    if (var_find(argv[1]) < 0)
        var_set(argv[1], "", 1);
    else {
        int i = var_find(argv[1]);
        vars[i].exported = 1;
    }
    return 0;
}

static int cmd_env(void) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (!vars[i].used)
            continue;
        if (!vars[i].exported)
            continue;
        shell_puts(vars[i].name);
        shell_puts("=");
        shell_puts(vars[i].value);
        shell_puts("\n");
    }
    return 0;
}

static int try_builtin(char **argv, int argc, char *out_path, int append,
                       char *in_path, int *status) {
    const char *cmd = argv[0];
    int is_help = (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0);
    int is_pid = (strcmp(cmd, "pid") == 0);
    int is_true = (strcmp(cmd, "true") == 0);
    int is_false = (strcmp(cmd, "false") == 0);
    int is_set = (strcmp(cmd, "set") == 0);
    int is_export = (strcmp(cmd, "export") == 0);
    int is_env = (strcmp(cmd, "env") == 0);
    int is_unset = (strcmp(cmd, "unset") == 0);
    int is_jobs = (strcmp(cmd, "jobs") == 0);
    int is_kill = (strcmp(cmd, "kill") == 0);
    int is_wait = (strcmp(cmd, "wait") == 0);
    int is_fg = (strcmp(cmd, "fg") == 0);
    int is_cd = (strcmp(cmd, "cd") == 0);
    int is_pwd = (strcmp(cmd, "pwd") == 0);

    if (!is_help && !is_pid && !is_true && !is_false &&
        !is_set && !is_export && !is_env && !is_unset && !is_jobs && !is_kill &&
        !is_wait && !is_fg && !is_cd && !is_pwd)
        return 0;

    int saved_out = -1, saved_in = -1;
    if (out_path || in_path) {
        if (apply_redirs(out_path, append, in_path, &saved_out, &saved_in) != 0) {
            restore_redirs(saved_out, saved_in);
            *status = 1;
            return 1;
        }
    }

    if (is_help) {
        cmd_help();
        *status = 0;
    } else if (is_pid) {
        printf("%d\n", getpid());
        *status = 0;
    } else if (is_true) {
        *status = 0;
    } else if (is_false) {
        *status = 1;
    } else if (is_set) {
        *status = cmd_set(argc, argv);
    } else if (is_export) {
        *status = cmd_export(argc, argv);
    } else if (is_env) {
        *status = cmd_env();
    } else if (is_unset) {
        if (argc < 2) {
            shell_puts("unset: usage: unset NAME\n");
            *status = 1;
        } else {
            var_unset(argv[1]);
            *status = 0;
        }
    } else if (is_jobs) {
        cmd_jobs();
        *status = 0;
    } else if (is_kill) {
        if (argc < 2) {
            shell_puts("kill: usage: kill PID\n");
            *status = 1;
        } else {
            int pid = 0;
            const char *s = argv[1];
            while (*s >= '0' && *s <= '9') {
                pid = pid * 10 + (*s - '0');
                s++;
            }
            if (pid <= 0 || kill(pid) != 0) {
                shell_puts("kill: failed\n");
                *status = 1;
            } else {
                jobs_remove(pid);
                *status = 0;
            }
        }
    } else if (is_wait) {
        *status = cmd_wait_builtin(argc, argv);
    } else if (is_fg) {
        *status = cmd_fg(argc, argv);
    } else if (is_cd) {
        const char *dir = (argc >= 2) ? argv[1] : "/";
        if (chdir(dir) != 0) {
            shell_puts("cd: failed\n");
            *status = 1;
        } else {
            *status = 0;
        }
    } else if (is_pwd) {
        char cwd[64];
        if (getcwd(cwd, sizeof(cwd)) == 0) {
            shell_puts(cwd);
            shell_puts("\n");
            *status = 0;
        } else {
            shell_puts("pwd: failed\n");
            *status = 1;
        }
    }

    restore_redirs(saved_out, saved_in);
    return 1;
}


/* Strip # comments outside quotes */
static void strip_comments(char *line) {
    int in_s = 0, in_d = 0;
    for (char *p = line; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (!in_d && *p == '\'')
            in_s = !in_s;
        else if (!in_s && *p == '"')
            in_d = !in_d;
        else if (!in_s && !in_d && *p == '#') {
            *p = 0;
            break;
        }
    }
}

static int is_list_op(const char *s) {
    if (!s || !s[0])
        return 0;
    if (s[0] == ';' && !s[1])
        return 1;
    if (s[0] == '&' && s[1] == '&' && !s[2])
        return 2;
    if (s[0] == '|' && s[1] == '|' && !s[2])
        return 3;
    if (s[0] == '&' && !s[1])
        return 4;
    return 0;
}

int cmd_sh_file(const char *path);

/* Run one pipeline or simple command (no list ops). background=1 → no wait. */
static int run_one(char **argv, int argc, int background) {
    if (argc <= 0)
        return 0;

    int has_pipe = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '|' && argv[i][1] == 0) {
            has_pipe = 1;
            break;
        }
    }
    if (has_pipe)
        return run_pipeline(argv, argc, background);

    char *out_path = 0, *in_path = 0;
    int append = 0;
    int ac = argc;
    if (parse_redirs(argv, &ac, &out_path, &append, &in_path) != 0) {
        shell_puts("redir: syntax error\n");
        return 1;
    }
    if (ac == 0) {
        shell_puts("redir: missing command\n");
        return 1;
    }

    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
        if (background) {
            shell_puts("exit: cannot background\n");
            return 1;
        }
        int code = parse_exit_code(ac, argv);
        shell_puts("bye\n");
        exit(code);
    }

    /* sh FILE — run script lines */
    if (strcmp(argv[0], "sh") == 0) {
        if (background) {
            shell_puts("sh: cannot background\n");
            return 1;
        }
        if (ac < 2) {
            shell_puts("sh: usage: sh FILE\n");
            return 1;
        }
        /* implemented below via cmd_sh after run_list exists — call inline */
        return cmd_sh_file(argv[1]);
    }

    int st = 0;
    if (try_builtin(argv, ac, out_path, append, in_path, &st))
        return st;

    if (background) {
        int saved_out = -1, saved_in = -1;
        if (out_path || in_path) {
            if (apply_redirs(out_path, append, in_path, &saved_out, &saved_in) != 0) {
                restore_redirs(saved_out, saved_in);
                return 1;
            }
        }
        int pid = try_spawn(argv[0], argv);
        restore_redirs(saved_out, saved_in);
        if (pid < 0) {
            shell_puts(argv[0]);
            shell_puts(": not found\n");
            return 127;
        }
        jobs_add(pid);
        printf("[%d] %d\n", job_count, pid);
        return 0;
    }

    return run_program(argv, out_path, append, in_path);
}

/* Execute argv list with ; && || and trailing & */
static int run_list(char **argv, int argc) {
    int i = 0;
    int status = 0;
    int skip = 0; /* skip next command (failed && or succeeded ||) */

    while (i < argc) {
        int start = i;
        int bg = 0;
        while (i < argc) {
            int op = is_list_op(argv[i]);
            if (op == 4) { /* & */
                bg = 1;
                break;
            }
            if (op == 1 || op == 2 || op == 3)
                break;
            /* single | is pipe, not list op */
            i++;
        }
        int cmd_argc = i - start;
        if (cmd_argc <= 0) {
            if (i < argc)
                i++;
            continue;
        }

        if (!skip) {
            char *save = argv[start + cmd_argc];
            argv[start + cmd_argc] = 0;
            status = run_one(&argv[start], cmd_argc, bg);
            argv[start + cmd_argc] = save;
            last_status = status;
        }
        skip = 0;

        if (i >= argc)
            break;
        int op = is_list_op(argv[i]);
        if (op == 0) {
            i++;
            continue;
        }
        i++; /* consume operator */
        if (op == 2) /* && */
            skip = (status != 0);
        else if (op == 3) /* || */
            skip = (status == 0);
        /* ; and & → skip stays 0 */
    }
    return status;
}

static int execute_line(char *line);

int cmd_sh_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        shell_puts("sh: cannot open ");
        shell_puts(path);
        shell_puts("\n");
        return 1;
    }
    char line[LINE_MAX];
    int n = 0;
    int st = 0;
    for (;;) {
        char c;
        long r = read(fd, &c, 1);
        if (r <= 0)
            break;
        if (c == '\n' || c == '\r') {
            line[n] = 0;
            if (n > 0)
                st = execute_line(line);
            n = 0;
            continue;
        }
        if (n + 1 < LINE_MAX)
            line[n++] = c;
        else {
            shell_puts("sh: line too long (truncated)\n");
            /* drain until newline */
            while (r > 0 && c != '\n' && c != '\r') {
                r = read(fd, &c, 1);
            }
            n = 0;
            last_status = 1;
            continue;
        }
    }
    if (n > 0) {
        line[n] = 0;
        st = execute_line(line);
    }
    close(fd);
    return st;
}

static int execute_line(char *line) {
    strip_comments(line);
    char *args[ARGS_SLOTS];
    int argc2 = tokenize(line, args, MAX_ARGS);
    if (argc2 < 0) {
        if (argc2 == -2)
            shell_puts("syntax: line too complex (expand pool full)\n");
        else if (argc2 == -3)
            shell_puts("syntax: unclosed ${\n");
        else
            shell_puts("syntax: unclosed quote\n");
        last_status = 1;
        return 1;
    }
    if (argc2 == 0)
        return last_status;
    args[argc2] = 0;
    last_status = run_list(args, argc2);
    return last_status;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char line[LINE_MAX];

    /* Standard layout (v1.1.3): programs live under bin/ ; PATH defaults to bin:. */
    if (!var_get("PATH"))
        var_set("PATH", "bin:.", 1);

    shell_puts("TigxOS shell " TIGX_VERSION "\n");
    shell_puts("Type help for commands.\n");

    for (;;) {
        jobs_reap_done();
        prompt();
        int n = readline(line, LINE_MAX);
        if (n < 0) {
            sleep_ms(50); /* avoid busy-loop if read fails */
            continue;
        }
        if (n == 0)
            continue;
        hist_add(line);
        execute_line(line);
    }
    return 0;
}
