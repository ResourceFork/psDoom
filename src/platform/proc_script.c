/*
 * psDoom -- external-script process backend (implementation). See proc_script.h
 * and docs/script-backend.md for the wire contract.
 *
 * - Enumerate: run PSDOOM_POLL_CMD through the shell, capture its stdout (with a
 *   timeout so a slow/hung script never freezes the game), and parse the
 *   tab-separated key=value lines into psd_proc_t.
 * - Act: run PSDOOM_RESPOND_CMD *directly* (no shell) as
 *   `<verb> <id> [key=value...]`, double-forked so it never blocks the game and
 *   never leaks a zombie. Direct exec means an entity's label/id can't be
 *   interpreted as shell syntax.
 */

#include "proc_script.h"
#include "proc_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>

/* ------------------------------------------------------------------ config */

#define SCRIPT_CMD_MAX     1024   /* max length of a configured command string */
#define SCRIPT_BUF_CAP  (512 * 1024) /* max poll stdout we read in one cycle    */
#define SCRIPT_LINE_MAX    2048   /* longest poll line we parse (excess clipped) */
#define SCRIPT_ARGV_MAX      64   /* argv slots for the response command         */
#define SCRIPT_SNAPSHOT_MAX 4096  /* cached entities for label lookup            */

/* ------------------------------------------------------------------- state */

static char script_poll_cmd[SCRIPT_CMD_MAX];
static char script_respond_cmd[SCRIPT_CMD_MAX];
static int  script_timeout_ms = 1000;

/* The last poll's entities, kept so a response can attach the entity's label
 * (the vtable's renice/kill receive only an id). */
static psd_proc_t script_last[SCRIPT_SNAPSHOT_MAX];
static int        script_last_n;

/* --------------------------------------------------------------- pure parse */

/* Parse one already-isolated line into `p`. Returns 1 if it carries a valid
 * entity (a usable `id`), 0 otherwise. */
static int script_parse_line(const char *line, psd_proc_t *p)
{
    char        work[SCRIPT_LINE_MAX];
    const char *s;
    char       *tok;
    char       *save = NULL;
    int         have_id = 0;
    size_t      len;

    /* Skip leading whitespace; ignore blanks and # comments. */
    for (s = line; *s == ' ' || *s == '\t'; s++)
    {
    }
    if (*s == '\0' || *s == '#')
    {
        return 0;
    }

    len = strlen(line);
    if (len >= sizeof(work))
    {
        len = sizeof(work) - 1;
    }
    memcpy(work, line, len);
    work[len] = '\0';

    /* Defaults. */
    memset(p, 0, sizeof(*p));

    for (tok = strtok_r(work, "\t", &save);
         tok != NULL;
         tok = strtok_r(NULL, "\t", &save))
    {
        char *eq = strchr(tok, '=');
        const char *key;
        const char *val;

        if (eq == NULL)
        {
            continue;   /* not a key=value field */
        }
        *eq = '\0';
        key = tok;
        val = eq + 1;

        if (strcmp(key, "id") == 0)
        {
            long long v = strtoll(val, NULL, 10);
            if (v > 1)                       /* 0 and 1 are reserved */
            {
                p->pid  = (int) v;
                have_id = 1;
            }
        }
        else if (strcmp(key, "parent") == 0)
        {
            long long v = strtoll(val, NULL, 10);
            p->ppid = (v > 0) ? (int) v : 0;
        }
        else if (strcmp(key, "weight") == 0)
        {
            p->footprint = strtoull(val, NULL, 10);
        }
        else if (strcmp(key, "load") == 0)
        {
            long long v = strtoll(val, NULL, 10);
            p->cpu_percent = (v > 0) ? (int) v : 0;
        }
        else if (strcmp(key, "flags") == 0)
        {
            long long v = strtoll(val, NULL, 10);
            p->is_daemon = (v & 1) ? 1 : 0;
        }
        else if (strcmp(key, "label") == 0)
        {
            strncpy(p->name, val, sizeof(p->name) - 1);
            p->name[sizeof(p->name) - 1] = '\0';
        }
        /* unknown keys: ignored (forward-compatibility) */
    }

    return have_id;
}

