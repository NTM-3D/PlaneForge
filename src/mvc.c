/* MVC stream analysis, offset-metadata writing, OFS and JSON interchange. */
#include "pf.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define START4_BYTES "\x00\x00\x00\x01"

/* ── depth byte conversion ─────────────────────────────────────────── */
/* One byte per frame: 0x80 = 0, 0x01..0x7f = +1..+127, 0x81..0xff = -1..-127. */
uint8_t pf_depth_to_byte(int d)
{
    if (d == 0)  return 0x80;
    if (d > 0)   return (uint8_t)d;
    return (uint8_t)(0x80 | ((-d) & 0x7f));
}

int pf_byte_to_depth(uint8_t b)
{
    return (b & 0x80) ? -(int)(b & 0x7f) : (int)(b & 0x7f);
}

/* Depth 0 marks an empty plane and is not a valid 3D-plane depth.  A plane
 * whose frames are all 0x80 is an empty plane, not a depth-0 plane, and is
 * left alone.  Otherwise every 0x80 frame is a depth-0 frame: fatal without
 * force, and written as +1 with it. */
long pf_depth_check(uint8_t *data, size_t n, int plane_id, int force,
                    const char *what)
{
    int any = 0;
    for (size_t i = 0; i < n; i++)
        if (data[i] != 0x80) { any = 1; break; }
    if (!any)
        return 0;

    long zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] != 0x80) continue;
        zeros++;
        if (force)
            data[i] = 0x01;
    }
    if (zeros == 0)
        return 0;
    if (!force)
        pf_fatal("plane %d: %s has %ld depth-0 frame(s); depth 0 marks an empty "
                 "plane and is not a valid 3D-plane depth (use -force to write "
                 "them as +1)", plane_id, what, zeros);
    if (zeros)
        pf_warn("plane %d: %s has %ld depth-0 frame(s); written as +1",
                plane_id, what, zeros);
    return zeros;
}

/* Same rule for a single depth value. */
int pf_depth_value(int d, int force, const char *what)
{
    if (d != 0)
        return d;
    if (!force)
        pf_fatal("%s: depth 0 marks an empty plane and is not a valid 3D-plane "
                 "depth (use -force to write +1)", what);
    pf_warn("%s: depth 0 written as +1", what);
    return 1;
}

/* Build a random GUID for a plane.  The last byte carries the plane id. */
void pf_make_guid(uint8_t guid[16], int plane_id)
{
    static int seeded = 0;
    static unsigned int rng = 0;
    if (!seeded) {
        rng = (unsigned int)time(NULL) ^ 0x9e3779b9u ^ (unsigned int)(uintptr_t)guid;
        seeded = 1;
    }
    for (int i = 0; i < 16; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        guid[i] = (uint8_t)rng;
    }
    guid[15] = (uint8_t)plane_id;
}

/* ── plane sources ─────────────────────────────────────────────────── */
static PlaneSource *src_alloc(void)
{
    PlaneSource *s = (PlaneSource *)calloc(1, sizeof(PlaneSource));
    if (!s) pf_fatal("out of memory");
    return s;
}

PlaneSource *src_static(int depth)
{
    PlaneSource *s = src_alloc();
    s->kind = SRC_STATIC;
    s->value = pf_depth_to_byte(depth);
    return s;
}

PlaneSource *src_sweep(int lo, int hi, int interval)
{
    PlaneSource *s = src_alloc();
    s->kind = SRC_SWEEP;
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    s->lo = lo; s->hi = hi; s->interval = interval;
    s->fixed = (lo == hi);
    int t = (0 > lo) ? 0 : lo;
    int cur = (t < hi) ? t : hi;
    s->cur = (cur == 0) ? 1 : cur;
    s->dir = 1; s->cnt = 0;
    return s;
}

PlaneSource *src_ofs(const uint8_t *data, size_t len, int pad_depth)
{
    PlaneSource *s = src_alloc();
    s->kind = SRC_OFS;
    s->data = data;
    s->len = len;
    s->pos = 0;
    s->value = pf_depth_to_byte(pad_depth);
    return s;
}

void src_free(PlaneSource *s)
{
    free(s);
}

uint8_t src_next(PlaneSource *s)
{
    switch (s->kind) {
    case SRC_STATIC:
        return s->value;
    case SRC_OFS:
        return (s->pos < s->len) ? s->data[s->pos++] : s->value;
    case SRC_SWEEP: {
        if (s->fixed)
            return pf_depth_to_byte(s->lo);
        int d = s->cur;
        s->cnt++;
        if (s->cnt >= s->interval) {
            s->cnt = 0;
            if (s->dir == 1) {
                if (s->cur >= s->hi) { s->dir = -1; s->cur = s->cur - 1; }
                else                 { s->cur = s->cur + 1; }
                if (s->cur == 0) s->cur = 1;
            } else {
                if (s->cur <= s->lo) { s->dir = 1; s->cur = s->cur + 1; }
                else                 { s->cur = s->cur - 1; }
                if (s->cur == 0) s->cur = -1;
            }
        }
        return pf_depth_to_byte(d);
    }
    }
    return 0x80;
}

/* ── scan ──────────────────────────────────────────────────────────── */
void scan_result_free(ScanResult *r)
{
    free(r->ps);
    r->ps = NULL;
}

