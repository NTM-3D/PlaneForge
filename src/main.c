/* PlaneForge command line interface. */
#include "pf.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

/* ═══════════════════════════════════════════════════════════════════════
 * Options
 * ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    const char *input;
    const char *output;
    const char *report;
    const char *base_view;
    const char *batch_job;
    const char *export_ofs;
    const char *from_ofs;
    const char *export_json;
    const char *from_json;
    int ofs_to_json;
    int json_to_ofs;
    const char *plane_spec;

    int analyze, dump_sei, show_empty, force, dropframe, help, version, quiet;

    int have_plane,   plane, plane_is_list;
    int have_depth,   depth;
    int have_min,     min_depth;
    int have_max,     max_depth;
    int have_interval, interval;
    int have_fps,     fps;
    int have_override, override_depth;
} Options;

static const char *USAGE =
PF_NAME " v" PF_VERSION "\n"
"NTM-3D\n"
"\n"
"usage: planeforge [options] <input> [output]\n"
"\n"
"Read, analyse and write fixed-depth or sweeping 3D-planes (offset-metadata)\n"
"inside raw MVC streams, and exchange that metadata with OFS 3D-Plane files\n"
"or with plane JSON files.  The same options serve both formats.\n"
"\n"
"analysis:\n"
"  -analyze                 print an OFMD report (JSON)\n"
"  -dump-sei                dump the first OFMD SEI NAL\n"
"  -report FILE             write the analysis report to FILE\n"
"  -show-empty              include empty planes in the report\n"
"\n"
"writing:\n"
"  -base-view FILE          base-view AVC stream (needed if input has no OFMD)\n"
"  -plane N                 plane index 0..31\n"
"  -depth N                 fixed depth -127..127 (0 is not valid)\n"
"  -min-depth N             sweep minimum depth -127..127 (0 is not valid)\n"
"  -max-depth N             sweep maximum depth -127..127 (0 is not valid)\n"
"  -interval N              sweep hold interval (>0; 1 updates every frame)\n"
"  -batch-job FILE          JSON batch job (array of entries)\n"
"  -force                   overwrite the output file; also allow depth 0\n"
"\n"
"export:\n"
"  -export-ofs DIR          export planes to 3D-Plane-NN.ofs files in DIR\n"
"  -export-json PATH        export planes to plane JSON; a .json file holds\n"
"                           every plane, a directory holds one file per plane\n"
"  -plane N                 with export, write only plane N\n"
"  -fps N                   frame rate for export: 1..4, 6 or 7\n"
"  -dropframe               mark exported planes as drop-frame (fps 4 only)\n"
"\n"
"import:\n"
"  -from-ofs PATH           import OFS file or directory into the MVC output\n"
"  -from-json PATH          import plane JSON into the MVC output\n"
"  -plane N|A-B|A,B,...     with import, choose the target plane(s)\n"
"  -override-depth N        depth -127..127 used to resolve frame mismatches\n"
"\n"
"conversion:\n"
"  -ofs-to-json             convert the input OFS file or directory to plane JSON\n"
"  -json-to-ofs             convert the input plane JSON to OFS file or directory\n"
"\n"
"misc:\n"
"  -quiet                   suppress progress output\n"
"  -h, -help                show this help\n"
"  -V, -version             show version\n";

/* ═══════════════════════════════════════════════════════════════════════
 * small helpers
 * ═══════════════════════════════════════════════════════════════════════ */
static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static long parse_int(const char *s, const char *what)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        pf_fatal("invalid integer for %s: '%s'", what, s);
    return v;
}

