/* Plane JSON interchange.
 *
 * A plane JSON file carries the same information as an OFS 3D-Plane file:
 * the frame rate, the drop-frame flag, the frame count, the plane GUID and
 * the per-frame depths.  The per-frame depths are run-length encoded: the
 * segment list holds one entry for every run of frames that share a depth.
 *
 *   {
 *     "format": "planeforge-planes",
 *     "version": 1,
 *     "frame_rate": 4,
 *     "drop_frame": false,
 *     "num_frames": 167955,
 *     "planes_details": {
 *       "4": {
 *         "plane_id": 4,
 *         "guid": "8f3c1a2b4d5e6f708192a3b4c5d6e7f8",
 *         "start_timecode": 0,
 *         "reserved": "00000000",
 *         "frame_rate": 4,
 *         "drop_frame": false,
 *         "num_frames": 167955,
 *         "segments": [
 *           { "start_frame": 0, "end_frame": 100, "depth": 1 },
 *           { "start_frame": 101, "end_frame": 167954, "depth": -7 }
 *         ]
 *       }
 *     }
 *   }
 */
#include "pf.h"

#include <stdlib.h>
#include <string.h>

#define JSON_PLANE_NAME "3D-Plane-%02d.json"

/* ── small helpers ─────────────────────────────────────────────────── */
static void write_hex(FILE *f, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        fprintf(f, "%02x", b[i]);
}

static int read_hex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int hi = -1, lo = -1;
        char a = s[i * 2], b = s[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── writing ───────────────────────────────────────────────────────── */
static void write_segments(FILE *f, const OfsPlane *p)
{
    size_t i = 0;
    int first = 1;
    fprintf(f, "[");
    while (i < p->num_frames) {
        int d = pf_byte_to_depth(p->offsets[i]);
        size_t j = i + 1;
        while (j < p->num_frames && pf_byte_to_depth(p->offsets[j]) == d)
            j++;
        if (!first) fprintf(f, ",");
        first = 0;
        fprintf(f, "\n        { \"start_frame\": %llu, \"end_frame\": %llu, \"depth\": %d }",
                (unsigned long long)i, (unsigned long long)(j - 1), d);
        i = j;
    }
    if (!first) fprintf(f, "\n      ");
    fprintf(f, "]");
}

static void write_plane(FILE *f, const OfsPlane *p, int last)
{
    fprintf(f, "    \"%d\": {\n", p->plane_id);
    fprintf(f, "      \"plane_id\": %d,\n", p->plane_id);
    fprintf(f, "      \"guid\": \"");
    {
        uint8_t g[16];
        memcpy(g, p->guid, 16);
        g[15] = (uint8_t)p->plane_id;
        write_hex(f, g, 16);
    }
    fprintf(f, "\",\n");
    fprintf(f, "      \"start_timecode\": %lu,\n", (unsigned long)p->start_timecode);
    fprintf(f, "      \"reserved\": \"");
    write_hex(f, p->reserved, 4);
    fprintf(f, "\",\n");
    fprintf(f, "      \"frame_rate\": %d,\n", p->frame_rate);
    fprintf(f, "      \"drop_frame\": %s,\n", p->drop_frame ? "true" : "false");
    fprintf(f, "      \"num_frames\": %lu,\n", (unsigned long)p->num_frames);
    fprintf(f, "      \"segments\": ");
    write_segments(f, p);
    fprintf(f, "\n    }%s\n", last ? "" : ",");
}

static int write_json_file(const char *path, const PlaneSet *s,
                           const OfsPlane *planes, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        pf_warn("cannot create '%s'", path);
        return -1;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"format\": \"%s\",\n", PJSON_FORMAT);
    fprintf(f, "  \"version\": %d,\n", PJSON_VERSION);
    fprintf(f, "  \"frame_rate\": %d,\n", s->frame_rate);
    fprintf(f, "  \"drop_frame\": %s,\n", s->drop_frame ? "true" : "false");
    fprintf(f, "  \"num_frames\": %lu,\n", (unsigned long)s->num_frames);
    fprintf(f, "  \"planes_details\": {\n");
    for (int i = 0; i < n; i++)
        write_plane(f, &planes[i], i == n - 1);
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    if (fclose(f) != 0) {
        pf_warn("failed writing '%s'", path);
        return -1;
    }
    return 0;
}

int pjson_write(const char *path, const PlaneSet *s, int combined)
{
    if (combined) {
        if (s->n == 0)
            return -1;
        pf_ensure_parent_dir(path);
        return write_json_file(path, s, s->planes, s->n);
    }

    pf_ensure_dir(path);
    int written = 0;
    for (int i = 0; i < s->n; i++) {
        char name[64], full[4096];
        snprintf(name, sizeof(name), JSON_PLANE_NAME, s->planes[i].plane_id);
        pf_join_path(full, sizeof(full), path, name);

        PlaneSet one;
        one.planes = &s->planes[i];
        one.n = 1;
        one.frame_rate = s->frame_rate;
        one.drop_frame = s->drop_frame;
        one.num_frames = s->planes[i].num_frames;
        if (write_json_file(full, &one, &s->planes[i], 1) == 0)
            written++;
    }
    return written > 0 ? 0 : -1;
}