int pf_scan(const char *path, ScanResult *out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        pf_fatal("cannot open '%s'", path);

    memset(out, 0, sizeof(*out));
    out->first_sei_hdr = -1;
    out->frame_rate = -1;

    PfReader r;
    pf_reader_init(&r, fp);
    PfProgress pr;
    pf_progress_start(&pr, "Analyzing", pf_file_size(path));
    int ret;
    while ((ret = pf_reader_next(&r)) == PF_NAL) {
        pf_progress_tick(&pr, r.bytes_read);
        int nt = r.nal_type;
        if (nt >= 0 && nt < 32)
            out->nal_counts[nt]++;
        out->nal_total++;

        if (nt != NAL_SEI)
            continue;
        if (out->first_sei_hdr < 0 && r.nal_len > 0)
            out->first_sei_hdr = r.nal[0];

        size_t rlen = 0;
        uint8_t *rbsp = ebsp_to_rbsp(r.nal + 1, r.nal_len - 1, &rlen);
        long idx = find_ofmd(rbsp, rlen);
        if (idx >= 0) {
            out->has_ofmd = 1;
            out->ofmd_messages++;
            OfmdFields f;
            if (ofmd_fields(rbsp + idx, rlen - (size_t)idx, &f) == 0) {
                if (f.planes > out->planes) {
                    int old = out->planes;
                    out->ps = (PlaneStat *)realloc(out->ps, sizeof(PlaneStat) * f.planes);
                    if (!out->ps) pf_fatal("out of memory");
                    for (int q = old; q < f.planes; q++)
                        memset(&out->ps[q], 0, sizeof(PlaneStat));
                    out->planes = f.planes;
                }
                if (f.frame_rate >= 1 && f.frame_rate <= 7 && f.frames > 0) {
                    if (out->frame_rate < 0)
                        out->frame_rate = f.frame_rate;
                    out->total_frames += f.frames;

                    int np = f.planes, nfr = f.frames;
                    size_t base = (size_t)idx + OFMD_HDR;
                    for (int p = 0; p < np; p++) {
                        size_t start = base + (size_t)p * nfr;
                        if (start + (size_t)nfr > rlen) /* malformed */
                            break;
                        PlaneStat *st = &out->ps[p];
                        st->frames += nfr;
                        int prev = st->last;
                        int prev_d = 0;
                        for (int i = 0; i < nfr; i++) {
                            uint8_t b = rbsp[start + i];
                            int d = (b & 0x80) ? -(b & 0x7f) : (b & 0x7f);
                            if (!st->have_minmax) { st->min = st->max = d; st->have_minmax = 1; }
                            else {
                                if (d < st->min) st->min = d;
                                if (d > st->max) st->max = d;
                            }
                            if (i == 0) {
                                if (st->have_last && d != prev) st->changes++;
                            } else if (d != prev_d) {
                                st->changes++;
                            }
                            prev_d = d;
                            st->last = d;
                            st->have_last = 1;
                        }
                    }
                }
            }
        }
        free(rbsp);
    }

    pf_progress_end(&pr);
    pf_reader_free(&r);
    fclose(fp);
    return 0;
}

/* ── SEI patching ──────────────────────────────────────────────────── */
static uint8_t *modified_sei_nal(const uint8_t *payload, size_t plen,
                                 PlaneSource **sources, WriteStats *st,
                                 size_t *out_len)
{
    if (plen == 0 || (payload[0] & 0x1f) != NAL_SEI)
        return NULL;

    size_t rlen = 0;
    uint8_t *rbsp = ebsp_to_rbsp(payload + 1, plen - 1, &rlen);
    long idx = find_ofmd(rbsp, rlen);
    if (idx < 0) { free(rbsp); return NULL; }

    OfmdFields f;
    if (ofmd_fields(rbsp + idx, rlen - (size_t)idx, &f) != 0) { free(rbsp); return NULL; }
    if (!(f.frame_rate >= 1 && f.frame_rate <= 7) || f.frames == 0) { free(rbsp); return NULL; }

    size_t data_start = (size_t)idx + OFMD_HDR;
    size_t pg_end = data_start + (size_t)f.planes * (size_t)f.frames;
    if (pg_end > rlen) { free(rbsp); return NULL; }

    int tgt_max = -1;
    for (int p = 0; p < MAX_PLANES; p++)
        if (sources[p]) tgt_max = p;

    int new_n = f.planes;
    if (tgt_max + 1 > new_n) new_n = tgt_max + 1;
    if (new_n > MAX_PLANES) { free(rbsp); return NULL; }
    int created = (new_n > f.planes);

    if (created) {
        /* Adding planes changes the block size, so the nested SEI length
         * fields must change too.  Rebuild the whole SEI NAL from the
         * parsed OFMD block (keeping the original header bytes). */
        size_t body = (size_t)new_n * (size_t)f.frames;
        uint8_t *nb = (uint8_t *)malloc(OFMD_HDR + body);
        if (!nb) pf_fatal("out of memory");
        memcpy(nb, rbsp + idx, OFMD_HDR);
        nb[10] = (uint8_t)((rbsp[idx + 10] & 0x80) | (new_n & 0x7f));
        memcpy(nb + OFMD_HDR, rbsp + data_start, (size_t)f.planes * (size_t)f.frames);
        memset(nb + OFMD_HDR + (size_t)f.planes * (size_t)f.frames,
               0x80, body - (size_t)f.planes * (size_t)f.frames);

        int any = 0;
        for (int p = 0; p < new_n; p++) {
            PlaneSource *src = sources[p];
            if (!src)
                continue;
            size_t off = (size_t)OFMD_HDR + (size_t)p * (size_t)f.frames;
            for (int i = 0; i < f.frames; i++)
                nb[off + i] = src_next(src);
            any = 1;
        }
        if (!any) { free(rbsp); free(nb); return NULL; }

        size_t slen = 0;
        uint8_t *sei = build_ofmd_sei_nal(nb, OFMD_HDR + body, payload[0], &slen);
        free(nb);
        free(rbsp);

        st->messages_modified++;
        st->frames_per_message = f.frames;
        if (new_n > st->planes_created)
            st->planes_created = new_n;
        *out_len = slen;
        return sei;
    }

    int any = 0;
    for (int p = 0; p < MAX_PLANES; p++) {
        PlaneSource *src = sources[p];
        if (!src || p >= new_n)
            continue;
        size_t off = data_start + (size_t)p * (size_t)f.frames;
        for (int i = 0; i < f.frames; i++)
            rbsp[off + i] = src_next(src);
        any = 1;
    }
    if (!any) { free(rbsp); return NULL; }

    st->messages_modified++;
    st->frames_per_message = f.frames;

    size_t ebsp_len = 0;
    uint8_t *ebsp = rbsp_to_ebsp(rbsp, rlen, &ebsp_len);
    free(rbsp);

    uint8_t *out = (uint8_t *)malloc(ebsp_len + 1);
    if (!out) pf_fatal("out of memory");
    out[0] = payload[0];
    memcpy(out + 1, ebsp, ebsp_len);
    free(ebsp);
    *out_len = ebsp_len + 1;
    return out;
}