static int is_plain_int(const char *s)
{
    if (*s == '+' || *s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* Parse a plane specification: a single index, a range "A-B", a list
 * "A,B,C", or any combination such as "0-3,7".  Returns the number of
 * plane indices produced. */
static int parse_plane_spec(const char *spec, int *out, int max)
{
    int n = 0;
    const char *s = spec;
    if (!*s)
        pf_fatal("empty plane list");
    while (*s) {
        char *end = NULL;
        errno = 0;
        long a = strtol(s, &end, 10);
        if (errno != 0 || end == s)
            pf_fatal("invalid plane list: '%s'", spec);
        s = end;
        long b = a;
        if (*s == '-') {
            s++;
            errno = 0;
            b = strtol(s, &end, 10);
            if (errno != 0 || end == s)
                pf_fatal("invalid plane range: '%s'", spec);
            s = end;
        }
        if (a > b) { long t = a; a = b; b = t; }
        if (a < 0 || b >= MAX_PLANES)
            pf_fatal("plane %ld is out of range 0..%d", a < 0 ? a : b, MAX_PLANES - 1);
        for (long v = a; v <= b; v++) {
            if (n >= max)
                pf_fatal("plane list has more than %d entries", max);
            out[n++] = (int)v;
        }
        if (*s == ',') { s++; continue; }
        if (*s == '\0') break;
        pf_fatal("invalid plane list: '%s'", spec);
    }
    return n;
}

static void jstr(FILE *o, const char *s)
{
    fputc('"', o);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  fputs("\\\"", o); break;
        case '\\': fputs("\\\\", o); break;
        case '\n': fputs("\\n", o); break;
        case '\r': fputs("\\r", o); break;
        case '\t': fputs("\\t", o); break;
        case '\b': fputs("\\b", o); break;
        case '\f': fputs("\\f", o); break;
        default:
            if (c < 0x20) fprintf(o, "\\u%04x", c);
            else          fputc(c, o);
        }
    }
    fputc('"', o);
}

/* ═══════════════════════════════════════════════════════════════════════
 * analyze
 * ═══════════════════════════════════════════════════════════════════════ */
static void dump_sei(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) pf_fatal("cannot open '%s'", path);
    PfReader r;
    pf_reader_init(&r, fp);
    int found = 0;
    while (pf_reader_next(&r) == PF_NAL) {
        if (r.nal_type != NAL_SEI) continue;
        size_t rlen = 0;
        uint8_t *rbsp = ebsp_to_rbsp(r.nal + 1, r.nal_len - 1, &rlen);
        long idx = find_ofmd(rbsp, rlen);
        if (idx < 0) { free(rbsp); continue; }
        uint8_t hb = r.nal_len ? r.nal[0] : 0;
        printf("NAL header byte: 0x%02x  (nal_ref_idc=%d, type=%d)\n",
               hb, (hb >> 5) & 3, hb & 0x1f);
        printf("SEI NAL RBSP: %lu bytes, OFMD at offset %ld\n",
               (unsigned long)rlen, idx);
        size_t end = (size_t)idx + 20;
        if (end > rlen) end = rlen;
        for (size_t i = 0; i < end; i += 16) {
            printf("  %4lu: ", (unsigned long)i);
            for (size_t k = 0; k < 16 && i + k < end; k++) {
                if (k) putchar(' ');
                printf("%02x", rbsp[i + k]);
            }
            putchar('\n');
        }
        free(rbsp);
        found = 1;
        break;
    }
    pf_reader_free(&r);
    fclose(fp);
    if (!found)
        printf("No SEI NAL with OFMD found.\n");
}

static const char *nal_name(int nt)
{
    switch (nt) {
    case 1:  return "non-IDR slice";
    case 5:  return "IDR slice";
    case 6:  return "SEI";
    case 7:  return "SPS";
    case 8:  return "PPS";
    case 9:  return "AUD";
    case 14: return "prefix";
    case 15: return "subset SPS";
    case 20: return "MVC ext slice";
    case 24: return "type-24 (frame marker)";
    default: return "?";
    }
}

