/* Progress reporting helpers (written to stderr). */
#include "pf.h"

#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static long long now_ms(void)
{
    return (long long)GetTickCount64();
}
#else
#  include <unistd.h>
#  include <time.h>
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

int pf_quiet = 0;

/* Small cache so repeated lookups of the same file avoid a reopen. */
#define PFS_CACHE 4
static struct { char path[1024]; long long sz; } pfs_cache[PFS_CACHE];
static int pfs_next = 0;

long long pf_file_size(const char *path)
{
    for (int i = 0; i < PFS_CACHE; i++)
        if (pfs_cache[i].path[0] && strcmp(pfs_cache[i].path, path) == 0)
            return pfs_cache[i].sz;

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long long n = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long long n = (long long)ftello(f);
#endif
    fclose(f);
    if (n < 0)
        return -1;

    if (strlen(path) < sizeof(pfs_cache[0].path)) {
        int slot = pfs_next++ % PFS_CACHE;
        snprintf(pfs_cache[slot].path, sizeof(pfs_cache[slot].path), "%s", path);
        pfs_cache[slot].sz = n;
    }
    return n;
}

static void human(long long bytes, char *out, size_t n)
{
    static const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    snprintf(out, n, "%.1f %s", v, u[i]);
}

void pf_progress_start(PfProgress *p, const char *label, long long total)
{
    memset(p, 0, sizeof(*p));
    p->label = label;
    p->total = total;
    if (pf_quiet || total <= 0)
        return;
    p->active = 1;
    p->start_ms = p->last_ms = now_ms();
    fprintf(stderr, "%s: 0%%", label);
    fflush(stderr);
}

void pf_progress_tick(PfProgress *p, long long done)
{
    if (!p->active)
        return;
    long long t = now_ms();
    if (t - p->last_ms < 120)
        return;
    p->last_ms = t;

    if (done > p->total) done = p->total;
    if (done < 0) done = 0;
    int pct = (int)(done * 100 / p->total);
    if (pct > 100) pct = 100;

    const int barw = 24;
    char bar[32];
    int fill = pct * barw / 100;
    for (int i = 0; i < barw; i++) bar[i] = (i < fill) ? '#' : '-';
    bar[barw] = '\0';

    char cur[32], tot[32], rate[32];
    human(done, cur, sizeof(cur));
    human(p->total, tot, sizeof(tot));
    double dt = (t - p->start_ms) / 1000.0;
    human((long long)(dt > 0.0 ? (double)done / dt : 0.0), rate, sizeof(rate));

    fprintf(stderr, "\r%s: %3d%% [%s] %s / %s  %s/s   ",
            p->label, pct, bar, cur, tot, rate);
    fflush(stderr);
}

void pf_progress_end(PfProgress *p)
{
    if (!p->active)
        return;
    long long t = now_ms();
    double dt = (t - p->start_ms) / 1000.0;
    char tot[32];
    human(p->total, tot, sizeof(tot));
    fprintf(stderr, "\r%s: 100%% [########################] %s in %.1fs\n",
            p->label, tot, dt);
    fflush(stderr);
    p->active = 0;
}