/* ── existing-OFMD writer ──────────────────────────────────────────── */
static int write_existing(const char *in_path, FILE *out,
                          PlaneSource **sources, WriteStats *st)
{
    FILE *fp = fopen(in_path, "rb");
    if (!fp) pf_fatal("cannot open '%s'", in_path);
    PfReader r;
    pf_reader_init(&r, fp);

    uint8_t *last_ofmd = NULL;
    size_t last_len = 0;
    int seen_gop = 0;

    PfProgress pr;
    pf_progress_start(&pr, "Writing", pf_file_size(in_path));

    int ret;
    while ((ret = pf_reader_next(&r)) == PF_NAL) {
        if (r.gap_len)
            fwrite(r.gap, 1, r.gap_len, out);

        int nt = r.nal_type;
        if (nt == NAL_IDR) {
            if (!seen_gop && last_ofmd) {
                fwrite(START4_BYTES, 1, 4, out);
                fwrite(last_ofmd, 1, last_len, out);
                st->injected_gops++;
            }
            seen_gop = 0;
            fwrite(r.sc, 1, r.sc_len, out);
            fwrite(r.nal, 1, r.nal_len, out);
        } else if (nt == NAL_SEI) {
            size_t nl = 0;
            uint8_t *nn = modified_sei_nal(r.nal, r.nal_len, sources, st, &nl);

            int had = 0;
            if (r.nal_len > 0) {
                size_t hl = 0;
                uint8_t *rb = ebsp_to_rbsp(r.nal + 1, r.nal_len - 1, &hl);
                had = find_ofmd(rb, hl) >= 0;
                free(rb);
            }
            if (had) {
                free(last_ofmd);
                const uint8_t *keep = nn ? nn : r.nal;
                size_t klen = nn ? nl : r.nal_len;
                last_ofmd = (uint8_t *)malloc(klen);
                memcpy(last_ofmd, keep, klen);
                last_len = klen;
                seen_gop = 1;
            }
            fwrite(r.sc, 1, r.sc_len, out);
            if (nn) { fwrite(nn, 1, nl, out); free(nn); }
            else    { fwrite(r.nal, 1, r.nal_len, out); }
        } else {
            fwrite(r.sc, 1, r.sc_len, out);
            fwrite(r.nal, 1, r.nal_len, out);
        }
        pf_progress_tick(&pr, r.bytes_read);
    }
    if (ret == PF_TAIL && r.gap_len)
        fwrite(r.gap, 1, r.gap_len, out);

    pf_progress_end(&pr);
    free(last_ofmd);
    pf_reader_free(&r);
    fclose(fp);
    return 0;
}

/* ── base-view GOP parsing ─────────────────────────────────────────── */
/*
 * A picture may be spread over several slice NAL units, so counting slice
 * NALs over-counts frames on streams that use multiple slices per picture.
 * Instead the first slice of each picture is counted, identified by
 * first_mb_in_slice == 0 (the first ue(v) of a VCL slice header).
 * IDR pictures are those whose first slice has nal_unit_type 5.
 */
typedef struct { const uint8_t *p; size_t bits; } BitReader;

static int br_bit(BitReader *b)
{
    int v = (b->p[b->bits >> 3] >> (7 - (b->bits & 7))) & 1;
    b->bits++;
    return v;
}

static uint32_t br_ue(BitReader *b, size_t maxbits)
{
    int z = 0;
    while (z < 32 && b->bits < maxbits && !br_bit(b))
        z++;
    uint32_t v = (z >= 32) ? 0u : ((1u << z) - 1u);
    for (int i = 0; i < z && b->bits < maxbits; i++)
        v += (uint32_t)br_bit(b) << (z - 1 - i);
    return v;
}

/* Return first_mb_in_slice of a VCL slice NAL, or -1 on failure. */
static int vcl_first_mb(const PfReader *r, uint32_t *out)
{
    size_t rlen = 0;
    uint8_t *rb = ebsp_to_rbsp(r->nal + 1, r->nal_len - 1, &rlen);
    if (rlen == 0) { free(rb); return -1; }
    BitReader b = { rb, 0 };
    *out = br_ue(&b, rlen * 8);
    free(rb);
    return 0;
}

