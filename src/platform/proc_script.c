/*
 * psDoom -- external-script process backend (implementation). See proc_script.h
 * and docs/script-backend.md for the wire contract.
 *
 * - Enumerate: run PSDOOM_POLL_CMD through the shell on a worker thread, so
 *   the game thread never waits for it, capture its stdout (with a timeout, so
 *   a hung script is stopped), and parse the tab-separated key=value lines
 *   into psd_proc_t on the game thread. list() starts a poll and returns -1
 *   ("not yet") until that poll's result is ready; the call after it ends
 *   consumes it. One poll runs at a time. The worker touches only its job
 *   (command, output buffer, result), handed over under script_job_mu; every
 *   other piece of state here belongs to the game thread. An id outside
 *   2..INT_MAX, or not plain decimal digits, drops the line.
 * - Act: run PSDOOM_RESPOND_CMD *directly* (no shell) as
 *   `<verb> <id> [key=value...]` (verb `post` for a wound, `delete` for a
 *   kill), double-forked so it never blocks the game and
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
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#ifdef __APPLE__
#include <libproc.h>   /* proc_pidinfo(PROC_PIDLISTFDS) */
#endif

extern char **environ;

/* ------------------------------------------------------------------ config */

#define SCRIPT_CMD_MAX     1024   /* max length of a configured command string */
#define SCRIPT_BUF_CAP  (512 * 1024) /* max poll stdout we read in one cycle    */
#define SCRIPT_LINE_MAX    2048   /* a longer poll line is dropped, not clipped  */
#define SCRIPT_ARGV_MAX      64   /* argv slots for the response command         */
#define SCRIPT_SNAPSHOT_MAX 4096  /* cached entities for label lookup            */
#define SCRIPT_RESULT_MAX_WAIT_MS 1000 /* a result kept longer (plus the timeout) is dropped */
#define SCRIPT_TIMEOUT_GRACE  5   /* timeouts in a row that keep the last list    */

/* ------------------------------------------------------------------- state */

static char script_poll_cmd[SCRIPT_CMD_MAX];
static char script_respond_cmd[SCRIPT_CMD_MAX];

/* A fixed responder argv (the bundled Summoner: python3 + summon), used
 * instead of splitting script_respond_cmd on whitespace, so paths with
 * spaces (an app in "/Applications/psDoom copy.app") stay one argument. */
static char script_resp_fixed[4][SCRIPT_CMD_MAX];
static int  script_resp_nfixed;

/* Why there are no process monsters, once known ("" while all is well);
 * set when the poll fails and logged once per failure streak. */
static char script_status[160];
static int  script_poll_failing;
static int  script_timeout_ms = 1000;
/* A poll that times out keeps the last good list (list() says "no new
 * list yet"), up to SCRIPT_TIMEOUT_GRACE in a row; only then, or with no
 * good list yet, does it list nothing. */
static int  script_have_list;   /* a poll answered since install       */
static int  script_timeouts;    /* timeouts in a row with no answer     */

/* The last poll's entities, kept so a response can attach the entity's label
 * (the vtable's renice/kill receive only an id). */
static psd_proc_t script_last[SCRIPT_SNAPSHOT_MAX];
static int        script_last_n;

/* The dimensions the last poll declared (`# dim` lines), the sort it says
 * it applied (`# sort`), and the host's request: the sort the player chose
 * ("" = the plugin's default) and the candidate limit. */
static psd_script_dim_t script_dims[PSD_SCRIPT_DIMS_MAX];
static int              script_ndims;
static char             script_applied[PSD_SCRIPT_ID_MAX + 1];
static char             script_req_sort[PSD_SCRIPT_ID_MAX + 1];
static int              script_req_limit;

/* The last poll's `# effect` words: what a wound and a kill do. */
static char             script_effect_post[PSD_SCRIPT_EFFECT_MAX];
static char             script_effect_del[PSD_SCRIPT_EFFECT_MAX];

/* The host's effect mode, sent as `mode=` on every response so Summoner
 * decides what runs: "dryrun" (the safe default), "post-only", "delete". */
static char             script_mode[16] = "dryrun";

/* Bumped by every install, so a poll started under an earlier configuration
 * is dropped rather than served as this one's. */
static unsigned int     script_gen;

/* ---- the poll job, shared with the worker thread (under script_job_mu) -- */

enum { SCRIPT_JOB_IDLE, SCRIPT_JOB_RUNNING, SCRIPT_JOB_DONE };