int psd_script_parse(const char *buf, psd_proc_t *out, int max)
{
    const char *line;
    int         count = 0;

    if (buf == NULL || out == NULL || max <= 0)
    {
        return 0;
    }

    line = buf;
    while (*line != '\0' && count < max)
    {
        const char *nl = strchr(line, '\n');
        size_t      len = (nl != NULL) ? (size_t) (nl - line) : strlen(line);
        char        linebuf[SCRIPT_LINE_MAX];

        if (len >= sizeof(linebuf))
        {
            len = sizeof(linebuf) - 1;
        }
        memcpy(linebuf, line, len);
        linebuf[len] = '\0';

        /* Drop a trailing CR (CRLF inputs). */
        if (len > 0 && linebuf[len - 1] == '\r')
        {
            linebuf[len - 1] = '\0';
        }

        if (script_parse_line(linebuf, &out[count]))
        {
            count++;
        }

        if (nl == NULL)
        {
            break;
        }
        line = nl + 1;
    }
    return count;
}

/* ----------------------------------------------------------- subprocess I/O */

static unsigned long long script_mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000ULL
         + (unsigned long long) ts.tv_nsec / 1000000ULL;
}

/*
 * Run `cmd` via the shell and capture up to `cap`-1 bytes of its stdout into
 * `buf` (always NUL-terminated). Kills the child if it exceeds `timeout_ms`.
 * Returns bytes captured (>= 0), or -1 if the child could not be spawned.
 */