static long long *parse_gops(const char *path, int *out_ngops)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) pf_fatal("cannot open base-view '%s'", path);
    PfReader r;
    pf_reader_init(&r, fp);

    long long frame_idx = 0;
    long long *idr = NULL;
    int nidr = 0, cap = 0;

    PfProgress pr;
    pf_progress_start(&pr, "Scanning base view", pf_file_size(path));

    while (pf_reader_next(&r) == PF_NAL) {
        int nt = r.nal_type;
        if (nt != NAL_SLICE && nt != NAL_IDR)
            continue;
        uint32_t first_mb = 0;
        if (vcl_first_mb(&r, &first_mb) != 0 || first_mb != 0)
            continue;               /* not the first slice of a picture */
        if (nt == NAL_IDR) {
            if (nidr == cap) {
                cap = cap ? cap * 2 : 256;
                idr = (long long *)realloc(idr, sizeof(long long) * cap);
                if (!idr) pf_fatal("out of memory");
            }
            idr[nidr++] = frame_idx;
        }
        frame_idx++;
        pf_progress_tick(&pr, r.bytes_read);
    }
    pf_progress_end(&pr);
    pf_reader_free(&r);
    fclose(fp);

    if (nidr == 0)
        pf_fatal("no IDR frames found in base view '%s'", path);

    long long *gops = (long long *)malloc(sizeof(long long) * nidr);
    if (!gops) pf_fatal("out of memory");
    for (int i = 0; i < nidr; i++) {
        long long end = (i + 1 < nidr) ? idr[i + 1] : frame_idx;
        gops[i] = end - idr[i];
    }
    free(idr);
    *out_ngops = nidr;
    return gops;
}

/* Split GOPs longer than the OFMD 7-bit frame field into <=MAX_OFFSET chunks. */
static long long *expand_gops(const long long *gops, int ngops,
                              int *out_n, long long *out_total)
{
    size_t cap = 0;
    for (int i = 0; i < ngops; i++)
        cap += (size_t)((gops[i] > 0 ? (gops[i] + MAX_OFFSET - 1) / MAX_OFFSET : 1) + 1);
    long long *out = (long long *)malloc(sizeof(long long) * (cap ? cap : 1));
    if (!out) pf_fatal("out of memory");

    int n = 0;
    long long total = 0;
    for (int i = 0; i < ngops; i++) {
        long long gl = gops[i];
        if (gl <= 0) continue;
        if (gl > MAX_OFFSET)
            pf_warn("GOP of %lld frame(s) exceeds the %d-frame OFMD limit; splitting",
                    gl, MAX_OFFSET);
        while (gl > MAX_OFFSET) { out[n++] = MAX_OFFSET; total += MAX_OFFSET; gl -= MAX_OFFSET; }
        out[n++] = gl;
        total += gl;
    }
    *out_n = n;
    *out_total = total;
    return out;
}

long long pf_base_view_frames(const char *base_view_path)
{
    int ng = 0;
    long long *g = parse_gops(base_view_path, &ng);
    long long s = 0;
    for (int i = 0; i < ng; i++)
        s += g[i];
    free(g);
    return s;
}

/* ── new-OFMD writer ───────────────────────────────────────────────── */

/* Build the OFMD SEI NAL for one GOP and write it to the output stream. */
static void inject_ofmd_sei(FILE *out, PlaneSource **sources, int n_planes_new,
                            long long gl, int sei_hdr_byte, WriteStats *st)
{
    size_t body = (size_t)n_planes_new * (size_t)gl;
    uint8_t *pd = (uint8_t *)malloc(body);
    if (!pd) pf_fatal("out of memory");
    for (int p = 0; p < n_planes_new; p++) {
        PlaneSource *s = sources[p];
        size_t off = (size_t)p * (size_t)gl;
        for (long long i = 0; i < gl; i++)
            pd[off + i] = s ? src_next(s) : 0x80;
    }
    size_t ofmdlen = 0;
    uint8_t *ofmd = build_ofmd_block(n_planes_new, (int)gl, 1, pd, body, &ofmdlen);
    free(pd);
    size_t seilen = 0;
    uint8_t *sei = build_ofmd_sei_nal(ofmd, ofmdlen, (uint8_t)sei_hdr_byte, &seilen);
    free(ofmd);
    fwrite(START4_BYTES, 1, 4, out);
    fwrite(sei, 1, seilen, out);
    free(sei);
    st->messages_modified++;
}

/* Insert the OFMD SEI NALs at the GOP boundaries.  The frame markers are
 * type-24 NALs, or AUD NALs when the stream holds none of the former; an
 * AUD is followed by the SEI NALs of its access unit. */
static int write_new(const char *in_path, FILE *out, PlaneSource **sources,
                     WriteStats *st, long long *gops, int ngops,
                     int n_planes_new, int sei_hdr_byte, int marker_type)
{
    long long *start = ngops ? (long long *)malloc(sizeof(long long) * ngops) : NULL;
    if (ngops && !start) pf_fatal("out of memory");
    long long cum = 0;
    for (int i = 0; i < ngops; i++) { start[i] = cum; cum += gops[i]; }

    FILE *fp = fopen(in_path, "rb");
    if (!fp) pf_fatal("cannot open '%s'", in_path);
    PfReader r;
    pf_reader_init(&r, fp);

    long long marker_idx = 0;
    int gop_idx = 0;
    long long next_gop_at = ngops ? start[0] : 0;
    int after_marker = (marker_type != NAL_TYPE24);

    PfProgress pr;
    pf_progress_start(&pr, "Injecting", pf_file_size(in_path));

    int ret;
    while ((ret = pf_reader_next(&r)) == PF_NAL) {
        if (r.gap_len)
            fwrite(r.gap, 1, r.gap_len, out);

        int is_marker = (r.nal_type == marker_type);
        int inject = is_marker && gop_idx < ngops && marker_idx >= next_gop_at;
        long long gl = 0;
        if (inject) {
            gl = gops[gop_idx];
            if (gl <= 0 || gl > MAX_OFFSET) { free(start); pf_fatal("invalid GOP length %lld for OFMD (max %d)", gl, MAX_OFFSET); }
            if (!after_marker)
                inject_ofmd_sei(out, sources, n_planes_new, gl, sei_hdr_byte, st);
        }
        fwrite(r.sc, 1, r.sc_len, out);
        fwrite(r.nal, 1, r.nal_len, out);
        if (inject) {
            if (after_marker)
                inject_ofmd_sei(out, sources, n_planes_new, gl, sei_hdr_byte, st);
            gop_idx++;
            if (gop_idx < ngops) next_gop_at = start[gop_idx];
        }
        if (is_marker)
            marker_idx++;
        pf_progress_tick(&pr, r.bytes_read);
    }
    if (ret == PF_TAIL && r.gap_len)
        fwrite(r.gap, 1, r.gap_len, out);

    pf_progress_end(&pr);
    free(start);
    pf_reader_free(&r);
    fclose(fp);
    return 0;
}