static pthread_mutex_t script_job_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  script_job_cv = PTHREAD_COND_INITIALIZER;
static int             script_job_state;      /* SCRIPT_JOB_*              */
static unsigned int    script_job_gen;        /* script_gen it was made for */
static int             script_job_rc;         /* bytes captured, or -1      */
static int             script_job_cut;        /* cut off before EOF         */
static int             script_job_exit;       /* exit status, if it exited  */
static int             script_job_timeout_ms;
static unsigned long long script_job_done_ms; /* when it finished (mono)  */
static char            script_job_cmd[SCRIPT_CMD_MAX + 64];
/* Written only by whoever runs the job (state RUNNING), read only by the
 * game thread once it is DONE. */
static char            script_job_buf[SCRIPT_BUF_CAP];
/* 0: not started yet; 1: running; -1: could not start (polls then run on
 * the game thread, as before, rather than not at all). */
static int             script_worker;

/* The poll child in flight (the leader of its process group), 0 if none.
 * Set at fork and cleared at reap under script_child_mu, so the worker's
 * timeout teardown and the at-exit teardown never both act on one child and
 * never act on a pid after it was reaped. */
static pthread_mutex_t script_child_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t           script_child;
/* Set by the at-exit teardown: no poll child is started after it. */
static int             script_child_closed;

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

/* Parse a `grade` value: a finite decimal number, clamped to [0, 1].
 * Returns 1 and sets *out, or 0 (malformed: no digits, trailing characters,
 * a hex float, NaN, infinity, or a value too large to represent) so the
 * field is ignored. */
static int script_parse_grade(const char *val, double *out)
{
    char  *end = NULL;
    double v;

    if (val == NULL || *val == '\0' || isspace((unsigned char) *val)
        || strpbrk(val, "xX") != NULL)   /* decimal only: no 0x1p-1 */
    {
        return 0;
    }
    v = strtod(val, &end);
    if (end == val || *end != '\0' || !isfinite(v))
    {
        return 0;
    }
    if (v < 0.0)
    {
        v = 0.0;
    }
    if (v > 1.0)
    {
        v = 1.0;
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
        else if (strcmp(key, "group") == 0)
        {
            strncpy(p->group, val, sizeof(p->group) - 1);
            p->group[sizeof(p->group) - 1] = '\0';
        }
        else if (strcmp(key, "children") == 0)
        {
            /* The backend's own child count, a whole number >= 0; anything
             * else is ignored. */
            int c;

            if (script_parse_int(val, 0, &c))
            {
                p->children     = c;
                p->has_children = 1;
            }
            else
            {
                p->has_children = 0;
            }
        }
        else if (strcmp(key, "grade") == 0)
        {
            double g;

            /* The last valid grade wins; a malformed one is ignored. */
            if (script_parse_grade(val, &g))
            {
                p->grade     = g;
                p->has_grade = 1;
            }
        }
        /* unknown keys: ignored (forward-compatibility) */
    }

    return have_id;
}

int psd_script_id_ok(const char *id)
{
    size_t i;

    if (id == NULL || !(id[0] >= 'a' && id[0] <= 'z'))
    {
        return 0;
    }
    for (i = 0; id[i] != '\0'; i++)
    {
        char c = id[i];
        if (i >= PSD_SCRIPT_ID_MAX
            || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
        {
            return 0;
        }
    }
    return 1;
}

/* The value of `key=` among space-separated words in `s` (the rest of the
 * line for `label`), copied into out; 1 if found. */
static int script_header_word(const char *s, const char *key, int rest,
                              char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = s;

    while (*p != '\0')
    {
        while (*p == ' ')
        {
            p++;
        }
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            const char *v = p + klen + 1;
            size_t      n = rest ? strlen(v) : strcspn(v, " ");
            if (n >= cap)
            {
                n = cap - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == ' '))
            {
                out[--n] = '\0';
            }
            return 1;
        }
        p += strcspn(p, " ");
    }
    return 0;
}

