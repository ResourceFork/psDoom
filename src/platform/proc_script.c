/*
 * psDoom -- external-script process backend (implementation). See proc_script.h
 * and docs/script-backend.md for the wire contract.
 *
 * - Enumerate: run PSDOOM_POLL_CMD through the shell, capture its stdout (with a
 *   timeout so a slow/hung script never freezes the game), and parse the
 *   tab-separated key=value lines into psd_proc_t. An id outside 2..INT_MAX,
 *   or not plain decimal digits, drops the line.
 * - Act: run PSDOOM_RESPOND_CMD *directly* (no shell) as
 *   `<verb> <id> [key=value...]`, double-forked so it never blocks the game and
 *   never leaks a zombie. Direct exec means an entity's label/id can't be
 *   interpreted as shell syntax. An id <= 1 is never handed to it.
 * - Both commands run in a process group of their own, get /dev/null as
 *   stdin and inherit no descriptor above stderr. A poll that overruns its
 *   timeout is stopped as a whole group (its pipeline and background children
 *   too), and only its complete lines are parsed.
 */

#include "proc_script.h"
#include "proc_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#ifdef __APPLE__
#include <libproc.h>   /* proc_pidinfo(PROC_PIDLISTFDS) */
#endif

/* ------------------------------------------------------------------ config */

#define SCRIPT_CMD_MAX     1024   /* max length of a configured command string */
#define SCRIPT_BUF_CAP  (512 * 1024) /* max poll stdout we read in one cycle    */
#define SCRIPT_LINE_MAX    2048   /* a longer poll line is dropped, not clipped  */
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

/* Parse `val` as plain decimal digits (no sign, space or trailing character)
 * with a value in min..INT_MAX. Returns 1 and sets *out on success, 0 if the
 * value is empty, malformed or out of range (so nothing ever wraps). */
static int script_parse_int(const char *val, int min, int *out)
{
    const char *s;
    int         v = 0;

    if (val == NULL || val[0] == '\0')
    {
        return 0;
    }
    for (s = val; *s != '\0'; s++)
    {
        int d;

        if (*s < '0' || *s > '9')
        {
            return 0;
        }
        d = *s - '0';
        if (v > (INT_MAX - d) / 10)
        {
            return 0;   /* beyond INT_MAX */
        }
        v = v * 10 + d;
    }
    if (v < min)
    {
        return 0;
    }
    *out = v;
    return 1;
}

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
        return 0;   /* too long to parse whole: dropped, never clipped */
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
            /* 2..INT_MAX only (0 and 1 are reserved). The last id wins, so an
             * invalid last id drops the line rather than keeping an earlier
             * one. */
            have_id = script_parse_int(val, 2, &p->pid);
            if (!have_id)
            {
                p->pid = 0;
            }
        }
        else if (strcmp(key, "parent") == 0)
        {
            long long v = strtoll(val, NULL, 10);
            /* Lenient as before, but a value beyond int range is "no
             * parent", never a wrapped id. */
            p->ppid = (v > 0 && v <= INT_MAX) ? (int) v : 0;
        }
        else if (strcmp(key, "weight") == 0)
        {
            p->footprint = strtoull(val, NULL, 10);
        }
        else if (strcmp(key, "load") == 0)
        {
            long long v = strtoll(val, NULL, 10);
            p->cpu_percent = (v <= 0) ? 0 : (v > INT_MAX) ? INT_MAX : (int) v;
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
            /* Too long to parse whole: drop it. Clipping could cut an id
             * field into a shorter, different id. */
            if (nl == NULL)
            {
                break;
            }
            line = nl + 1;
            continue;
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

/* Slack above the highest open descriptor, for descriptors another thread
 * opens between this listing and the fork. */
#define SCRIPT_FD_SLACK 64

/*
 * One past the highest descriptor the child must close. Computed in the
 * parent, before fork (none of this is async-signal-safe). On macOS it is
 * the highest descriptor actually open (listed with proc_pidinfo) plus slack:
 * that also covers a descriptor above a lowered soft RLIMIT_NOFILE, and keeps
 * the close loop short when the limit is huge. Elsewhere, or if the listing
 * fails, it is the descriptor table size.
 */
static int script_fd_limit(void)
{
    int n = getdtablesize();

#ifdef __APPLE__
    {
        int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, NULL, 0);

        if (bytes > 0)
        {
            size_t              cap  = (size_t) bytes
                                     + SCRIPT_FD_SLACK * sizeof(struct proc_fdinfo);
            struct proc_fdinfo *fdis = malloc(cap);

            if (fdis != NULL)
            {
                int got = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fdis, (int) cap);

                if (got > 0)
                {
                    int count = got / (int) sizeof(struct proc_fdinfo);
                    int high  = STDERR_FILENO;
                    int i;

                    for (i = 0; i < count; i++)
                    {
                        if (fdis[i].proc_fd > high)
                        {
                            high = fdis[i].proc_fd;
                        }
                    }
                    free(fdis);
                    return high + 1 + SCRIPT_FD_SLACK;
                }
                free(fdis);
            }
        }
    }