/* ── dispatch ──────────────────────────────────────────────────────── */
static int write_stream_impl(const char *in_path, const char *out_path,
                             PlaneSource **sources, WriteStats *st,
                             const char *base_view, const ScanResult *pre,
                             int force_planes_new)
{
    ScanResult local;
    const ScanResult *sr = pre;
    if (!sr) {
        pf_scan(in_path, &local);
        sr = &local;
    }

    FILE *fo = fopen(out_path, "wb");
    if (!fo)
        pf_fatal("cannot create '%s'", out_path);

    int rc = 0;
    if (sr->has_ofmd) {
        rc = write_existing(in_path, fo, sources, st);
    } else {
        if (!base_view) {
            fclose(fo);
            if (!pre) scan_result_free(&local);
            pf_fatal("input contains no OFMD offset-metadata; supply -base-view "
                     "so GOP boundaries can be determined");
        }
        int marker_type = NAL_TYPE24;
        if (sr->nal_counts[NAL_TYPE24] == 0) {
            if (sr->nal_counts[NAL_TYPE_AUD] == 0) {
                fclose(fo);
                if (!pre) scan_result_free(&local);
                pf_fatal("no type-24 or AUD NALs found in MVC stream; cannot determine frame boundaries");
            }
            marker_type = NAL_TYPE_AUD;
        }
        int ngops = 0;
        long long *gops0 = parse_gops(base_view, &ngops);
        int nchunk = 0;
        long long total = 0;
        long long *gops = expand_gops(gops0, ngops, &nchunk, &total);
        free(gops0);
        if (total != sr->nal_counts[marker_type])
            pf_warn("base view has %lld frame(s) but MVC has %lld frame "
                    "marker(s); offset metadata may be misaligned",
                    total, sr->nal_counts[marker_type]);
        int np = force_planes_new > 0 ? force_planes_new : MAX_PLANES;
        int seihdr = sr->first_sei_hdr >= 0 ? sr->first_sei_hdr : 0x06;
        rc = write_new(in_path, fo, sources, st, gops, nchunk, np, seihdr, marker_type);
        free(gops);
    }
    fclose(fo);
    if (!pre) scan_result_free(&local);
    return rc;
}

int pf_write_stream(const char *in_path, const char *out_path,
                    PlaneSource *sources[MAX_PLANES], WriteStats *st,
                    const char *base_view)
{
    PlaneSource **s = (PlaneSource **)sources;
    return write_stream_impl(in_path, out_path, s, st, base_view, NULL, 0);
}

int pf_write_stream_pre(const char *in_path, const char *out_path,
                        PlaneSource *sources[MAX_PLANES], WriteStats *st,
                        const char *base_view, const ScanResult *pre)
{
    PlaneSource **s = (PlaneSource **)sources;
    return write_stream_impl(in_path, out_path, s, st, base_view, pre, 0);
}

/* ── plane collection ──────────────────────────────────────────────── */
static void buf_append(uint8_t **d, size_t *len, size_t *cap,
                       const uint8_t *p, size_t n)
{
    if (*len + n > *cap) {
        size_t nc = *cap ? *cap * 2 : (1u << 16);
        while (nc < *len + n) nc *= 2;
        *d = (uint8_t *)realloc(*d, nc);
        if (!*d) pf_fatal("out of memory");
        *cap = nc;
    }
    memcpy(*d + *len, p, n);
    *len += n;
}

static int all_undefined(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (d[i] != 0x80) return 0;
    return 1;
}

/* Read the offset-metadata of an MVC stream into a plane set.  Planes with
 * no depth data are left out, as they carry no 3D-plane information. */