/* ── reading ───────────────────────────────────────────────────────── */
static void planeset_add(PlaneSet *s, const OfsPlane *p)
{
    s->planes = (OfsPlane *)realloc(s->planes, sizeof(OfsPlane) * (size_t)(s->n + 1));
    if (!s->planes) pf_fatal("out of memory");
    s->planes[s->n++] = *p;
}

/* Read the three fields of one segment.  Returns 0 on success. */
static int segment_fields(const Jv *sv, long long *start, long long *end,
                          int *depth, int plane_id, int index, const char *path)
{
    if (sv->t != JV_OBJ) {
        pf_warn("'%s': plane %d segment %d is not an object", path, plane_id, index + 1);
        return -1;
    }
    Jv *sf = jget(sv, "start_frame"), *ef = jget(sv, "end_frame"),
       *dp = jget(sv, "depth");
    if (!sf || sf->t != JV_NUM || !ef || ef->t != JV_NUM ||
        !dp || dp->t != JV_NUM) {
        pf_warn("'%s': plane %d segment %d is missing start_frame, end_frame "
                "or depth", path, plane_id, index + 1);
        return -1;
    }
    if (dp->num < -MAX_OFFSET || dp->num > MAX_OFFSET) {
        pf_warn("'%s': plane %d segment %d has depth %lld (must be %d..%d)",
                path, plane_id, index + 1, dp->num, -MAX_OFFSET, MAX_OFFSET);
        return -1;
    }
    if (ef->num < sf->num) {
        pf_warn("'%s': plane %d segment %d ends before it starts",
                path, plane_id, index + 1);
        return -1;
    }
    *start = sf->num;
    *end = ef->num;
    *depth = (int)dp->num;
    return 0;
}