static int script_run_capture(const char *cmd, char *buf, int cap, int timeout_ms)
{
    int                fds[2];
    pid_t              pid;
    int                total = 0;
    unsigned long long deadline;

    if (cmd == NULL || cmd[0] == '\0' || buf == NULL || cap <= 0)
    {
        return -1;
    }
    buf[0] = '\0';

    if (pipe(fds) != 0)
    {
        return -1;
    }

    pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0)
    {
        /* child: stdout -> pipe, then run the (user-owned) shell command */
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
        _exit(127);
    }

    /* parent: read with a deadline */
    close(fds[1]);
    deadline = script_mono_ms() + (unsigned long long) (timeout_ms > 0 ? timeout_ms : 1000);

    for (;;)
    {
        unsigned long long now = script_mono_ms();
        long           remaining = (long) ((long long) deadline - (long long) now);
        fd_set         rfds;
        struct timeval tv;
        int            sel;
        int            space;
        ssize_t        r;

        if (remaining <= 0)
        {
            kill(pid, SIGKILL);
            break;
        }

        FD_ZERO(&rfds);
        FD_SET(fds[0], &rfds);
        tv.tv_sec  = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;

        sel = select(fds[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (sel == 0)
        {
            kill(pid, SIGKILL);   /* timed out */
            break;
        }

        space = cap - 1 - total;
        if (space <= 0)
        {
            kill(pid, SIGKILL);   /* buffer full; stop reading */
            break;
        }

        r = read(fds[0], buf + total, (size_t) space);
        if (r < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (r == 0)
        {
            break;                /* EOF: command finished */
        }
        total += (int) r;
    }

    buf[total] = '\0';
    close(fds[0]);
    waitpid(pid, NULL, 0);        /* reap (already dead, or killed above) */
    return total;
}

/* Split `cmd` (a copy is made by the caller) on whitespace into argv[], up to
 * `max`-headroom slots; returns the count. */
static int script_split_cmd(char *cmd, char **argv, int max)
{
    char *save = NULL;
    char *tok;
    int   n = 0;

    for (tok = strtok_r(cmd, " \t", &save);
         tok != NULL && n < max;
         tok = strtok_r(NULL, " \t", &save))
    {
        argv[n++] = tok;
    }
    return n;
}

/*
 * Invoke the response command as `<verb> <id> [amount=N] [label=...]`, exec'd
 * directly (no shell). Double-forked so the game neither blocks on it nor
 * accumulates zombies. No-op if no response command is configured.
 */
static void script_run_respond(const char *verb, int id,
                               int have_amount, int amount, const char *label)
{
    pid_t pid;

    if (script_respond_cmd[0] == '\0')
    {
        return;
    }

    pid = fork();
    if (pid < 0)
    {
        return;
    }
    if (pid == 0)
    {
        /* intermediate child: fork again so the grandchild is reparented to
         * init (auto-reaped) and we never wait on the actual command. */
        pid_t grandchild = fork();

        if (grandchild == 0)
        {
            char  cmd[SCRIPT_CMD_MAX];
            char  idbuf[32];
            char  amountbuf[48];
            char  labelbuf[64];
            char *argv[SCRIPT_ARGV_MAX];
            int   n;

            /* command + its static args from the configured string */
            snprintf(cmd, sizeof(cmd), "%s", script_respond_cmd);
            n = script_split_cmd(cmd, argv, SCRIPT_ARGV_MAX - 6);

            /* psDoom-supplied arguments (the documented contract) */
            snprintf(idbuf, sizeof(idbuf), "%d", id);
            argv[n++] = (char *) verb;
            argv[n++] = idbuf;
            if (have_amount)
            {
                snprintf(amountbuf, sizeof(amountbuf), "amount=%d", amount);
                argv[n++] = amountbuf;
            }
            if (label != NULL && label[0] != '\0')
            {
                snprintf(labelbuf, sizeof(labelbuf), "label=%s", label);
                argv[n++] = labelbuf;
            }
            argv[n] = NULL;

            if (n > 0 && argv[0] != NULL)
            {
                execvp(argv[0], argv);
            }
            _exit(127);   /* exec failed */
        }

        _exit(0);         /* intermediate child exits immediately */
    }

    waitpid(pid, NULL, 0);   /* reap the intermediate child (exits at once) */
}

/* --------------------------------------------------------------- vtable ops */

/* The label last seen for `id`, or NULL. */
static const char *script_label_for(int id)
{
    int i;

    for (i = 0; i < script_last_n; i++)
    {
        if (script_last[i].pid == id)
        {
            return script_last[i].name;
        }
    }
    return NULL;
}

static int script_list(psd_proc_t *out, int max)
{
    static char  buf[SCRIPT_BUF_CAP];
    unsigned int uid = (unsigned int) getuid();
    int          n;
    int          i;

    if (script_run_capture(script_poll_cmd, buf, sizeof(buf), script_timeout_ms) < 0)
    {
        return 0;
    }

    n = psd_script_parse(buf, out, max);

    /* Stamp the current uid so script entities pass the (uid) triage filter,
     * and cache the snapshot for label lookup during a response. */
    script_last_n = (n < SCRIPT_SNAPSHOT_MAX) ? n : SCRIPT_SNAPSHOT_MAX;
    for (i = 0; i < n; i++)
    {
        out[i].uid = uid;
        if (i < script_last_n)
        {
            script_last[i] = out[i];
        }
    }
    return n;
}

static unsigned int script_current_uid(void)
{
    return (unsigned int) getuid();
}

static void script_renice(int pid, int nice_delta)
{
    script_run_respond("wound", pid, 1, nice_delta, script_label_for(pid));
}

static int script_kill(int pid)
{
    /* Fire-and-forget: we can't synchronously know the script's exit, so report
     * "dispatched". A still-present entity simply reappears on the next poll. */
    script_run_respond("kill", pid, 0, 0, script_label_for(pid));
    return 1;
}

static const proc_backend_t script_backend =
{
    script_list,
    script_current_uid,
    script_renice,
    script_kill,
};

/* ----------------------------------------------------------------- install */

void proc_script_install(const char *poll_cmd, const char *respond_cmd,
                         int timeout_ms)
{
    snprintf(script_poll_cmd, sizeof(script_poll_cmd), "%s",
             poll_cmd != NULL ? poll_cmd : "");
    snprintf(script_respond_cmd, sizeof(script_respond_cmd), "%s",
             respond_cmd != NULL ? respond_cmd : "");
    script_timeout_ms = (timeout_ms > 0) ? timeout_ms : 1000;
    script_last_n     = 0;

    proc_backend_set(&script_backend);
}

int proc_script_install_from_env(void)
{
    const char *poll = getenv("PSDOOM_POLL_CMD");
    const char *respond;
    const char *timeout;
    int         ms = 0;

    if (poll == NULL || poll[0] == '\0')
    {
        return 0;   /* not configured: keep the native backend */
    }

    respond = getenv("PSDOOM_RESPOND_CMD");
    timeout = getenv("PSDOOM_POLL_TIMEOUT_MS");
    if (timeout != NULL)
    {
        ms = atoi(timeout);
    }

    proc_script_install(poll, respond, ms);
    fprintf(stderr, "psDoom: script backend active (poll: %s)\n", poll);
    return 1;
}