static void collect_stream_planes(const char *in_path, PlaneSet *out,
                                  long long *out_msgs, int fps_override,
                                  int have_fps, int drop_frame,
                                  int have_plane, int only_plane, int force)
{
    FILE *fp = fopen(in_path, "rb");
    if (!fp) pf_fatal("cannot open '%s'", in_path);
    PfReader r;
    pf_reader_init(&r, fp);

    uint8_t **bufs = NULL;
    size_t *lens = NULL, *caps = NULL;
    int np = 0, frame_rate = -1;
    long long msgs = 0;

    PfProgress pr;
    pf_progress_start(&pr, "Exporting", pf_file_size(in_path));

    int ret;
    while ((ret = pf_reader_next(&r)) == PF_NAL) {
        if (r.nal_type != NAL_SEI)
            continue;
        size_t rlen = 0;
        uint8_t *rbsp = ebsp_to_rbsp(r.nal + 1, r.nal_len - 1, &rlen);
        long idx = find_ofmd(rbsp, rlen);
        if (idx < 0) { free(rbsp); continue; }
        OfmdFields f;
        if (ofmd_fields(rbsp + idx, rlen - (size_t)idx, &f) != 0 ||
            !(f.frame_rate >= 1 && f.frame_rate <= 7) || f.frames <= 0) {
            free(rbsp);
            continue;
        }
        if (frame_rate < 0)
            frame_rate = f.frame_rate;

        if (np == 0) {
            np = f.planes;
            bufs = (uint8_t **)calloc(np ? np : 1, sizeof(uint8_t *));
            lens = (size_t *)calloc(np ? np : 1, sizeof(size_t));
            caps = (size_t *)calloc(np ? np : 1, sizeof(size_t));
            if (!bufs || !lens || !caps) pf_fatal("out of memory");
        }
        msgs++;
        size_t base = (size_t)idx + OFMD_HDR;
        int nfr = f.frames;
        for (int p = 0; p < np; p++) {
            size_t start = base + (size_t)p * nfr;
            if (start + (size_t)nfr > rlen)
                break;
            buf_append(&bufs[p], &lens[p], &caps[p], rbsp + start, (size_t)nfr);
        }
        free(rbsp);
        pf_progress_tick(&pr, r.bytes_read);
    }
    pf_progress_end(&pr);
    pf_reader_free(&r);
    fclose(fp);

    if (np == 0 || frame_rate < 0)
        pf_fatal("no OFMD offset-metadata found in '%s'", in_path);

    if (have_plane && (only_plane < 0 || only_plane >= np))
        pf_fatal("plane %d is not present (input has %d plane(s))", only_plane, np);

    int eff_fr = have_fps ? fps_override : frame_rate;
    if (drop_frame && eff_fr != 4)
        pf_fatal("drop_frame requires a frame rate of 4 (29.97)");

    memset(out, 0, sizeof(*out));
    out->frame_rate = eff_fr;
    out->drop_frame = drop_frame ? 1 : 0;
    out->planes = (OfsPlane *)calloc(np ? np : 1, sizeof(OfsPlane));
    if (!out->planes) pf_fatal("out of memory");

    int n = 0;
    for (int p = 0; p < np; p++) {
        if (have_plane && p != only_plane)
            continue;
        if (lens[p] == 0 || all_undefined(bufs[p], lens[p]))
            continue;
        OfsPlane *o = &out->planes[n];
        memset(o, 0, sizeof(*o));
        o->plane_id = p;
        pf_make_guid(o->guid, p);
        o->frame_rate = eff_fr;
        o->drop_frame = drop_frame ? 1 : 0;
        o->num_frames = (uint32_t)lens[p];
        o->offsets = bufs[p];
        bufs[p] = NULL;
        pf_depth_check(o->offsets, o->num_frames, p, force, "stream");
        if (out->num_frames == 0)
            out->num_frames = o->num_frames;
        n++;
    }
    out->n = n;

    for (int p = 0; p < np; p++)
        free(bufs[p]);
    free(bufs); free(lens); free(caps);

    *out_msgs = msgs;
}

/* ── export to OFS ──────────────────────────────────────────────────── */
int cmd_export(const char *in_path, const char *out_dir, int fps_override,
               int have_fps, int drop_frame, int have_plane, int only_plane,
               int force)
{
    PlaneSet set;
    long long msgs = 0;
    collect_stream_planes(in_path, &set, &msgs, fps_override, have_fps,
                          drop_frame, have_plane, only_plane, force);

    pf_ensure_dir(out_dir);

    int written = 0;
    for (int i = 0; i < set.n; i++) {
        OfsPlane *o = &set.planes[i];
        char name[32], path[4096];
        snprintf(name, sizeof(name), "3D-Plane-%02d.ofs", o->plane_id);
        pf_join_path(path, sizeof(path), out_dir, name);
        if (ofs_write(path, o) == 0) {
            printf("%s: %u frames (frame_rate %d, %s)\n",
                   name, o->num_frames, o->frame_rate,
                   o->drop_frame ? "drop-frame" : "non-drop");
            written++;
        }
    }

    if (have_plane && written == 0)
        pf_fatal("plane %d has no defined depth data in '%s'", only_plane, in_path);

    printf("Exported %d plane(s) from %lld OFMD message(s) to '%s'\n",
           written, msgs, out_dir);

    planeset_free(&set);
    return 0;
}

/* ── export to JSON ─────────────────────────────────────────────────── */
int cmd_export_json(const char *in_path, const char *out_path, int fps_override,
                    int have_fps, int drop_frame, int have_plane, int only_plane,
                    int force)
{
    PlaneSet set;
    long long msgs = 0;
    collect_stream_planes(in_path, &set, &msgs, fps_override, have_fps,
                          drop_frame, have_plane, only_plane, force);

    int combined = pf_path_has_ext(out_path, ".json");
    if (pjson_write(out_path, &set, combined) != 0) {
        planeset_free(&set);
        pf_fatal("cannot write plane JSON to '%s'", out_path);
    }

    for (int i = 0; i < set.n; i++) {
        OfsPlane *o = &set.planes[i];
        printf("plane %d: %u frames (frame_rate %d, %s)\n",
               o->plane_id, o->num_frames, o->frame_rate,
               o->drop_frame ? "drop-frame" : "non-drop");
    }

    if (have_plane && set.n == 0)
        pf_fatal("plane %d has no defined depth data in '%s'", only_plane, in_path);

    printf("Exported %d plane(s) from %lld OFMD message(s) to '%s'\n",
           set.n, msgs, out_path);

    planeset_free(&set);
    return 0;
}

