/* OFMD offset-metadata block parsing and construction. */
#include "pf.h"

#include <stdlib.h>
#include <string.h>

/*
 * OFMD offset-metadata block: a 14-byte header followed by plane data.
 *
 *   offset  field
 *   0       "OFMD"
 *   4       frame rate (low nibble)
 *   10      plane count (low 7 bits)
 *   11      frame count (low 7 bits)
 *   13      flag
 *   14      plane count * frame count depth bytes, plane-major
 */

/* UUID-ish prefix that precedes the OFMD block inside the nested SEI. */
static const uint8_t NESTED_PAYLOAD_HDR[16] = {
    0x17, 0xEE, 0x8C, 0x60, 0xF8, 0x4D, 0x11, 0xD9,
    0x8C, 0xD6, 0x08, 0x00, 0x20, 0x0C, 0x9A, 0x66
};

long find_ofmd(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + OFMD_MAGIC_LEN <= n; ) {
        const uint8_t *m = memchr(p + i, 'O', n - i);
        if (!m)
            return -1;
        size_t idx = (size_t)(m - p);
        if (idx > 0 && idx + OFMD_HDR <= n &&
            memcmp(p + idx, OFMD_MAGIC, OFMD_MAGIC_LEN) == 0)
            return (long)idx;
        i = idx + 1;
    }
    return -1;
}

int ofmd_fields(const uint8_t *o, size_t n, OfmdFields *f)
{
    if (n < OFMD_HDR)
        return -1;
    f->frame_rate = o[4] & 0x0f;
    f->planes     = o[10] & 0x7f;
    f->frames     = o[11] & 0x7f;
    f->ig         = o[13];
    return 0;
}

uint8_t *build_ofmd_block(int n_planes, int n_frames, int frame_rate,
                          const uint8_t *plane_data, size_t plane_data_len,
                          size_t *out_len)
{
    size_t need = OFMD_HDR + (size_t)n_planes * (size_t)n_frames;
    uint8_t *h = (uint8_t *)malloc(need);
    if (!h)
        pf_fatal("out of memory");

    memcpy(h, OFMD_MAGIC, 4);
    h[4]  = (uint8_t)(0x80 | (frame_rate & 0x0f));
    h[5]  = 0x01;
    h[6]  = 0x00;
    h[7]  = 0x01;
    h[8]  = 0x00;
    h[9]  = 0x01;
    h[10] = (uint8_t)(n_planes & 0x7f);
    h[11] = (uint8_t)(n_frames & 0x7f);
    h[12] = 0x80;
    h[13] = 0x00;

    size_t body = (size_t)n_planes * (size_t)n_frames;
    if (plane_data && plane_data_len) {
        size_t copy = plane_data_len < body ? plane_data_len : body;
        memcpy(h + OFMD_HDR, plane_data, copy);
        if (copy < body)
            memset(h + OFMD_HDR + copy, 0x80, body - copy);
    } else {
        memset(h + OFMD_HDR, 0x80, body);
    }

    *out_len = need;
    return h;
}

static void put_sei_field(uint8_t **p, size_t *j, size_t *cap, size_t value)
{
    while (value >= 255) {
        if (*j + 1 > *cap) { *cap = *cap * 2 + 64; *p = realloc(*p, *cap); if(!*p) pf_fatal("oom"); }
        (*p)[(*j)++] = 0xff;
        value -= 255;
    }
    if (*j + 1 > *cap) { *cap = *cap * 2 + 64; *p = realloc(*p, *cap); if(!*p) pf_fatal("oom"); }
    (*p)[(*j)++] = (uint8_t)value;
}

/* Bytes put_sei_field emits for `value` (0xff run plus terminator). */
static size_t sei_field_len(size_t value) { return value / 255 + 1; }

uint8_t *build_ofmd_sei_nal(const uint8_t *ofmd, size_t ofmd_len,
                            uint8_t nal_hdr_byte, size_t *out_len)
{
    /*
     * Nested SEI wrapper: an outer message of type 0x25 (nesting) holds a flag
     * byte (0x60) followed by a nested message of type 0x05, whose payload is
     * the 16-byte prefix and the OFMD block.  The outer RBSP ends with the
     * rbsp_trailing_bits byte 0x80.
     */
    size_t npay = 16 + ofmd_len;
    /* nested_sei = type(0x05) + len + payload */
    size_t nested_len = 1 + sei_field_len(npay) + npay;
    uint8_t *nested = (uint8_t *)malloc(nested_len);
    if (!nested)
        pf_fatal("out of memory");
    size_t nj = 0, ncap = nested_len;
    put_sei_field(&nested, &nj, &ncap, 0x05);
    put_sei_field(&nested, &nj, &ncap, npay);
    memcpy(nested + nj, NESTED_PAYLOAD_HDR, 16);
    nj += 16;
    memcpy(nested + nj, ofmd, ofmd_len);
    nj += ofmd_len;

    /* nesting_payload = 0x60 + nested_sei, then a trailing byte per message. */
    size_t nestpay = 1 + nested_len;
    size_t rbsp_len = 1 + sei_field_len(nestpay) + nestpay + 1;
    uint8_t *rbsp = (uint8_t *)malloc(rbsp_len);
    if (!rbsp)
        pf_fatal("out of memory");
    size_t rj = 0, rcap = rbsp_len;
    put_sei_field(&rbsp, &rj, &rcap, 0x25);
    put_sei_field(&rbsp, &rj, &rcap, nestpay);
    rbsp[rj++] = 0x60;
    memcpy(rbsp + rj, nested, nested_len);
    rj += nested_len;
    rbsp[rj++] = 0x80;      /* rbsp_trailing_bits */

    free(nested);

    size_t ebsp_len = 0;
    uint8_t *ebsp = rbsp_to_ebsp(rbsp, rj, &ebsp_len);
    free(rbsp);

    uint8_t *out = (uint8_t *)malloc(ebsp_len + 1);
    if (!out)
        pf_fatal("out of memory");
    out[0] = nal_hdr_byte;
    memcpy(out + 1, ebsp, ebsp_len);
    free(ebsp);
    *out_len = ebsp_len + 1;
    return out;
}