int psd_script_parse_header(const char *buf, psd_script_dim_t *dims, int max,
                            char *applied, size_t applied_cap)
{
    const char *line = buf;
    int         n = 0;

    if (applied != NULL && applied_cap > 0)
    {
        applied[0] = '\0';
    }
    while (line != NULL && *line != '\0')
    {
        const char *nl  = strchr(line, '\n');
        size_t      len = (nl != NULL) ? (size_t) (nl - line) : strlen(line);
        char        l[SCRIPT_LINE_MAX];

        if (line[0] != '#')
        {
            break;              /* the header is the leading comment block */
        }
        if (len < sizeof(l))
        {
            char id[64];
            char roles[64];

            memcpy(l, line, len);
            l[len] = '\0';
            if (strncmp(l, "# dim ", 6) == 0 && dims != NULL && n < max
                && script_header_word(l + 6, "id", 0, id, sizeof(id))
                && psd_script_id_ok(id))
            {
                psd_script_dim_t *d = &dims[n];
                char kind[16] = "";
                memset(d, 0, sizeof(*d));
                snprintf(d->id, sizeof(d->id), "%s", id);
                (void) script_header_word(l + 6, "kind", 0, kind,
                                          sizeof(kind));
                roles[0] = '\0';
                (void) script_header_word(l + 6, "roles", 0, roles,
                                          sizeof(roles));
                d->numeric   = strcmp(kind, "numeric") == 0;
                d->sortable  = strstr(roles, "sort") != NULL && d->numeric;
                d->groupable = strstr(roles, "group") != NULL;
                if (!script_header_word(l + 6, "label", 1, d->label,
                                        sizeof(d->label)) || !d->label[0])
                {
                    snprintf(d->label, sizeof(d->label), "%s", d->id);
                }
                n++;
            }
            else if (strncmp(l, "# sort ", 7) == 0 && applied != NULL
                     && script_header_word(l + 7, "id", 0, id, sizeof(id))
                     && psd_script_id_ok(id))
            {
                snprintf(applied, applied_cap, "%s", id);
            }
        }
        line = (nl != NULL) ? nl + 1 : NULL;
    }
    return n;
}

void psd_script_parse_effects(const char *buf, char *post, char *del,
                              size_t cap)
{
    const char *line = buf;

    if (post == NULL || del == NULL || cap == 0)
    {
        return;
    }
    post[0] = '\0';
    del[0]  = '\0';
    while (line != NULL && *line == '#')
    {
        const char *nl  = strchr(line, '\n');
        size_t      len = (nl != NULL) ? (size_t) (nl - line) : strlen(line);
        char        l[SCRIPT_LINE_MAX];
        char        verb[16];

        if (len < sizeof(l))
        {
            memcpy(l, line, len);
            l[len] = '\0';
            if (strncmp(l, "# effect ", 9) == 0
                && script_header_word(l + 9, "verb", 0, verb, sizeof(verb)))
            {
                char *dst = strcmp(verb, "post") == 0   ? post
                          : strcmp(verb, "delete") == 0 ? del
                                                        : NULL;
                size_t i;

                if (dst != NULL
                    && script_header_word(l + 9, "text", 1, dst, cap))
                {
                    for (i = 0; dst[i] != '\0'; i++)
                    {
                        if ((unsigned char) dst[i] < 0x20
                            || (unsigned char) dst[i] > 0x7e)
                        {
                            dst[i] = ' ';
                        }
                    }
                }
            }
        }
        line = (nl != NULL) ? nl + 1 : NULL;
    }
}

const char *proc_script_effect(int kill)
{
    const char *s = kill ? script_effect_del : script_effect_post;

    return s[0] != '\0' ? s : NULL;
}