/* ── OFS files ──────────────────────────────────────────────────────── */
static int collect_ofs_files(const char *from_ofs, char paths[][4096], int max)
{
    /* A directory is turned into 3D-Plane-%02d.ofs for ids 0..MAX_PLANES-1.
     * A single file is used as-is.  */
    int n = 0;
    if (pf_is_dir(from_ofs)) {
        for (int p = 0; p < MAX_PLANES && n < max; p++) {
            char name[32];
            snprintf(name, sizeof(name), "3D-Plane-%02d.ofs", p);
            pf_join_path(paths[n], 4096, from_ofs, name);
            FILE *t = fopen(paths[n], "rb");
            if (t) { fclose(t); n++; }
        }
    } else {
        snprintf(paths[0], 4096, "%s", from_ofs);
        n = 1;
    }
    return n;
}

static void load_ofs_set(const char *from_ofs, PlaneSet *out)
{
    char paths[MAX_PLANES][4096];
    int nfiles = collect_ofs_files(from_ofs, paths, MAX_PLANES);
    if (nfiles == 0)
        pf_fatal("no OFS files found at '%s'", from_ofs);

    memset(out, 0, sizeof(*out));
    for (int i = 0; i < nfiles; i++) {
        OfsPlane o;
        if (ofs_read(paths[i], &o) != 0)
            continue;
        out->planes = (OfsPlane *)realloc(out->planes,
                                          sizeof(OfsPlane) * (size_t)(out->n + 1));
        if (!out->planes) pf_fatal("out of memory");
        out->planes[out->n++] = o;
    }
    if (out->n == 0)
        pf_fatal("no usable OFS files found at '%s'", from_ofs);

    out->frame_rate = out->planes[0].frame_rate;
    out->drop_frame = out->planes[0].drop_frame;
    out->num_frames = out->planes[0].num_frames;
    for (int i = 1; i < out->n; i++)
        if (out->planes[i].frame_rate != out->frame_rate)
            pf_warn("plane %d has frame rate %d but plane %d has %d",
                    out->planes[i].plane_id, out->planes[i].frame_rate,
                    out->planes[0].plane_id, out->frame_rate);
}

/* ── import into an MVC stream ─────────────────────────────────────── */
static int import_plane_set(const char *in_path, const char *out_path,
                             PlaneSet *set, int have_override, int override_depth,
                             const char *base_view, const int *targets,
                             int ntargets, int force, const char *what)
{
    int nloaded = set->n;

    /* Map each loaded plane onto a target plane index. */
    int file_plane[MAX_PLANES];
    if (ntargets == 0) {
        for (int i = 0; i < nloaded; i++) {
            int id = set->planes[i].plane_id;
            if (id < 0 || id >= MAX_PLANES)
                pf_fatal("%s plane %d has an out-of-range plane id %d", what, i, id);
            file_plane[i] = id;
        }
    } else if (ntargets == 1) {
        int base = targets[0];
        if (base < 0 || base >= MAX_PLANES)
            pf_fatal("target plane %d is out of range 0..%d", base, MAX_PLANES - 1);
        if (base + nloaded > MAX_PLANES)
            pf_fatal("target range %d..%d exceeds plane %d",
                     base, base + nloaded - 1, MAX_PLANES - 1);
        for (int i = 0; i < nloaded; i++)
            file_plane[i] = base + i;
    } else {
        if (ntargets != nloaded)
            pf_fatal("plane list names %d plane(s) but %d %s file(s) were found",
                     ntargets, nloaded, what);
        for (int i = 0; i < nloaded; i++) {
            if (targets[i] < 0 || targets[i] >= MAX_PLANES)
                pf_fatal("target plane %d is out of range 0..%d", targets[i], MAX_PLANES - 1);
            file_plane[i] = targets[i];
        }
    }

    int have[MAX_PLANES];
    memset(have, 0, sizeof(have));
    for (int i = 0; i < nloaded; i++) {
        int id = file_plane[i];
        if (have[id])
            pf_fatal("two %s planes map to plane %d", what, id);
        have[id] = 1;
    }

    for (int i = 0; i < nloaded; i++) {
        OfsPlane *o = &set->planes[i];
        pf_depth_check(o->offsets, o->num_frames, file_plane[i], force, what);
    }

    ScanResult sr;
    pf_scan(in_path, &sr);

    int mvc_fr = sr.has_ofmd ? sr.frame_rate : -1;
    if (mvc_fr >= 0) {
        for (int i = 0; i < nloaded; i++) {
            if (set->planes[i].frame_rate && set->planes[i].frame_rate != mvc_fr)
                pf_warn("%s for plane %d has frame_rate %d but MVC has %d "
                        "(OFMD frame rate left unchanged)",
                        what, file_plane[i], set->planes[i].frame_rate, mvc_fr);
        }
    }

    PlaneSource *sources[MAX_PLANES];
    memset(sources, 0, sizeof(sources));

    /* Frames the writer produces per plane: the summed OFMD frame count in
     * existing-OFMD mode, or the base-view frame count otherwise. */
    long long N;
    if (sr.has_ofmd) {
        N = sr.total_frames;
    } else {
        if (!base_view)
            pf_fatal("input has no OFMD; supply -base-view to import metadata");
        N = pf_base_view_frames(base_view);
    }

    int mismatch = 0;
    for (int i = 0; i < nloaded; i++) {
        int id = file_plane[i];
        OfsPlane *o = &set->planes[i];
        long long M = o->num_frames;
        if (M == N) {
            sources[id] = src_ofs(o->offsets, o->num_frames, 0);
        } else {
            mismatch++;
            if (!have_override) {
                pf_fatal("plane %d: %s has %lld frame(s) but MVC has %lld; "
                         "specify -override-depth to resolve the mismatch",
                         id, what, M, N);
            }
            if (M < N) {
                pf_warn("plane %d: %s has %lld frame(s), MVC has %lld; "
                        "padding %lld extra frame(s) with depth %d",
                        id, what, M, N, N - M, override_depth);
                sources[id] = src_ofs(o->offsets, o->num_frames, override_depth);
            } else {
                pf_warn("plane %d: %s has %lld frame(s), MVC has %lld; "
                        "MVC is shorter, using depth %d for all its frame(s)",
                        id, what, M, N, override_depth);
                sources[id] = src_static(override_depth);
            }
        }
    }

    WriteStats st;
    memset(&st, 0, sizeof(st));

    int rc = write_stream_impl(in_path, out_path, sources, &st, base_view,
                               &sr, 0);

    for (int p = 0; p < MAX_PLANES; p++)
        if (sources[p]) src_free(sources[p]);

    printf("Imported %d %s plane(s) into '%s'%s\n",
           nloaded, what, out_path,
           mismatch ? " (frame-count mismatch overridden)" : "");
    printf("OFMD messages changed: %lld, planes created: %lld\n",
           st.messages_modified, st.planes_created);

    scan_result_free(&sr);
    return rc;
}

