/*
 * PlaneForge - 3D-plane (offset-metadata) toolkit for MVC streams.
 *
 * Reads, analyses, edits and creates OFMD offset-metadata inside raw MVC
 * streams and interchanges it with OFS 3D-Plane files.
 */
#ifndef PLANEFORGE_PF_H
#define PLANEFORGE_PF_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#define PF_NAME    "PlaneForge"
#define PF_VERSION "0.1"

/* NAL unit types (H.264 / MVC) */
#define NAL_SLICE  1
#define NAL_IDR    5
#define NAL_SEI    6
#define NAL_TYPE24 24

/* OFMD offset-metadata ------------------------------------------------ */
#define OFMD_MAGIC     "OFMD"
#define OFMD_MAGIC_LEN 4
#define OFMD_HDR       14    /* bytes of the OFMD header (incl. magic)   */
#define MAX_PLANES     32
#define MAX_OFFSET     127

/* OFS 3D-Plane files -------------------------------------------------- */
#define OFS_HEADER   41       /* canonical header size                    */
#define OFS_SIGNATURE { 0x89,'O','F','S',0x0d,0x0a,0x1a,0x0a }

/* ── error helpers ─────────────────────────────────────────────────── */
#if defined(__GNUC__)
#  define PF_NORETURN __attribute__((noreturn))
#else
#  define PF_NORETURN
#endif

void pf_fatal(const char *fmt, ...) PF_NORETURN;
void pf_warn(const char *fmt, ...);

/* ── progress reporting ────────────────────────────────────────────── */
/* Set to suppress progress output. */
extern int pf_quiet;

typedef struct {
    const char *label;
    long long   total;
    long long   start_ms;
    long long   last_ms;
    int         active;
} PfProgress;

void pf_progress_start(PfProgress *p, const char *label, long long total);
void pf_progress_tick(PfProgress *p, long long done);
void pf_progress_end(PfProgress *p);

long long pf_file_size(const char *path);

/* ── streaming Annex-B NAL reader ──────────────────────────────────── */
typedef struct {
    FILE     *fp;
    uint8_t  *buf;
    size_t    cap;
    size_t    len;      /* valid bytes in buf                          */
    size_t    pos;      /* consumed cursor into buf                    */
    int       eof;
    long long bytes_read;  /* total bytes pulled from the file          */

    /* Current entry (valid until the next pf_reader_next call). */
    const uint8_t *gap;     size_t gap_len;   /* bytes before start code */
    const uint8_t *sc;      size_t sc_len;    /* the start code itself   */
    const uint8_t *nal;     size_t nal_len;   /* NAL body incl. hdr byte */
    int            nal_type;
} PfReader;

enum { PF_EOF = 0, PF_NAL = 1, PF_TAIL = 2 };

int  pf_reader_init(PfReader *r, FILE *fp);
int  pf_reader_next(PfReader *r);
void pf_reader_free(PfReader *r);

/* ── RBSP / EBSP emulation-prevention conversion ───────────────────── */
/* Returned buffers are malloc'd; caller frees. */
uint8_t *ebsp_to_rbsp(const uint8_t *e, size_t n, size_t *out_len);
uint8_t *rbsp_to_ebsp(const uint8_t *r, size_t n, size_t *out_len);

/* ── OFMD helpers ──────────────────────────────────────────────────── */
typedef struct {
    int frame_rate;
    int planes;
    int frames;
    int ig;
} OfmdFields;

/* Find "OFMD" magic; requires index > 0 and enough header bytes. */
long find_ofmd(const uint8_t *p, size_t n);
int  ofmd_fields(const uint8_t *o, size_t n, OfmdFields *f);

/* Build a raw OFMD block for n_planes planes / n_frames frames.
 * plane_data is optional (NULL -> all 0x80).  Result malloc'd. */
uint8_t *build_ofmd_block(int n_planes, int n_frames, int frame_rate,
                          const uint8_t *plane_data, size_t plane_data_len,
                          size_t *out_len);

/* Wrap an OFMD block in the nested SEI container used on Blu-ray. */
uint8_t *build_ofmd_sei_nal(const uint8_t *ofmd, size_t ofmd_len,
                            uint8_t nal_hdr_byte, size_t *out_len);

/* ── plane sources (feed depth bytes, one frame at a time) ─────────── */
typedef enum {
    SRC_STATIC,     /* constant value                              */
    SRC_SWEEP,      /* generated sweep                             */
    SRC_OFS         /* OFS offsets then constant pad when exhausted */
} SrcKind;

typedef struct {
    SrcKind kind;
    uint8_t value;          /* STATIC value / OFS pad depth            */
    /* sweep state */
    int lo, hi, interval;
    long long cnt;
    int dir, cur, inited, fixed;
    /* OFS data */
    const uint8_t *data;
    size_t len, pos;
} PlaneSource;

PlaneSource *src_static(int depth);            /* depth -127..127         */
PlaneSource *src_sweep(int lo, int hi, int interval);
PlaneSource *src_ofs(const uint8_t *data, size_t len, int pad_depth);
uint8_t      src_next(PlaneSource *s);
void         src_free(PlaneSource *s);

/* ── analysis ──────────────────────────────────────────────────────── */
typedef struct {
    long long frames;
    long long changes;
    int  have_minmax;
    int  min, max;          /* signed depth                            */
    int  have_last;
    int  last;
} PlaneStat;

typedef struct {
    int          has_ofmd;
    long long    ofmd_messages;
    int          planes;    /* max planes seen                         */
    int          frame_rate;
    long long    total_frames;
    PlaneStat   *ps;        /* [planes]                                */
    long long    nal_counts[32];
    long long    nal_total;
    int          first_sei_hdr;   /* -1 if none                       */
} ScanResult;

void scan_result_free(ScanResult *r);

/* Full streaming analysis of an MVC stream.  Returns 0 on success. */
int pf_scan(const char *path, ScanResult *out);

/* ── write statistics ──────────────────────────────────────────────── */
typedef struct {
    long long messages_modified;
    long long planes_created;
    long long frames_per_message;
    long long injected_gops;
} WriteStats;

/* Patch/insert offset-metadata into an MVC stream. */
int pf_write_stream(const char *in_path, const char *out_path,
                    PlaneSource *sources[MAX_PLANES], WriteStats *st,
                    const char *base_view);

/* As pf_write_stream, reusing a previous scan of `in_path`. */
int pf_write_stream_pre(const char *in_path, const char *out_path,
                        PlaneSource *sources[MAX_PLANES], WriteStats *st,
                        const char *base_view, const ScanResult *pre);

/* Sum of frame counts across all GOPs of a base-view AVC stream. */
long long pf_base_view_frames(const char *base_view_path);

/* ── OFS I/O ───────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  guid[16];
    int      frame_rate;
    int      drop_frame;
    int      plane_id;
    uint32_t num_frames;
    uint8_t *offsets;       /* num_frames bytes                        */
} OfsPlane;

int  ofs_read(const char *path, OfsPlane *out);      /* 0 on success    */
int  ofs_write(const char *path, const OfsPlane *p); /* 0 on success    */
void ofs_free(OfsPlane *p);

/* ── high level operations ─────────────────────────────────────────── */
int cmd_export(const char *in_path, const char *out_dir,
               int fps_override, int have_fps, int drop_frame,
               int have_plane, int only_plane);
int cmd_import(const char *in_path, const char *out_path, const char *from_ofs,
               int have_override, int override_depth, const char *base_view,
               const int *targets, int ntargets);

#endif /* PLANEFORGE_PF_H */