int psd_script_poll_command(const char *base, const char *sort, int limit,
                            char *out, size_t cap)
{
    char words[64] = "";
    int  w = 0;

    if (sort != NULL && sort[0] != '\0' && psd_script_id_ok(sort))
    {
        w += snprintf(words + w, sizeof(words) - (size_t) w, " sort=%s", sort);
    }
    if (limit > 0 && limit <= 4096)
    {
        w += snprintf(words + w, sizeof(words) - (size_t) w, " limit=%d",
                      limit);
    }
    if (snprintf(out, cap, "%s%s", base, words) >= (int) cap)
    {
        snprintf(out, cap, "%s", base);   /* too long: no request at all */
        return 0;
    }
    return 1;
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
 * game's files, sockets and WAD never reach a script), and execve `path`
 * with `envp`. The game is multi-threaded (the poll worker), so a forked
 * child may only make async-signal-safe calls: the parent resolves the path
 * (script_resolve_exec), builds argv, takes the environment and the close
 * limit before fork, and the child makes only sigprocmask, setpgid, dup2,
 * fcntl, close, execve and _exit calls -- no malloc, lock, stdio or PATH
 * search. Never returns.
 */
static void script_child_exec(int stdin_fd, int stdout_fd, int fd_limit,
                              const char *path, char *const argv[],
                              char *const envp[], const sigset_t *mask)
{
    int fd;

    /* The poll worker runs with every signal blocked, and a mask survives
     * exec: start the command with none blocked, so it (and any helper it
     * leaves running) still dies of SIGTERM, SIGINT or SIGPIPE. */
    (void) sigprocmask(SIG_SETMASK, mask, NULL);
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
    (void) execve(path, argv, envp);
    _exit(127);
}

/*
 * Before fork: the executable `name` names, as exec would find it -- `name`
 * itself if it holds a slash, else the first executable regular file
 * <dir>/<name> along $PATH ("/usr/bin:/bin" if unset; an empty entry is the
 * current directory). Returns 0 if there is none or it does not fit.
 */
static int script_resolve_exec(const char *name, char *out, size_t cap)
{
    const char *path;
    const char *p;

    if (name == NULL || name[0] == '\0')
    {
        return 0;
    }
    if (strchr(name, '/') != NULL)
    {
        return snprintf(out, cap, "%s", name) < (int) cap;
    }
    path = getenv("PATH");
    if (path == NULL)
    {
        path = "/usr/bin:/bin";
    }
    for (p = path; ; )
    {
        size_t      len = strcspn(p, ":");
        struct stat st;

        if (snprintf(out, cap, "%.*s%s%s", (int) len, len ? p : "",
                     len ? "/" : "", name) < (int) cap
            && stat(out, &st) == 0 && S_ISREG(st.st_mode)
            && access(out, X_OK) == 0)
        {
            return 1;
        }
        if (p[len] == '\0')
        {
            break;
        }
        p += len + 1;
    }
    return 0;
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

/* Stop poll child `pid` as a group and reap it, unless it was already
 * reaped (it exited, or the at-exit teardown stopped it). */
static void script_stop_child(pid_t pid)
{
    pthread_mutex_lock(&script_child_mu);
    if (script_child == pid)
    {
        script_stop_group(pid);
        script_child = 0;
    }
    pthread_mutex_unlock(&script_child_mu);
}

/* At exit: a poll still running would outlive the game (the worker thread
 * ends with the process), so stop its group as a timeout would. */
static void script_atexit_teardown(void)
{
    pthread_mutex_lock(&script_child_mu);
    if (script_child > 1)
    {
        script_stop_group(script_child);
    }
    script_child = 0;
    script_child_closed = 1;
    pthread_mutex_unlock(&script_child_mu);
}

/*
 * Run `cmd` via the shell, in a process group of its own, and capture up to
 * `cap`-1 bytes of its stdout into `buf` (always NUL-terminated). If it
 * overruns `timeout_ms` (still writing, or holding stdout open, or not
 * exiting after EOF), or fills the buffer, the whole group is killed and its
 * leader reaped, so no part of the command outlives the poll. Returns bytes
 * captured (>= 0), or -1 if the child could not be spawned. Sets *cut to 1
 * if the capture was cut off (timeout, full buffer, error), and *exit_status
 * to the command's exit status when it exited on its own (0 otherwise).
 * Blocks for up to `timeout_ms`: it runs on the poll worker thread.
 */
static int script_run_capture(const char *cmd, char *buf, int cap, int timeout_ms,
                              int *cut, int *exit_status)
{
    int                fds[2];
    pid_t              pid;
    int                total = 0;
    int                stop  = 0;
    int                fd_limit;
    int                devnull;
    char              *argv[4];
    char             **envp = environ;   /* taken before fork */
    unsigned long long deadline;
    sigset_t           nomask;

    *cut         = 0;
    *exit_status = 0;
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
    sigemptyset(&nomask);

    /* Held across fork, so the child is recorded before anything (the
     * at-exit teardown) can look for it. The forked child never uses it. */
    pthread_mutex_lock(&script_child_mu);
    pid = script_child_closed ? -1 : fork();   /* exiting: start nothing */
    if (pid > 0)
    {
        script_child = pid;
    }
    if (pid != 0)
    {
        pthread_mutex_unlock(&script_child_mu);
    }
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
        script_child_exec(devnull, fds[1], fd_limit, "/bin/sh", argv, envp, &nomask);
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
        int   st = 0;
        pid_t w;

        pthread_mutex_lock(&script_child_mu);
        if (script_child != pid)
        {
            /* The at-exit teardown already stopped and reaped it. */
            pthread_mutex_unlock(&script_child_mu);
            *cut = 1;
            return total;
        }
        w = waitpid(pid, &st, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR))
        {
            script_child = 0;
            pthread_mutex_unlock(&script_child_mu);
            if (w == pid && WIFEXITED(st))
            {
                *exit_status = WEXITSTATUS(st);
            }
            return total;         /* exited and reaped */
        }
        pthread_mutex_unlock(&script_child_mu);
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

    *cut = 1;
    script_stop_child(pid);
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
    char  modebuf[32];
    char *argv[SCRIPT_ARGV_MAX];
    int   n;
    int   fd_limit;
    int      devnull;
    pid_t    pid;
    sigset_t nomask;
    char     exe[PATH_MAX];
    char   **envp = environ;   /* taken before fork */

    if (id <= 1)
    {
        return 0;   /* 0, 1 and negatives are reserved: never a target */
    }
    if (script_respond_cmd[0] == '\0' && script_resp_nfixed == 0)
    {
        return 1;
    }

    /* Build argv before fork (snprintf is not async-signal-safe): the
     * command and its static args (a fixed argv, or the configured string
     * split on whitespace), then the psDoom-supplied arguments. */
    if (script_resp_nfixed > 0)
    {
        for (n = 0; n < script_resp_nfixed; n++)
        {
            argv[n] = script_resp_fixed[n];
        }
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "%s", script_respond_cmd);
        n = script_split_cmd(cmd, argv, SCRIPT_ARGV_MAX - 7);
    }
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
    snprintf(modebuf, sizeof(modebuf), "mode=%s", script_mode);
    argv[n++] = modebuf;
    argv[n]  = NULL;
    if (!script_resolve_exec(argv[0], exe, sizeof(exe)))
    {
        return 1;   /* nothing to run: as a failed exec, nothing happens */
    }
    devnull  = script_open_devnull();
    fd_limit = script_fd_limit();
    sigemptyset(&nomask);

    pid = fork();
    if (pid == 0)
    {
        /* intermediate child: fork again so the grandchild is reparented to
         * launchd (auto-reaped) and we never wait on the actual command. */
        if (fork() == 0)
        {
            script_child_exec(devnull, -1, fd_limit, exe, argv, envp, &nomask);
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

/* The poll worker: wait for a job, run it, hand the result back. It touches
 * nothing but the job. */
static void *script_worker_main(void *arg)
{
    (void) arg;
    for (;;)
    {
        char cmd[sizeof(script_job_cmd)];
        int  timeout_ms;
        int  rc;
        int  cut;
        int  exit_status;

        pthread_mutex_lock(&script_job_mu);
        while (script_job_state != SCRIPT_JOB_RUNNING)
        {
            pthread_cond_wait(&script_job_cv, &script_job_mu);
        }
        memcpy(cmd, script_job_cmd, sizeof(cmd));
        timeout_ms = script_job_timeout_ms;
        pthread_mutex_unlock(&script_job_mu);

        rc = script_run_capture(cmd, script_job_buf, (int) sizeof(script_job_buf),
                                timeout_ms, &cut, &exit_status);

        pthread_mutex_lock(&script_job_mu);
        script_job_rc    = rc;
        script_job_cut   = cut;
        script_job_exit  = exit_status;
        script_job_done_ms = script_mono_ms();
        script_job_state = SCRIPT_JOB_DONE;
        pthread_mutex_unlock(&script_job_mu);
    }
    return NULL;
}

/* Start the worker once. It blocks every signal, so the game's handlers
 * keep running on the game thread. */
static void script_worker_start(void)
{
    pthread_attr_t attr;
    pthread_t      thread;
    sigset_t       all;
    sigset_t       saved;

    if (script_worker != 0)
    {
        return;
    }
    script_worker = -1;
    if (pthread_attr_init(&attr) != 0)
    {
        return;
    }
    (void) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    sigfillset(&all);
    (void) pthread_sigmask(SIG_SETMASK, &all, &saved);
    if (pthread_create(&thread, &attr, script_worker_main, NULL) == 0)
    {
        script_worker = 1;
        (void) atexit(script_atexit_teardown);
    }
    (void) pthread_sigmask(SIG_SETMASK, &saved, NULL);
    (void) pthread_attr_destroy(&attr);
    if (script_worker != 1)
    {
        fprintf(stderr, "psDoom: no poll thread; polling on the game thread\n");
    }
}

/* Turn a finished poll (its output in buf, its capture result) into the
 * entity list, the status and the dimensions. Game thread only. */
static int script_consume(int rc, int cut, int exit_status, const char *buf,
                          psd_proc_t *out, int max)
{
    unsigned int uid = (unsigned int) getuid();
    int          n;
    int          i;

    if (rc >= 0 && cut && strchr(buf, '\n') == NULL && script_have_list
        && ++script_timeouts < SCRIPT_TIMEOUT_GRACE)
    {
        /* Slow, not gone: keep the last list (and its labels) and poll
         * again. Every process monster stays; none is read as exited. */
        if (script_timeouts == 1)
        {
            fprintf(stderr, "psDoom: the process list was slow; keeping the "
                    "last one (poll: %s)\n", script_poll_cmd);
        }
        return -1;
    }
    if (rc < 0 || (cut && strchr(buf, '\n') == NULL))
    {
        /* No process monsters until a poll works again; say why once. */
        snprintf(script_status, sizeof(script_status),
                 "No process monsters: the process list did not answer");
        if (!script_poll_failing)
        {
            fprintf(stderr, "psDoom: %s (poll: %s)\n", script_status,
                    script_poll_cmd);
        }
        script_poll_failing = 1;
        script_last_n = 0;   /* state unknown: nothing counts as live */
        return 0;
    }

    n = psd_script_parse(buf, out, max);
    /* A poll that lists nothing because it failed -- it exited non-zero, or
     * Summoner reported an error -- is a failure to report, not an empty
     * list. */
    if (n == 0 && (exit_status != 0
                   || strncmp(buf, "# summon error", 14) == 0))
    {
        snprintf(script_status, sizeof(script_status),
                 "No process monsters: the process list failed (see the log)");
        if (!script_poll_failing)
        {
            fprintf(stderr, "psDoom: %s (exit %d; %.200s)\n", script_status,
                    exit_status, buf);
        }
        script_poll_failing = 1;
        script_last_n = 0;
        return 0;
    }
    script_have_list = 1;
    script_timeouts  = 0;
    if (script_poll_failing)
    {
        fprintf(stderr, "psDoom: Summoner answers again\n");
        script_poll_failing = 0;
        script_status[0]    = '\0';
    }
    script_ndims = psd_script_parse_header(buf, script_dims,
                                           PSD_SCRIPT_DIMS_MAX, script_applied,
                                           sizeof(script_applied));
    psd_script_parse_effects(buf, script_effect_post, script_effect_del,
                             sizeof(script_effect_post));

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

/* The poll command with the host's request as it stands now. */
static void script_request_command(char *cmd, size_t cap)
{
    /* A request rides on the poll only once the backend has declared
     * dimensions: a plain script never gets words it did not ask for. */
    if (script_ndims > 0)
    {
        int sortable = 0;
        int d;
        for (d = 0; d < script_ndims; d++)
        {
            if (script_dims[d].sortable
                && strcmp(script_dims[d].id, script_req_sort) == 0)
            {
                sortable = 1;
            }
        }
        (void) psd_script_poll_command(script_poll_cmd,
                                       sortable ? script_req_sort : NULL,
                                       script_req_limit, cmd, cap);
    }
    else
    {
        snprintf(cmd, cap, "%s", script_poll_cmd);
    }
}

/* Hand the poll command to the worker (the job must be idle). */
static void script_job_submit(const char *cmd)
{
    pthread_mutex_lock(&script_job_mu);
    snprintf(script_job_cmd, sizeof(script_job_cmd), "%s", cmd);
    script_job_timeout_ms = script_timeout_ms;
    script_job_gen        = script_gen;
    script_job_state      = SCRIPT_JOB_RUNNING;
    pthread_cond_signal(&script_job_cv);
    pthread_mutex_unlock(&script_job_mu);
}

/*
 * Never waits for a poll. If the poll this backend started has finished,
 * consume it and return its list (>= 0). Otherwise return -1 ("not yet"):
 * either a poll is still running, or none was, and this call started one,
 * with the request (Sort by, limit) as it stands now. The caller keeps what
 * it has and calls again on a later tic.
 */
static int script_list(psd_proc_t *out, int max)
{
    char cmd[SCRIPT_CMD_MAX + 64];
    int  state;
    int  rc;
    int  cut;
    int  exit_status;
    int  current;

    pthread_mutex_lock(&script_job_mu);
    state       = script_job_state;
    rc          = script_job_rc;
    cut         = script_job_cut;
    exit_status = script_job_exit;
    current     = script_job_gen == script_gen;
    /* A result that waited longer than a poll may take plus a second (the
     * game was paused, or between levels) describes the past: poll again. */
    if (state == SCRIPT_JOB_DONE
        && script_mono_ms() - script_job_done_ms
           > (unsigned long long) script_timeout_ms + SCRIPT_RESULT_MAX_WAIT_MS)
    {
        current = 0;
    }
    pthread_mutex_unlock(&script_job_mu);

    if (state == SCRIPT_JOB_RUNNING)
    {
        return -1;
    }
    if (state == SCRIPT_JOB_DONE)
    {
        /* The worker is idle until the next job: script_job_buf is ours. */
        int n = current ? script_consume(rc, cut, exit_status, script_job_buf,
                                         out, max)
                        : -1;   /* an earlier install's, or stale: dropped */

        pthread_mutex_lock(&script_job_mu);
        script_job_state = SCRIPT_JOB_IDLE;
        pthread_mutex_unlock(&script_job_mu);
        if (n >= 0)
        {
            return n;
        }
    }

    script_request_command(cmd, sizeof(cmd));
    script_worker_start();
    if (script_worker != 1)
    {
        /* No thread could be made: poll here, blocking, as before. */
        rc = script_run_capture(cmd, script_job_buf, (int) sizeof(script_job_buf),
                                script_timeout_ms, &cut, &exit_status);
        return script_consume(rc, cut, exit_status, script_job_buf, out, max);
    }

    script_job_submit(cmd);
    return -1;
}

void proc_script_discard(void)
{
    script_gen++;   /* the result in flight is now an earlier one's: dropped */
}

void proc_script_prefetch(void)
{
    char cmd[SCRIPT_CMD_MAX + 64];
    int  idle;

    if (!proc_script_active())
    {
        return;
    }
    script_worker_start();
    if (script_worker != 1)
    {
        return;   /* polls run inside list() then: nothing to start early */
    }
    pthread_mutex_lock(&script_job_mu);
    if (script_job_state == SCRIPT_JOB_DONE && script_job_gen != script_gen)
    {
        script_job_state = SCRIPT_JOB_IDLE;   /* an earlier install's poll */
    }
    idle = script_job_state == SCRIPT_JOB_IDLE;
    pthread_mutex_unlock(&script_job_mu);
    if (idle)
    {
        script_request_command(cmd, sizeof(cmd));
        script_job_submit(cmd);
    }
}

static unsigned int script_current_uid(void)
{
    return (unsigned int) getuid();
}

static void script_renice(int pid, int nice_delta)
{
    (void) script_run_respond("post", pid, 1, nice_delta, script_label_for(pid));
}

static int script_kill(int pid)
{
    /* Fire-and-forget: we can't synchronously know the script's exit, so report
     * "dispatched". A still-present entity simply reappears on the next poll.
     * 0 only when refused (an id <= 1). */
    return script_run_respond("delete", pid, 0, 0, script_label_for(pid));
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
    1,               /* preserves_order: the plugin curates */
};

/* ----------------------------------------------------------------- install */

void proc_script_install(const char *poll_cmd, const char *respond_cmd,
                         int timeout_ms)
{
    snprintf(script_poll_cmd, sizeof(script_poll_cmd), "%s",
             poll_cmd != NULL ? poll_cmd : "");
    snprintf(script_respond_cmd, sizeof(script_respond_cmd), "%s",
             respond_cmd != NULL ? respond_cmd : "");
    script_resp_nfixed  = 0;
    script_status[0]    = '\0';
    script_poll_failing = 0;
    script_timeout_ms = (timeout_ms > 0) ? timeout_ms : 1000;
    script_have_list  = 0;
    script_timeouts   = 0;
    script_last_n     = 0;
    script_ndims      = 0;
    script_applied[0] = '\0';
    script_effect_post[0] = '\0';
    script_effect_del[0]  = '\0';
    script_req_sort[0] = '\0';
    script_req_limit  = 0;
    snprintf(script_mode, sizeof(script_mode), "dryrun");
    script_gen++;   /* a poll still running is the old configuration's */

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
        return 0;   /* not configured: the bundled Summoner is the default */
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

/* ------------------------------------------------------- sort request (host) */

int proc_script_active(void)
{
    return proc_backend() == &script_backend;
}

void proc_script_set_limit(int limit)
{
    script_req_limit = limit;
}

/* The sortable dimension the next poll asks for: the player's choice if the
 * backend still offers it, else what the last poll applied. */
static int script_current_sort_index(void)
{
    const char *want = script_req_sort[0] ? script_req_sort : script_applied;
    int         i;

    for (i = 0; i < script_ndims; i++)
    {
        if (script_dims[i].sortable && strcmp(script_dims[i].id, want) == 0)
        {
            return i;
        }
    }
    return -1;
}

int proc_script_sort_count(void)
{
    int i, n = 0;

    for (i = 0; i < script_ndims; i++)
    {
        n += script_dims[i].sortable;
    }
    return n;
}

const char *proc_script_sort_label(void)
{
    int i = script_current_sort_index();
    return (i >= 0) ? script_dims[i].label : NULL;
}

void proc_script_cycle_sort(int dir)
{
    int start = script_current_sort_index();
    int i;
    int k;

    if (proc_script_sort_count() == 0)
    {
        return;
    }
    if (start < 0)
    {
        start = (dir >= 0) ? script_ndims - 1 : 0;
    }
    for (k = 1; k <= script_ndims; k++)
    {
        i = (start + (dir >= 0 ? k : -k) + script_ndims * k) % script_ndims;
        if (script_dims[i].sortable)
        {
            snprintf(script_req_sort, sizeof(script_req_sort), "%s",
                     script_dims[i].id);
            return;
        }
    }
}

int proc_script_set_mode(const char *mode)
{
    if (mode == NULL
        || (strcmp(mode, "dryrun") != 0 && strcmp(mode, "post-only") != 0
            && strcmp(mode, "delete") != 0))
    {
        snprintf(script_mode, sizeof(script_mode), "dryrun");
        return 0;
    }
    snprintf(script_mode, sizeof(script_mode), "%s", mode);
    return 1;
}

/* ------------------------------------------------- the bundled Summoner */

int psd_sh_quote(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    size_t i;

    if (cap < 3)
    {
        return 0;
    }
    out[o++] = '\'';
    for (i = 0; in[i] != '\0'; i++)
    {
        if (in[i] == '\'')
        {
            if (o + 4 >= cap)
            {
                return 0;
            }
            memcpy(out + o, "'\\''", 4);
            o += 4;
        }
        else
        {
            if (o + 1 >= cap)
            {
                return 0;
            }
            out[o++] = in[i];
        }
    }
    if (o + 2 > cap)
    {
        return 0;
    }
    out[o++] = '\'';
    out[o] = '\0';
    return 1;
}

int psd_summon_bundle_path(const char *exe, char *out, size_t cap)
{
    const char *slash;
    size_t      dirlen;

    if (exe == NULL || exe[0] != '/' || (slash = strrchr(exe, '/')) == NULL)
    {
        return 0;
    }
    dirlen = (size_t) (slash - exe);
    /* <bundle>/Contents/MacOS/<exe> -> <bundle>/Contents/Resources/... */
    if (dirlen < 6 || strncmp(slash - 6, "/MacOS", 6) != 0)
    {
        return 0;
    }
    return snprintf(out, cap, "%.*s/Resources/summoner/summon",
                    (int) (dirlen - 6), exe) < (int) cap;
}

/* Interpreters tried, in order. /usr/bin/python3 is never used: on a Mac
 * without the Command Line Tools it is a shim that pops an install dialog
 * (every poll would raise it). The real python3 the Command Line Tools or
 * Xcode install is used directly instead. */
static const char *const psd_python_candidates[] =
{
    "/opt/homebrew/bin/python3",
    "/usr/local/bin/python3",
    "/Library/Developer/CommandLineTools/usr/bin/python3",
    "/Applications/Xcode.app/Contents/Developer/usr/bin/python3",
    NULL,
};

int psd_python_choose(int (*usable)(const char *path), char *out, size_t cap)
{
    int i;

    for (i = 0; psd_python_candidates[i] != NULL; i++)
    {
        if (usable(psd_python_candidates[i]))
        {
            return snprintf(out, cap, "%s", psd_python_candidates[i])
                   < (int) cap;
        }
    }
    return 0;
}

static int script_executable(const char *path)
{
    return access(path, X_OK) == 0;
}

static int script_readable(const char *path)
{
    return access(path, R_OK) == 0;
}

int proc_script_install_bundled(const char *exe_path)
{
    char summon[SCRIPT_CMD_MAX] = "";
    char python[SCRIPT_CMD_MAX] = "";
    char qpy[SCRIPT_CMD_MAX];
    char qsummon[SCRIPT_CMD_MAX];
    char poll[SCRIPT_CMD_MAX];

    proc_backend_set(&proc_none_backend);
    if (!psd_summon_bundle_path(exe_path, summon, sizeof(summon))
        || !script_readable(summon))
    {
        snprintf(script_status, sizeof(script_status),
                 "No process monsters: part of the app is missing");
        fprintf(stderr, "psDoom: %s (looked for %s)\n", script_status,
                exe_path != NULL ? summon : "(no executable path)");
        return 0;
    }
    if (!psd_python_choose(script_executable, python, sizeof(python)))
    {
        snprintf(script_status, sizeof(script_status),
                 "No process monsters: python3 is needed");
        fprintf(stderr, "psDoom: %s (install Homebrew python3, or the "
                "Command Line Tools: xcode-select --install)\n",
                script_status);
        return 0;
    }
    if (!psd_sh_quote(python, qpy, sizeof(qpy))
        || !psd_sh_quote(summon, qsummon, sizeof(qsummon))
        /* -I: no PYTHONPATH or user site from the environment; -B: never
         * write __pycache__ into the (signed) app bundle. */
        || snprintf(poll, sizeof(poll), "%s -I -B %s get", qpy, qsummon)
           >= (int) sizeof(poll))
    {
        snprintf(script_status, sizeof(script_status),
                 "No process monsters: the app path is too long");
        fprintf(stderr, "psDoom: %s\n", script_status);
        return 0;
    }
    proc_script_install(poll, NULL, 0);
    snprintf(script_resp_fixed[0], sizeof(script_resp_fixed[0]), "%s", python);
    snprintf(script_resp_fixed[1], sizeof(script_resp_fixed[1]), "-I");
    snprintf(script_resp_fixed[2], sizeof(script_resp_fixed[2]), "-B");
    snprintf(script_resp_fixed[3], sizeof(script_resp_fixed[3]), "%s", summon);
    script_resp_nfixed = 4;
    fprintf(stderr, "psDoom: Summoner active (%s %s)\n", python, summon);
    return 1;
}

const char *proc_script_status(void)
{
    return script_status[0] ? script_status : NULL;
}