int cmd_import(const char *in_path, const char *out_path, const char *from_ofs,
               int have_override, int override_depth, const char *base_view,
               const int *targets, int ntargets, int force)
{
    PlaneSet set;
    load_ofs_set(from_ofs, &set);
    int rc = import_plane_set(in_path, out_path, &set, have_override,
                              override_depth, base_view, targets, ntargets,
                              force, "OFS");
    planeset_free(&set);
    return rc;
}

int cmd_import_json(const char *in_path, const char *out_path,
                    const char *from_json,
                    int have_override, int override_depth, const char *base_view,
                    const int *targets, int ntargets, int force)
{
    PlaneSet set;
    if (pjson_read(from_json, &set) != 0)
        pf_fatal("cannot read plane JSON from '%s'", from_json);
    int rc = import_plane_set(in_path, out_path, &set, have_override,
                              override_depth, base_view, targets, ntargets,
                              force, "JSON");
    planeset_free(&set);
    return rc;
}

/* ── format conversion ─────────────────────────────────────────────── */
int cmd_ofs_to_json(const char *from_ofs, const char *out_path, int force)
{
    PlaneSet set;
    load_ofs_set(from_ofs, &set);

    PlaneSet keep;
    memset(&keep, 0, sizeof(keep));
    keep.frame_rate = set.frame_rate;
    keep.drop_frame = set.drop_frame;
    for (int i = 0; i < set.n; i++) {
        OfsPlane *o = &set.planes[i];
        if (o->num_frames == 0 || all_undefined(o->offsets, o->num_frames)) {
            ofs_free(o);
            continue;
        }
        pf_depth_check(o->offsets, o->num_frames, o->plane_id, force, "OFS");
        keep.planes = (OfsPlane *)realloc(keep.planes,
                                          sizeof(OfsPlane) * (size_t)(keep.n + 1));
        if (!keep.planes) pf_fatal("out of memory");
        keep.planes[keep.n++] = *o;
        o->offsets = NULL;
    }
    planeset_free(&set);

    if (keep.n == 0) {
        planeset_free(&keep);
        pf_fatal("no plane with depth data in '%s'", from_ofs);
    }
    if (keep.num_frames == 0)
        keep.num_frames = keep.planes[0].num_frames;

    int combined = pf_path_has_ext(out_path, ".json");
    if (pjson_write(out_path, &keep, combined) != 0) {
        planeset_free(&keep);
        pf_fatal("cannot write plane JSON to '%s'", out_path);
    }

    printf("Converted %d OFS plane(s) to JSON in '%s'\n", keep.n, out_path);
    planeset_free(&keep);
    return 0;
}

int cmd_json_to_ofs(const char *from_json, const char *out_path, int force)
{
    PlaneSet set;
    if (pjson_read(from_json, &set) != 0)
        pf_fatal("cannot read plane JSON from '%s'", from_json);

    for (int i = 0; i < set.n; i++) {
        OfsPlane *o = &set.planes[i];
        pf_depth_check(o->offsets, o->num_frames, o->plane_id, force, "JSON");
    }

    if (pf_path_has_ext(out_path, ".ofs")) {
        if (set.n != 1)
            pf_fatal("'%s' holds %d plane(s); a single OFS file holds one plane",
                     from_json, set.n);
        pf_ensure_parent_dir(out_path);
        if (ofs_write(out_path, &set.planes[0]) != 0)
            pf_fatal("cannot write '%s'", out_path);
        printf("%s: %u frames (plane %d)\n", out_path,
               set.planes[0].num_frames, set.planes[0].plane_id);
    } else {
        pf_ensure_dir(out_path);
        for (int i = 0; i < set.n; i++) {
            OfsPlane *o = &set.planes[i];
            char name[32], path[4096];
            snprintf(name, sizeof(name), "3D-Plane-%02d.ofs", o->plane_id);
            pf_join_path(path, sizeof(path), out_path, name);
            if (ofs_write(path, o) != 0)
                pf_fatal("cannot write '%s'", path);
            printf("%s: %u frames (plane %d)\n", name, o->num_frames, o->plane_id);
        }
    }

    printf("Converted %d plane(s) to OFS in '%s'\n", set.n, out_path);
    planeset_free(&set);
    return 0;
}