static int cmd_analyze(const Options *o)
{
    if (o->dump_sei) { dump_sei(o->input); return 0; }

    ScanResult sr;
    pf_scan(o->input, &sr);

    if (sr.ofmd_messages > 0) {
        FILE *out = stdout;
        if (o->report) {
            out = fopen(o->report, "w");
            if (!out) pf_fatal("cannot write '%s'", o->report);
        }
        fprintf(out, "{\n");
        fprintf(out, "  \"input\": ");
        jstr(out, o->input);
        fprintf(out, ",\n");
        fprintf(out, "  \"ofmd_messages\": %lld,\n", sr.ofmd_messages);
        fprintf(out, "  \"planes\": %d,\n", sr.planes);
        fprintf(out, "  \"plane_details\": {");
        int first = 1;
        for (int p = 0; p < sr.planes; p++) {
            PlaneStat *st = &sr.ps[p];
            int mn = st->have_minmax ? st->min : 0;
            int mx = st->have_minmax ? st->max : 0;
            const char *type = (mn == 0 && mx == 0) ? "empty"
                             : (mn == mx) ? "static" : "dynamic";
            if (!o->show_empty && strcmp(type, "empty") == 0)
                continue;
            if (!first) fprintf(out, ",");
            first = 0;
            fprintf(out, "\n    \"%d\": {\n", p);
            fprintf(out, "      \"type\": \"%s\",\n", type);
            fprintf(out, "      \"frames\": %lld,\n", st->frames);
            fprintf(out, "      \"min\": %d,\n", mn);
            fprintf(out, "      \"max\": %d,\n", mx);
            fprintf(out, "      \"changes\": %lld\n", st->changes);
            fprintf(out, "    }");
        }
        if (!first) fprintf(out, "\n  ");
        fprintf(out, "}\n}\n");
        if (o->report) {
            fclose(out);
            printf("Wrote %s\n", o->report);
        }
    } else {
        printf("No OFMD blocks found.  %lld NAL units total.\n", sr.nal_total);
        for (int nt = 0; nt < 32; nt++) {
            if (sr.nal_counts[nt] == 0) continue;
            printf("  type %2d (%20s): %lld\n",
                   nt, nal_name(nt), sr.nal_counts[nt]);
        }
        printf("\nTo insert new offset-metadata, re-run with -base-view <base.avc>\n");
    }
    scan_result_free(&sr);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * set / sweep
 * ═══════════════════════════════════════════════════════════════════════ */
static int cmd_set(const Options *o)
{
    if (o->plane_is_list)
        pf_fatal("-plane must be a single index for -depth");
    if (o->plane < 0 || o->plane >= MAX_PLANES)
        pf_fatal("plane must be 0..%d", MAX_PLANES - 1);
    if (o->depth < -MAX_OFFSET || o->depth > MAX_OFFSET)
        pf_fatal("depth must be -%d..%d", MAX_OFFSET, MAX_OFFSET);
    int depth = pf_depth_value(o->depth, o->force, "-depth");
    if (file_exists(o->output) && !o->force)
        pf_fatal("output exists: %s (use -force)", o->output);

    PlaneSource *sources[MAX_PLANES];
    memset(sources, 0, sizeof(sources));
    sources[o->plane] = src_static(depth);

    WriteStats st;
    memset(&st, 0, sizeof(st));
    pf_write_stream(o->input, o->output, sources, &st, o->base_view);
    src_free(sources[o->plane]);

    printf("{\n");
    printf("  \"output\": ");
    jstr(stdout, o->output);
    printf(",\n");
    printf("  \"plane\": %d,\n", o->plane);
    printf("  \"depth\": %d,\n", depth);
    printf("  \"ofmd_modified\": %lld,\n", st.messages_modified);
    printf("  \"gops_injected\": %lld,\n", st.injected_gops);
    printf("  \"created\": %s\n", st.planes_created > 0 ? "true" : "false");
    printf("}\n");
    return 0;
}

static int cmd_sweep(const Options *o)
{
    if (o->plane_is_list)
        pf_fatal("-plane must be a single index for a sweep");
    if (o->plane < 0 || o->plane >= MAX_PLANES)
        pf_fatal("plane must be 0..%d", MAX_PLANES - 1);
    if (o->min_depth < -MAX_OFFSET || o->min_depth > MAX_OFFSET ||
        o->max_depth < -MAX_OFFSET || o->max_depth > MAX_OFFSET)
        pf_fatal("depths must be -%d..%d", MAX_OFFSET, MAX_OFFSET);
    int min_depth = pf_depth_value(o->min_depth, o->force, "-min-depth");
    int max_depth = pf_depth_value(o->max_depth, o->force, "-max-depth");
    if (min_depth > max_depth)
        pf_fatal("min depth %d is above max depth %d", min_depth, max_depth);
    if (o->interval <= 0)
        pf_fatal("interval must be > 0");
    if (file_exists(o->output) && !o->force)
        pf_fatal("output exists: %s (use -force)", o->output);

    ScanResult sr;
    pf_scan(o->input, &sr);
    long long tf;
    if (sr.ofmd_messages > 0) {
        tf = sr.total_frames;
    } else {
        if (!o->base_view) {
            scan_result_free(&sr);
            pf_fatal("no OFMD in input; supply -base-view");
        }
        tf = pf_base_view_frames(o->base_view);
    }

    PlaneSource *sources[MAX_PLANES];
    memset(sources, 0, sizeof(sources));
    sources[o->plane] = src_sweep(min_depth, max_depth, o->interval);

    WriteStats st;
    memset(&st, 0, sizeof(st));
    pf_write_stream_pre(o->input, o->output, sources, &st, o->base_view, &sr);
    src_free(sources[o->plane]);
    scan_result_free(&sr);

    printf("{\n");
    printf("  \"output\": ");
    jstr(stdout, o->output);
    printf(",\n");
    printf("  \"plane\": %d,\n", o->plane);
    printf("  \"mode\": \"sweep\",\n");
    printf("  \"min_depth\": %d,\n", min_depth);
    printf("  \"max_depth\": %d,\n", max_depth);
    printf("  \"interval\": %d,\n", o->interval);
    printf("  \"total_frames\": %lld,\n", tf);
    printf("  \"ofmd_modified\": %lld,\n", st.messages_modified);
    printf("  \"gops_injected\": %lld,\n", st.injected_gops);
    printf("  \"created\": %s\n", st.planes_created > 0 ? "true" : "false");
    printf("}\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * batch
 * ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    int plane;
    int mode;      /* 0 static, 1 sweep */
    int depth;
    int min, max, interval;
} BatchEnt;

static int parse_batch(const char *path, BatchEnt *ents, int *out_n, int force)
{
    size_t len = 0;
    char *txt = pf_read_file(path, &len);
    const char *p = txt;
    if (len >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

    char err[128];
    Jv *root = json_parse(p, err, sizeof(err));
    if (!root || err[0]) {
        json_free(root);
        pf_fatal("batch job error: bad JSON: %s", err[0] ? err : "parse error");
    }
    if (root->t != JV_ARR)
        pf_fatal("batch job error: top-level must be an array");
    if (root->an == 0)
        pf_fatal("batch job error: need >= 1 entry");
    if (root->an > MAX_PLANES)
        pf_fatal("batch job error: max %d entries", MAX_PLANES);

    int seen[MAX_PLANES];
    memset(seen, 0, sizeof(seen));
    int n = 0;
    for (int i = 0; i < root->an; i++) {
        Jv *e = root->arr[i];
        if (e->t != JV_OBJ)
            pf_fatal("batch job error: entry %d is not an object", i + 1);
        Jv *t = jget(e, "type");
        if (!t || t->t != JV_STR)
            pf_fatal("batch job error: entry %d missing 'type'", i + 1);

        BatchEnt *be = &ents[n];
        memset(be, 0, sizeof(*be));

        if (strcmp(t->str, "static") == 0) {
            if (e->on != 3)
                pf_fatal("batch job error: entry %d must have keys type, plane, depth", i + 1);
            Jv *pl = jget(e, "plane"), *dp = jget(e, "depth");
            if (!pl || pl->t != JV_NUM) pf_fatal("batch job error: entry %d bad 'plane'", i + 1);
            if (!dp || dp->t != JV_NUM) pf_fatal("batch job error: entry %d bad 'depth'", i + 1);
            if (pl->num < 0 || pl->num >= MAX_PLANES) pf_fatal("batch job error: entry %d plane out of range", i + 1);
            if (dp->num < -MAX_OFFSET || dp->num > MAX_OFFSET) pf_fatal("batch job error: entry %d depth out of range", i + 1);
            be->mode = 0;
            be->plane = (int)pl->num;
            be->depth = pf_depth_value((int)dp->num, force, "batch job depth");
        } else if (strcmp(t->str, "sweep") == 0) {
            if (e->on != 5)
                pf_fatal("batch job error: entry %d must have keys type, plane, min-depth, max-depth, interval", i + 1);
            Jv *pl = jget(e, "plane"), *mn = jget(e, "min-depth"),
               *mx = jget(e, "max-depth"), *iv = jget(e, "interval");
            if (!pl || pl->t != JV_NUM) pf_fatal("batch job error: entry %d bad 'plane'", i + 1);
            if (!mn || mn->t != JV_NUM) pf_fatal("batch job error: entry %d bad 'min-depth'", i + 1);
            if (!mx || mx->t != JV_NUM) pf_fatal("batch job error: entry %d bad 'max-depth'", i + 1);
            if (!iv || iv->t != JV_NUM) pf_fatal("batch job error: entry %d bad 'interval'", i + 1);
            if (pl->num < 0 || pl->num >= MAX_PLANES) pf_fatal("batch job error: entry %d plane out of range", i + 1);
            if (mn->num < -MAX_OFFSET || mn->num > MAX_OFFSET ||
                mx->num < -MAX_OFFSET || mx->num > MAX_OFFSET)
                pf_fatal("batch job error: entry %d depth out of range", i + 1);
            if (iv->num < 1) pf_fatal("batch job error: entry %d interval < 1", i + 1);
            be->mode = 1;
            be->plane = (int)pl->num;
            be->min = pf_depth_value((int)mn->num, force, "batch job min-depth");
            be->max = pf_depth_value((int)mx->num, force, "batch job max-depth");
            be->interval = (int)iv->num;
        } else {
            pf_fatal("batch job error: entry %d bad type '%s'", i + 1, t->str);
        }

        if (seen[be->plane])
            pf_fatal("batch job error: entry %d duplicate plane %d", i + 1, be->plane);
        seen[be->plane] = 1;
        n++;
    }

    json_free(root);
    free(txt);
    *out_n = n;
    return 0;
}

static int cmd_batch(const Options *o)
{
    BatchEnt ents[MAX_PLANES];
    int n = 0;
    parse_batch(o->batch_job, ents, &n, o->force);

    if (file_exists(o->output) && !o->force)
        pf_fatal("output exists: %s (use -force)", o->output);

    ScanResult sr;
    pf_scan(o->input, &sr);
    if (sr.ofmd_messages == 0 && !o->base_view) {
        scan_result_free(&sr);
        pf_fatal("no OFMD in input; supply -base-view");
    }

    PlaneSource *sources[MAX_PLANES];
    memset(sources, 0, sizeof(sources));
    int planes[MAX_PLANES];
    for (int i = 0; i < n; i++) {
        sources[ents[i].plane] = ents[i].mode == 0
            ? src_static(ents[i].depth)
            : src_sweep(ents[i].min, ents[i].max, ents[i].interval);
        planes[i] = ents[i].plane;
    }

    WriteStats st;
    memset(&st, 0, sizeof(st));
    pf_write_stream_pre(o->input, o->output, sources, &st, o->base_view, &sr);
    scan_result_free(&sr);

    for (int i = 0; i < n; i++)
        src_free(sources[ents[i].plane]);

    /* bubble sort plane list */
    for (int i = 0; i < n; i++)
        for (int k = i + 1; k < n; k++)
            if (planes[k] < planes[i]) { int t = planes[i]; planes[i] = planes[k]; planes[k] = t; }

    printf("{\n");
    printf("  \"output\": ");
    jstr(stdout, o->output);
    printf(",\n");
    printf("  \"mode\": \"batch\",\n");
    printf("  \"entries\": %d,\n", n);
    printf("  \"planes\": [");
    for (int i = 0; i < n; i++) {
        if (i) printf(", ");
        printf("%d", planes[i]);
    }
    printf("],\n");
    printf("  \"ofmd_modified\": %lld,\n", st.messages_modified);
    printf("  \"gops_injected\": %lld,\n", st.injected_gops);
    printf("  \"created\": %s\n", st.planes_created > 0 ? "true" : "false");
    printf("}\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * argument parsing / main
 * ═══════════════════════════════════════════════════════════════════════ */
static int takes_value(const char *name)
{
    while (*name == '-') name++;
    static const char *with_val[] = {
        "report", "base-view", "batch-job", "plane", "depth",
        "min-depth", "max-depth", "interval", "export-ofs", "fps",
        "from-ofs", "override-depth", "export-json", "from-json", NULL
    };
    for (int i = 0; with_val[i]; i++)
        if (strcmp(name, with_val[i]) == 0) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fputs(USAGE, stdout);
        return 0;
    }

    Options o;
    memset(&o, 0, sizeof(o));
    o.depth = o.min_depth = o.max_depth = o.override_depth = 0;

    const char *pos[4];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            /* An option token; leading dashes are not significant. */
            const char *nm = a;
            while (*nm == '-') nm++;
            if (*nm == '\0') {                 /* bare "-" */
                if (npos >= 4) pf_fatal("too many positional arguments");
                pos[npos++] = a;
                continue;
            }
            char name[64];
            const char *val = NULL;
            char *eq = strchr(nm, '=');
            if (eq) {
                size_t l = (size_t)(eq - nm);
                if (l >= sizeof(name)) pf_fatal("unknown option: %s", a);
                memcpy(name, nm, l); name[l] = '\0';
                val = eq + 1;
            } else {
                snprintf(name, sizeof(name), "%s", nm);
            }
            if (takes_value(name) && !val) {
                if (i + 1 >= argc) pf_fatal("option -%s requires a value", name);
                val = argv[++i];
            }

            if (strcmp(name, "analyze") == 0)          o.analyze = 1;
            else if (strcmp(name, "dump-sei") == 0)    o.dump_sei = 1;
            else if (strcmp(name, "show-empty") == 0)  o.show_empty = 1;
            else if (strcmp(name, "force") == 0)       o.force = 1;
            else if (strcmp(name, "dropframe") == 0)   o.dropframe = 1;
            else if (strcmp(name, "quiet") == 0)       o.quiet = 1;
            else if (strcmp(name, "h") == 0 || strcmp(name, "help") == 0)     o.help = 1;
            else if (strcmp(name, "V") == 0 || strcmp(name, "version") == 0)  o.version = 1;
            else if (strcmp(name, "report") == 0)      o.report = val;
            else if (strcmp(name, "base-view") == 0)   o.base_view = val;
            else if (strcmp(name, "batch-job") == 0)   o.batch_job = val;
            else if (strcmp(name, "export-ofs") == 0)  o.export_ofs = val;
            else if (strcmp(name, "from-ofs") == 0)    o.from_ofs = val;
            else if (strcmp(name, "export-json") == 0) o.export_json = val;
            else if (strcmp(name, "from-json") == 0)   o.from_json = val;
            else if (strcmp(name, "ofs-to-json") == 0) o.ofs_to_json = 1;
            else if (strcmp(name, "json-to-ofs") == 0) o.json_to_ofs = 1;
            else if (strcmp(name, "plane") == 0) {
                o.plane_spec = val; o.have_plane = 1;
                if (is_plain_int(val)) o.plane = (int)parse_int(val, "-plane");
                else                   o.plane_is_list = 1;
            }
            else if (strcmp(name, "depth") == 0)       { o.depth = (int)parse_int(val, "-depth"); o.have_depth = 1; }
            else if (strcmp(name, "min-depth") == 0)   { o.min_depth = (int)parse_int(val, "-min-depth"); o.have_min = 1; }
            else if (strcmp(name, "max-depth") == 0)   { o.max_depth = (int)parse_int(val, "-max-depth"); o.have_max = 1; }
            else if (strcmp(name, "interval") == 0)    { o.interval = (int)parse_int(val, "-interval"); o.have_interval = 1; }
            else if (strcmp(name, "fps") == 0)         { o.fps = (int)parse_int(val, "-fps"); o.have_fps = 1; }
            else if (strcmp(name, "override-depth") == 0) { o.override_depth = (int)parse_int(val, "-override-depth"); o.have_override = 1; }
            else pf_fatal("unknown option: %s", a);
        } else {
            if (npos >= 4) pf_fatal("too many positional arguments");
            pos[npos++] = a;
        }
    }

    pf_quiet = o.quiet;

    if (o.version) { printf("%s v%s\n", PF_NAME, PF_VERSION); return 0; }
    if (o.help)    { fputs(USAGE, stdout); return 0; }

    if (npos >= 1) o.input  = pos[0];
    if (npos >= 2) o.output = pos[1];

    if (!o.input)
        pf_fatal("no input file given (try -help)");

    if (o.analyze || o.dump_sei)
        return cmd_analyze(&o);

    if (o.export_ofs) {
        if (o.plane_is_list)
            pf_fatal("-plane for -export-ofs must be a single index");
        if (o.have_fps && o.fps != 1 && o.fps != 2 && o.fps != 3 &&
            o.fps != 4 && o.fps != 6 && o.fps != 7)
            pf_fatal("-fps must be 1, 2, 3, 4, 6 or 7");
        if (o.have_override)
            pf_fatal("-override-depth is only valid with -from-ofs");
        return cmd_export(o.input, o.export_ofs,
                          o.have_fps ? o.fps : 0, o.have_fps, o.dropframe,
                          o.have_plane, o.plane, o.force);
    }

    if (o.export_json) {
        if (o.plane_is_list)
            pf_fatal("-plane for -export-json must be a single index");
        if (o.have_fps && o.fps != 1 && o.fps != 2 && o.fps != 3 &&
            o.fps != 4 && o.fps != 6 && o.fps != 7)
            pf_fatal("-fps must be 1, 2, 3, 4, 6 or 7");
        if (o.have_override)
            pf_fatal("-override-depth is only valid with -from-ofs or -from-json");
        return cmd_export_json(o.input, o.export_json,
                               o.have_fps ? o.fps : 0, o.have_fps, o.dropframe,
                               o.have_plane, o.plane, o.force);
    }

    if (o.ofs_to_json) {
        if (o.have_plane || o.have_fps || o.dropframe || o.have_override)
            pf_fatal("-ofs-to-json takes no plane, fps or depth options");
        if (!o.output)
            pf_fatal("output path required with -ofs-to-json");
        return cmd_ofs_to_json(o.input, o.output, o.force);
    }

    if (o.json_to_ofs) {
        if (o.have_plane || o.have_fps || o.dropframe || o.have_override)
            pf_fatal("-json-to-ofs takes no plane, fps or depth options");
        if (!o.output)
            pf_fatal("output path required with -json-to-ofs");
        return cmd_json_to_ofs(o.input, o.output, o.force);
    }

    if (o.from_ofs || o.from_json) {
        if (!o.output)
            pf_fatal("output file required with -from-ofs/-from-json");
        if (o.have_override &&
            (o.override_depth < -MAX_OFFSET || o.override_depth > MAX_OFFSET))
            pf_fatal("-override-depth must be -%d..%d", MAX_OFFSET, MAX_OFFSET);
        if (o.have_fps || o.dropframe)
            pf_fatal("-fps/-dropframe apply to export only");
        if (o.have_depth || o.have_min || o.have_max || o.have_interval ||
            o.batch_job)
            pf_fatal("import cannot be combined with depth/sweep/batch options");

        int targets[MAX_PLANES];
        int ntargets = 0;
        if (o.have_plane)
            ntargets = parse_plane_spec(o.plane_spec, targets, MAX_PLANES);

        if (o.from_json)
            return cmd_import_json(o.input, o.output, o.from_json,
                                   o.have_override, o.override_depth,
                                   o.base_view, targets, ntargets, o.force);
        return cmd_import(o.input, o.output, o.from_ofs,
                          o.have_override, o.override_depth, o.base_view,
                          targets, ntargets, o.force);
    }

    if (!o.output)
        pf_fatal("output file required (try -help)");

    if (o.batch_job) {
        if (o.have_plane || o.have_depth || o.have_min || o.have_max || o.have_interval)
            pf_fatal("-batch-job cannot be combined with manual plane/sweep options");
        return cmd_batch(&o);
    }

    if (o.have_depth) {
        if (!o.have_plane)
            pf_fatal("-plane required with -depth");
        if (o.have_min || o.have_max || o.have_interval)
            pf_fatal("-depth cannot be combined with sweep options");
        return cmd_set(&o);
    }

    if (o.have_min || o.have_max || o.have_interval) {
        if (!o.have_plane)
            pf_fatal("-plane required with sweep options");
        if (!o.have_min || !o.have_max || !o.have_interval)
            pf_fatal("sweep needs -min-depth -max-depth -interval");
        return cmd_sweep(&o);
    }

    pf_fatal("no operation requested: use -batch-job, -depth, sweep options, "
             "-export-ofs or -from-ofs");
    return 0;
}
