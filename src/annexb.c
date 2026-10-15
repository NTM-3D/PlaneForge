/* Streaming Annex-B reader and RBSP/EBSP helpers. */
#include "pf.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── diagnostics ───────────────────────────────────────────────────── */
void pf_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(PF_NAME ": error: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

void pf_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(PF_NAME ": warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ── start-code scanning ───────────────────────────────────────────── */
/*
 * Return the index of the next Annex-B start code at or after `pos`, and
 * set *sclen to 3 or 4.  A "00 00 01" that is immediately preceded by a
 * zero byte belongs to a 4-byte "00 00 00 01" start code instead.
 * Returns (size_t)-1 when none is found.
 */
static size_t find_sc(const uint8_t *b, size_t pos, size_t n, int *sclen)
{
    size_t i = pos;
    while (i + 2 < n) {
        const uint8_t *z = memchr(b + i, 0, n - 2 - i);
        if (!z)
            return (size_t)-1;
        size_t zp = (size_t)(z - b);
        if (b[zp + 1] == 0 && b[zp + 2] == 1) {
            if (zp > 0 && b[zp - 1] == 0) {
                if (zp - 1 >= pos) { *sclen = 4; return zp - 1; }
                i = zp + 1;
                continue;
            }
            *sclen = 3;
            return zp;
        }
        i = zp + 1;
    }
    return (size_t)-1;
}

/* ── buffered filling ──────────────────────────────────────────────── */
static void reader_fill(PfReader *r)
{
    if (r->eof)
        return;
    /* Reclaim the consumed prefix so reads append to the tail.  This runs
     * once per refill rather than once per NAL. */
    if (r->pos > 0) {
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->len -= r->pos;
        r->pos = 0;
    }
    if (r->len == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : (1u << 20);
        uint8_t *nb = (uint8_t *)realloc(r->buf, ncap);
        if (!nb)
            pf_fatal("out of memory");
        r->buf = nb;
        r->cap = ncap;
    }
    size_t got = fread(r->buf + r->len, 1, r->cap - r->len, r->fp);
    if (got == 0) {
        r->eof = 1;
        return;
    }
    r->len += got;
    r->bytes_read += (long long)got;
}

int pf_reader_init(PfReader *r, FILE *fp)
{
    memset(r, 0, sizeof(*r));
    r->fp = fp;
    return 0;
}

void pf_reader_free(PfReader *r)
{
    free(r->buf);
    r->buf = NULL;
}

/*
 * Fetch the next entry.
 *   PF_NAL  - r->gap/sc/nal describe one NAL unit.
 *   PF_TAIL - trailing bytes with no following start code (in r->gap).
 *   PF_EOF  - nothing left.
 */
int pf_reader_next(PfReader *r)
{
    for (;;) {
        int sclen = 0;
        size_t sc = find_sc(r->buf, r->pos, r->len, &sclen);

        if (sc == (size_t)-1) {
            if (!r->eof) { reader_fill(r); continue; }
            if (r->pos < r->len) {
                r->gap = r->buf + r->pos;
                r->gap_len = r->len - r->pos;
                r->pos = r->len;
                return PF_TAIL;
            }
            return PF_EOF;
        }

        size_t hdr = sc + (size_t)sclen;
        if (hdr >= r->len) {                 /* need the header byte */
            if (!r->eof) { reader_fill(r); continue; }
            /* dangling start code at EOF: treat as tail */
            r->gap = r->buf + r->pos;
            r->gap_len = r->len - r->pos;
            r->pos = r->len;
            return PF_TAIL;
        }

        int nsclen = 0;
        size_t nxt = find_sc(r->buf, hdr, r->len, &nsclen);
        if (nxt == (size_t)-1 && !r->eof) {
            reader_fill(r);
            continue;
        }

        r->gap     = r->buf + r->pos;
        r->gap_len = sc - r->pos;
        r->sc      = r->buf + sc;
        r->sc_len  = (size_t)sclen;
        r->nal     = r->buf + hdr;
        r->nal_len = ((nxt == (size_t)-1) ? r->len : nxt) - hdr;
        r->nal_type = r->nal_len ? (r->nal[0] & 0x1f) : -1;
        r->pos = (nxt == (size_t)-1) ? r->len : nxt;
        return PF_NAL;
    }
}

/* ── RBSP / EBSP ───────────────────────────────────────────────────── */
uint8_t *ebsp_to_rbsp(const uint8_t *e, size_t n, size_t *out_len)
{
    uint8_t *o = (uint8_t *)malloc(n + 1);
    if (!o)
        pf_fatal("out of memory");
    size_t j = 0, zeros = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = e[i];
        if (zeros >= 2 && b == 3 && (i + 1 >= n || e[i + 1] <= 3)) {
            zeros = 0;
            continue;
        }
        o[j++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    *out_len = j;
    return o;
}

uint8_t *rbsp_to_ebsp(const uint8_t *rp, size_t n, size_t *out_len)
{
    uint8_t *o = (uint8_t *)malloc(n + n / 2 + 16);
    if (!o)
        pf_fatal("out of memory");
    size_t j = 0, zeros = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = rp[i];
        if (zeros >= 2 && b <= 3) {
            o[j++] = 3;
            zeros = 0;
        }
        o[j++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    *out_len = j;
    return o;
}
