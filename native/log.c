//-------------------------------------------------------------------
// The card log - see log.h.
//-------------------------------------------------------------------

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "log.h"
#include "pipeline.h"

#ifndef _WIN32
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#endif

static char log_path[256];

unsigned long rw_ms(void)
{
#ifndef _WIN32
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (unsigned long)tv.tv_sec * 1000UL + (unsigned long)tv.tv_usec / 1000UL;
#else
    return (unsigned long)(clock() * 1000.0 / CLOCKS_PER_SEC);
#endif
}

void rw_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    if (!log_path[0] || !(f = fopen(log_path, "a"))) return;
    fprintf(f, "%8lu  ", rw_ms() % 100000000UL);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void rw_log_mem(const char *when)
{
#ifndef _WIN32
    char line[128], out[192];
    unsigned long free_kb = 0, cached_kb = 0, rss_kb = 0, v;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f)
    {
        while (fgets(line, sizeof(line), f))
        {
            if (sscanf(line, "MemFree: %lu", &v) == 1) free_kb = v;
            else if (sscanf(line, "Cached: %lu", &v) == 1) cached_kb = v;
        }
        fclose(f);
    }
    f = fopen("/proc/self/status", "r");
    if (f)
    {
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "VmRSS: %lu", &v) == 1) rss_kb = v;
        fclose(f);
    }
    snprintf(out, sizeof(out), "mem %s: free %lu KB, cached %lu KB, this app %lu KB", when, free_kb, cached_kb, rss_kb);
    rw_log("%s", out);
#else
    rw_log("mem %s: (not measured on this host)", when);
#endif
}

//-------------------------------------------------------------------
// The fault handler. Only async-signal-safe calls: open, write, close.

#ifndef _WIN32
static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
static struct sigaction old_act[5];

static int put_num(char *p, unsigned long v)
{
    char t[16];
    int n = 0, i;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 15);
    for (i = 0; i < n; i++) p[i] = t[n - 1 - i];
    return n;
}

static void on_fault(int sig)
{
    static const char a[] = "NATIVE CRASH signal ";
    static const char b[] = " during stage ";
    static const char c[] = " at progress ";
    char buf[128];
    int n = 0, i, fd;

    memcpy(buf + n, a, sizeof(a) - 1); n += sizeof(a) - 1;
    n += put_num(buf + n, (unsigned long)sig);
    memcpy(buf + n, b, sizeof(b) - 1); n += sizeof(b) - 1;
    n += put_num(buf + n, (unsigned long)rw_stage);
    memcpy(buf + n, c, sizeof(c) - 1); n += sizeof(c) - 1;
    n += put_num(buf + n, (unsigned long)rw_progress);
    buf[n++] = '\n';

    fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd >= 0) { write(fd, buf, n); close(fd); }

    // Back to the previous handler; returning re-runs the faulting
    // instruction, which now goes to it (abort is raised again instead).
    for (i = 0; i < 5; i++)
        if (sigs[i] == sig) sigaction(sig, &old_act[i], 0);
    if (sig == SIGABRT) raise(sig);
}
#endif

void rw_log_open(const char *path)
{
#ifndef _WIN32
    static int installed;
#endif
    if (!path) { log_path[0] = 0; return; }
    strncpy(log_path, path, sizeof(log_path) - 1);
    log_path[sizeof(log_path) - 1] = 0;
#ifndef _WIN32
    if (!installed)
    {
        struct sigaction sa;
        int i;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_fault;
        sigemptyset(&sa.sa_mask);
        for (i = 0; i < 5; i++) sigaction(sigs[i], &sa, &old_act[i]);
        installed = 1;
    }
#endif
}