#endif
    return (n > 256) ? n : 256;
}

static void script_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    if (flags != -1)
    {
        (void) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

/* /dev/null for a child's stdin, opened close-on-exec in the parent. -1 if
 * it cannot be opened (the child then keeps the inherited stdin). */
static int script_open_devnull(void)
{
    int fd = open("/dev/null", O_RDONLY);

    if (fd >= 0)
    {
        script_set_cloexec(fd);
    }
    return fd;
}

/*
 * In a freshly forked child: move into a new process group of its own (so a
 * timeout can stop the whole command, pipeline and background jobs included),
 * make `stdin_fd` (if >= 0; /dev/null, since a background group reading the
 * terminal would be stopped by SIGTTIN) its stdin and `stdout_fd` (if >= 0)
 * its stdout, close every descriptor above stderr up to `fd_limit` (the
 * game's files, sockets and WAD never reach a script), and exec `path` (or
 * search PATH for argv[0] if `path` is NULL). The game is multi-threaded, so
 * the parent builds argv and the limit before fork; setpgid, dup2, fcntl and
 * close are async-signal-safe, and execv is, but execvp's PATH search is not
 * strictly so (as before this change). Never returns.
 */
static void script_child_exec(int stdin_fd, int stdout_fd, int fd_limit,
                              const char *path, char *const argv[])
{
    int fd;

    (void) setpgid(0, 0);
    if (stdin_fd >= 0 && stdin_fd != STDIN_FILENO
        && dup2(stdin_fd, STDIN_FILENO) < 0)
    {
        _exit(127);
    }
    if (stdout_fd == STDOUT_FILENO)
    {
        (void) fcntl(STDOUT_FILENO, F_SETFD, 0);   /* keep it across exec */
    }
    else if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0)
    {
        _exit(127);
    }
    for (fd = STDERR_FILENO + 1; fd < fd_limit; fd++)
    {
        (void) close(fd);
    }
    if (path != NULL)
    {
        execv(path, argv);
    }
    else
    {
        execvp(argv[0], argv);
    }
    _exit(127);
}

static void script_reap(pid_t pid)
{
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
    {
    }
}

/* 1 if `pid` is still our unreaped child (running or a zombie). Does not reap
 * it. While that holds, its pid -- and so the process group id it leads --
 * cannot have been handed to any other process. */
static int script_child_unreaped(pid_t pid)
{
    siginfo_t info;

    for (;;)
    {
        memset(&info, 0, sizeof(info));
        if (waitid(P_PID, (id_t) pid, &info,
                   WEXITED | WSTOPPED | WNOHANG | WNOWAIT) == 0)
        {
            return 1;
        }
        if (errno != EINTR)
        {
            return 0;
        }
    }
}

/*
 * Stop the poll command's process group, `pgid` (the recorded pid of the
 * child that leads it), then reap that child. The group is signalled only
 * while its leader is verified to be our unreaped child, so the id is ours;
 * if it is not, nothing is signalled. Members of the group other than the
 * leader are not our children: once killed, launchd reaps them.
 */
static void script_stop_group(pid_t pgid)
{
    if (pgid <= 1 || !script_child_unreaped(pgid))
    {
        return;
    }
    if (kill(-pgid, SIGKILL) != 0)
    {
        (void) kill(pgid, SIGKILL);   /* group not formed: the leader alone */
    }
    script_reap(pgid);
}

/*
 * Run `cmd` via the shell, in a process group of its own, and capture up to
 * `cap`-1 bytes of its stdout into `buf` (always NUL-terminated). If it
 * overruns `timeout_ms` (still writing, or holding stdout open, or not
 * exiting after EOF), or fills the buffer, the whole group is killed and its
 * leader reaped, so no part of the command outlives the poll. Returns bytes
 * captured (>= 0), or -1 if the child could not be spawned.
 */
