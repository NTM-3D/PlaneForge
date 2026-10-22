/* Reader/writer for OFS 3D-Plane files.
 *
 *   offset  size  field
 *   0       8     signature 89 4f 46 53 0d 0a 1a 0a
 *   8       4     version "0100"
 *   12      16    guid (guid[15] = plane id)
 *   28      1     frame_rate(hi nibble) | drop_frame(lo nibble)
 *   29      4     number_of_rolls + reserved + marker bits (zero)
 *   33      4     start_timecode (zero)
 *   37      4     number_of_frames (u32 LE)
 *   41      8     offsets length (u64 LE)
 *   49      N     one signed-magnitude depth byte per frame
 */
#include "pf.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static const uint8_t SIG[8] = OFS_SIGNATURE;

void ofs_free(OfsPlane *p)
{
    if (p) {
        free(p->offsets);
        p->offsets = NULL;
    }
}

int ofs_read(const char *path, OfsPlane *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        pf_warn("cannot open OFS file '%s'", path);
        return -1;
    }
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long long sz = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long long sz = (long long)ftello(f);
#endif
    if (sz < OFS_HEADER) { fclose(f); pf_warn("'%s' is too small to be an OFS file", path); return -1; }
    rewind(f);
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) pf_fatal("out of memory");
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); pf_warn("short read on '%s'", path); return -1; }
    fclose(f);

    if (memcmp(b, SIG, 8) != 0) {
        free(b);
        pf_warn("'%s' is not an OFS file (bad signature)", path);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->guid, b + 12, 16);
    out->plane_id   = b[27];          /* guid[15] */
    out->frame_rate = (b[28] >> 4) & 0x0f;
    out->drop_frame = b[28] & 0x0f;
    memcpy(out->reserved, b + 29, 4);
    out->start_timecode = rd32(b + 33);

    uint32_t nf = rd32(b + 37);
    const uint8_t *off = NULL;

    if ((size_t)sz == (size_t)OFS_HEADER + nf) {
        off = b + OFS_HEADER;                       /* no length prefix   */
    } else if (sz >= OFS_HEADER + 8) {
        uint64_t ulen = rd64(b + OFS_HEADER);
        if (ulen == nf && (size_t)sz >= OFS_HEADER + 8 + nf)
            off = b + OFS_HEADER + 8;               /* wincode/bincode    */
        else if ((size_t)sz >= OFS_HEADER + 8 + nf)
            off = b + OFS_HEADER + 8;               /* tolerate extras    */
    }

    if (!off) {
        free(b);
        pf_warn("'%s' has an inconsistent frame count", path);
        return -1;
    }

    out->num_frames = nf;
    out->offsets = (uint8_t *)malloc(nf ? nf : 1);
    if (!out->offsets) pf_fatal("out of memory");
    memcpy(out->offsets, off, nf);
    free(b);
    return 0;
}

int ofs_write(const char *path, const OfsPlane *p)
{
    uint8_t hdr[OFS_HEADER + 8];
    memcpy(hdr, SIG, 8);
    hdr[8]  = '0'; hdr[9] = '1'; hdr[10] = '0'; hdr[11] = '0';
    memcpy(hdr + 12, p->guid, 16);
    hdr[27] = (uint8_t)p->plane_id;     /* the plane id lives in the guid */
    hdr[28] = (uint8_t)(((p->frame_rate & 0x0f) << 4) | (p->drop_frame & 0x0f));
    memcpy(hdr + 29, p->reserved, 4);
    wr32(hdr + 33, p->start_timecode);
    wr32(hdr + 37, p->num_frames);
    wr64(hdr + 41, p->num_frames);

    FILE *f = fopen(path, "wb");
    if (!f) {
        pf_warn("cannot create '%s'", path);
        return -1;
    }
    int ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
             (p->num_frames == 0 ||
              fwrite(p->offsets, 1, p->num_frames, f) == p->num_frames);
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        pf_warn("failed writing '%s'", path);
        return -1;
    }
    return 0;
}