/* Parse the "planes_details" object of one file into the set. */
static int parse_planes(PlaneSet *s, const Jv *root, const char *path)
{
    Jv *pd = jget(root, "planes_details");
    if (!pd || pd->t != JV_OBJ) {
        pf_warn("'%s' has no 'planes_details' object", path);
        return -1;
    }

    int file_fr = -1, file_df = 0;
    long long file_nf = -1;
    Jv *fr = jget(root, "frame_rate");
    if (fr && fr->t == JV_NUM) file_fr = (int)fr->num;
    Jv *df = jget(root, "drop_frame");
    if (df && df->t == JV_BOOL) file_df = df->b;
    Jv *nf = jget(root, "num_frames");
    if (nf && nf->t == JV_NUM && nf->num >= 0) file_nf = nf->num;

    if (s->frame_rate < 0 && file_fr >= 0)
        s->frame_rate = file_fr;
    if (s->n == 0)
        s->drop_frame = file_df;

    for (int i = 0; i < pd->on; i++) {
        const Jv *po = pd->vals[i];
        if (po->t != JV_OBJ) {
            pf_warn("'%s': plane entry '%s' is not an object", path, pd->keys[i]);
            return -1;
        }
        OfsPlane p;
        memset(&p, 0, sizeof(p));

        Jv *pid = jget(po, "plane_id");
        if (pid && pid->t == JV_NUM) {
            p.plane_id = (int)pid->num;
        } else {
            char *end = NULL;
            long v = strtol(pd->keys[i], &end, 10);
            if (end == pd->keys[i] || *end != '\0') {
                pf_warn("'%s': plane entry '%s' has no usable plane id", path, pd->keys[i]);
                return -1;
            }
            p.plane_id = (int)v;
        }
        if (p.plane_id < 0 || p.plane_id >= MAX_PLANES) {
            pf_warn("'%s': plane id %d is out of range 0..%d",
                    path, p.plane_id, MAX_PLANES - 1);
            return -1;
        }
        p.frame_rate = file_fr >= 0 ? file_fr : s->frame_rate;
        p.drop_frame = file_df;

        Jv *pfr = jget(po, "frame_rate");
        if (pfr && pfr->t == JV_NUM && pfr->num >= 1) p.frame_rate = (int)pfr->num;
        Jv *pdf = jget(po, "drop_frame");
        if (pdf && pdf->t == JV_BOOL) p.drop_frame = pdf->b;
        if (p.frame_rate < 1) {
            pf_warn("'%s': plane %d has no frame rate; using 1", path, p.plane_id);
            p.frame_rate = 1;
        }

        Jv *g = jget(po, "guid");
        if (g && g->t == JV_STR) {
            if (strlen(g->str) != 32 || read_hex(g->str, p.guid, 16) != 0) {
                pf_warn("'%s': plane %d has a malformed guid", path, p.plane_id);
                return -1;
            }
            p.guid[15] = (uint8_t)p.plane_id;
        } else {
            pf_make_guid(p.guid, p.plane_id);
        }
        Jv *stc = jget(po, "start_timecode");
        if (stc && stc->t == JV_NUM && stc->num >= 0)
            p.start_timecode = (uint32_t)stc->num;
        Jv *res = jget(po, "reserved");
        if (res && res->t == JV_STR) {
            if (strlen(res->str) != 8 || read_hex(res->str, p.reserved, 4) != 0) {
                pf_warn("'%s': plane %d has malformed reserved bytes", path, p.plane_id);
                return -1;
            }
        }

        long long frames = -1;
        Jv *pnf = jget(po, "num_frames");
        if (pnf && pnf->t == JV_NUM && pnf->num >= 0) frames = pnf->num;
        else if (file_nf >= 0) frames = file_nf;

        Jv *segs = jget(po, "segments");
        if (!segs || segs->t != JV_ARR) {
            pf_warn("'%s': plane %d has no 'segments' array", path, p.plane_id);
            return -1;
        }

        /* The segments must cover the plane's frames without a gap. */
        long long coverage = 0;
        for (int sgi = 0; sgi < segs->an; sgi++) {
            long long sf, ef;
            int dp;
            if (segment_fields(segs->arr[sgi], &sf, &ef, &dp, p.plane_id, sgi, path) != 0)
                return -1;
            if (sf != coverage) {
                pf_warn("'%s': plane %d segment %d starts at frame %lld but %lld "
                        "was expected", path, p.plane_id, sgi + 1, sf, coverage);
                return -1;
            }
            coverage = ef + 1;
        }
        if (frames < 0) {
            frames = coverage;
        } else if (frames != coverage) {
            pf_warn("'%s': plane %d covers %lld of %lld frame(s)",
                    path, p.plane_id, coverage, frames);
            return -1;
        }
        if (frames > 0xffffffffLL) {
            pf_warn("'%s': plane %d has too many frames", path, p.plane_id);
            return -1;
        }
        p.num_frames = (uint32_t)frames;

        p.offsets = (uint8_t *)malloc(p.num_frames ? p.num_frames : 1);
        if (!p.offsets) pf_fatal("out of memory");
        for (int sgi = 0; sgi < segs->an; sgi++) {
            long long sf, ef;
            int dp;
            if (segment_fields(segs->arr[sgi], &sf, &ef, &dp, p.plane_id, sgi, path) != 0) {
                free(p.offsets);
                return -1;
            }
            uint8_t b = pf_depth_to_byte(dp);
            for (long long f = sf; f <= ef; f++)
                p.offsets[f] = b;
        }

        if (s->num_frames == 0)
            s->num_frames = p.num_frames;
        planeset_add(s, &p);
    }
    return 0;
}

static int read_json_file(const char *path, PlaneSet *s)
{
    size_t len = 0;
    char *txt = pf_read_file(path, &len);
    const char *p = txt;
    if (len >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

    char err[160];
    Jv *root = json_parse(p, err, sizeof(err));
    if (!root || err[0]) {
        json_free(root);
        free(txt);
        pf_warn("'%s': bad JSON: %s", path, err[0] ? err : "parse error");
        return -1;
    }
    if (root->t != JV_OBJ) {
        json_free(root);
        free(txt);
        pf_warn("'%s': top level must be an object", path);
        return -1;
    }

    Jv *fmt = jget(root, "format");
    if (fmt && fmt->t == JV_STR && strcmp(fmt->str, PJSON_FORMAT) != 0) {
        json_free(root);
        free(txt);
        pf_warn("'%s' is not a %s file", path, PJSON_FORMAT);
        return -1;
    }

    int rc = parse_planes(s, root, path);
    json_free(root);
    free(txt);
    return rc;
}

int pjson_read(const char *path, PlaneSet *out)
{
    memset(out, 0, sizeof(*out));
    out->frame_rate = -1;

    if (pf_is_dir(path)) {
        for (int p = 0; p < MAX_PLANES; p++) {
            char name[64], full[4096];
            snprintf(name, sizeof(name), JSON_PLANE_NAME, p);
            pf_join_path(full, sizeof(full), path, name);
            FILE *t = fopen(full, "rb");
            if (!t) continue;
            fclose(t);
            if (read_json_file(full, out) != 0) {
                planeset_free(out);
                return -1;
            }
        }
    } else {
        if (read_json_file(path, out) != 0) {
            planeset_free(out);
            return -1;
        }
    }

    if (out->n == 0) {
        planeset_free(out);
        return -1;
    }
    return 0;
}

void planeset_free(PlaneSet *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++)
        ofs_free(&s->planes[i]);
    free(s->planes);
    s->planes = NULL;
    s->n = 0;
}