static int script_run_capture(const char *cmd, char *buf, int cap, int timeout_ms)
{
    int                fds[2];
    pid_t              pid;
    int                total = 0;
    int                stop  = 0;
    int                fd_limit;
    int                devnull;
    char              *argv[4];
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
    script_set_cloexec(fds[0]);
    script_set_cloexec(fds[1]);

    argv[0]  = "sh";
    argv[1]  = "-c";
    argv[2]  = (char *) cmd;
    argv[3]  = NULL;
    devnull  = script_open_devnull();
    fd_limit = script_fd_limit();

    pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        if (devnull >= 0)
        {
            close(devnull);
        }
        return -1;
    }
    if (pid == 0)
    {
        script_child_exec(devnull, fds[1], fd_limit, "/bin/sh", argv);
    }
    if (devnull >= 0)
    {
        close(devnull);
    }

    /* Record the group: its id is the child's pid. Set it here as well as in
     * the child, so it exists before any timeout below whichever runs first
     * (this fails harmlessly once the child has exec'd). */
    (void) setpgid(pid, pid);

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
            stop = 1;             /* timed out */
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
            stop = 1;
            break;
        }
        if (sel == 0)
        {
            stop = 1;             /* timed out */
            break;
        }

        space = cap - 1 - total;
        if (space <= 0)
        {
            stop = 1;             /* buffer full; stop reading */
            break;
        }

        r = read(fds[0], buf + total, (size_t) space);
        if (r < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            stop = 1;
            break;
        }
        if (r == 0)
        {
            break;                /* EOF: command closed its stdout */
        }
        total += (int) r;
    }

    /* Cut off before EOF (timeout, full buffer, error): keep only complete
     * lines. The line being written is unfinished, and parsing it could turn
     * a half-written id (42 of 4242) into a different, valid one. */
    if (stop)
    {
        int keep = total;

        while (keep > 0 && buf[keep - 1] != '\n')
        {
            keep--;
        }
        total = keep;
    }
    buf[total] = '\0';
    close(fds[0]);

    /* After EOF the command should exit at once; give it until the deadline,
     * never longer, so the game is not held by a command that closed stdout
     * and kept running. */
    while (!stop)
    {
        pid_t w = waitpid(pid, NULL, WNOHANG);

        if (w == pid || (w < 0 && errno != EINTR))
        {
            return total;         /* exited and reaped */
        }
        if (script_mono_ms() >= deadline)
        {
            stop = 1;
            break;
        }
        {
            struct timespec ts = { 0, 2 * 1000 * 1000 };   /* 2 ms */
            nanosleep(&ts, NULL);
        }
    }

    script_stop_group(pid);
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
 * directly (no shell), in a process group of its own, with /dev/null as stdin
 * and no descriptor above stderr. Double-forked so the game neither blocks on it nor
 * accumulates zombies. Returns 0 if refused (an id <= 1 is never handed to
 * the responder), else 1 (dispatched, or no response command configured).
 */
static int script_run_respond(const char *verb, int id,
                              int have_amount, int amount, const char *label)
{
    char  cmd[SCRIPT_CMD_MAX];
    char  idbuf[32];
    char  amountbuf[48];
    char  labelbuf[64];
    char *argv[SCRIPT_ARGV_MAX];
    int   n;
    int   fd_limit;
    int   devnull;
    pid_t pid;

    if (id <= 1)
    {
        return 0;   /* 0, 1 and negatives are reserved: never a target */
    }
    if (script_respond_cmd[0] == '\0')
    {
        return 1;
    }

    /* Build argv before fork (snprintf is not async-signal-safe): the
     * command and its static args from the configured string, then the
     * psDoom-supplied arguments (the documented contract). */
    snprintf(cmd, sizeof(cmd), "%s", script_respond_cmd);
    n = script_split_cmd(cmd, argv, SCRIPT_ARGV_MAX - 6);
    if (n <= 0)
    {
        return 1;
    }
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
    argv[n]  = NULL;
    devnull  = script_open_devnull();
    fd_limit = script_fd_limit();

    pid = fork();
    if (pid == 0)
    {
        /* intermediate child: fork again so the grandchild is reparented to
         * launchd (auto-reaped) and we never wait on the actual command. */
        if (fork() == 0)
        {
            script_child_exec(devnull, -1, fd_limit, NULL, argv);
        }
        _exit(0);         /* intermediate child exits immediately */
    }
    if (devnull >= 0)
    {
        close(devnull);
    }
    if (pid < 0)
    {
        return 1;
    }

    script_reap(pid);     /* reap the intermediate child (exits at once) */
    return 1;
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
        script_last_n = 0;   /* state unknown: nothing counts as live */
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
    (void) script_run_respond("wound", pid, 1, nice_delta, script_label_for(pid));
}

static int script_kill(int pid)
{
    /* Fire-and-forget: we can't synchronously know the script's exit, so report
     * "dispatched". A still-present entity simply reappears on the next poll.
     * 0 only when refused (an id <= 1). */
    return script_run_respond("kill", pid, 0, 0, script_label_for(pid));
}

/* Script entities can't be re-read without running the poll command (slow, and
 * it would block the game tick), and the contract carries no start time. So
 * "now" is the most recent poll: an id is live if that poll listed it.
 * start_token is always 0, so identity is the id alone, as the contract
 * defines (psd_proc_same skips the label). */
static int script_lookup(int id, psd_proc_t *out)
{
    int i;

    if (out == NULL)
    {
        return 0;
    }
    for (i = 0; i < script_last_n; i++)
    {
        if (script_last[i].pid == id)
        {
            *out = script_last[i];
            return 1;
        }
    }
    return 0;
}

static const proc_backend_t script_backend =
{
    script_list,
    script_current_uid,
    script_renice,
    script_kill,
    script_lookup,
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
