//-------------------------------------------------------------------
// Streaming baseline JPEG encoder - see jpegenc.h.
//
// All integer. The camera's library is built for armeabi, which has no
// hardware floating point: every float operation is a library call, and a
// float DCT made the encoder the slowest part of a shot. The forward DCT is
// the IJG's fast integer AAN (jfdctfst, 8 fractional bits), with the AAN
// scale factors folded into fixed-point quantiser reciprocals.
//-------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include "jpegenc.h"

static const unsigned char zz[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

static const unsigned char std_lq[64] = {
    16,11,10,16, 24, 40, 51, 61, 12,12,14,19, 26, 58, 60, 55,
    14,13,16,24, 40, 57, 69, 56, 14,17,22,29, 51, 87, 80, 62,
    18,22,37,56, 68,109,103, 77, 24,35,55,64, 81,104,113, 92,
    49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103, 99
};

static const unsigned char std_cq[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
};

static const unsigned char dc_l_bits[16] = { 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0 };
static const unsigned char dc_c_bits[16] = { 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0 };
static const unsigned char dc_vals[12]   = { 0,1,2,3,4,5,6,7,8,9,10,11 };

static const unsigned char ac_l_bits[16] = { 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d };
static const unsigned char ac_l_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const unsigned char ac_c_bits[16] = { 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77 };
static const unsigned char ac_c_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

typedef struct { unsigned short code[256]; unsigned char size[256]; } huff_t;

struct jpeg_enc
{
    unsigned w, h, row, have;           // have: rows in the strip
    unsigned char *strip;               // 8 rows of RGB
    jpeg_write_fn out;
    void *ctx;
    int ok;

    unsigned char qt[2][64];            // natural order
    long  recip[2][64];                 // 2^18 / divisor, natural order
    huff_t dc[2], ac[2];
    int pred[3];

    unsigned bitbuf, bitcnt;
    unsigned char ob[4096];
    unsigned olen;
};

//-------------------------------------------------------------------

static void flush_out(struct jpeg_enc *e)
{
    if (e->olen && e->ok && !e->out(e->ctx, e->ob, e->olen)) e->ok = 0;
    e->olen = 0;
}

static void byte_out(struct jpeg_enc *e, unsigned char b)
{
    e->ob[e->olen++] = b;
    if (e->olen == sizeof(e->ob)) flush_out(e);
}

static void put_bits(struct jpeg_enc *e, unsigned code, unsigned n)
{
    e->bitbuf = (e->bitbuf << n) | (code & ((1u << n) - 1));
    e->bitcnt += n;
    while (e->bitcnt >= 8)
    {
        unsigned char b = (unsigned char)(e->bitbuf >> (e->bitcnt - 8));
        byte_out(e, b);
        if (b == 0xff) byte_out(e, 0);
        e->bitcnt -= 8;
    }
}

static void build_huff(huff_t *t, const unsigned char *bits, const unsigned char *vals)
{
    unsigned code = 0, k = 0, len, i;
    memset(t, 0, sizeof(*t));
    for (len = 1; len <= 16; len++)
    {
        for (i = 0; i < bits[len - 1]; i++, k++)
        {
            t->code[vals[k]] = (unsigned short)code++;
            t->size[vals[k]] = (unsigned char)len;
        }
        code <<= 1;
    }
}

static void marker(struct jpeg_enc *e, unsigned m, unsigned len)
{
    byte_out(e, 0xff); byte_out(e, (unsigned char)m);
    byte_out(e, (unsigned char)(len >> 8)); byte_out(e, (unsigned char)len);
}

static void dht(struct jpeg_enc *e, int cls, int id, const unsigned char *bits,
                const unsigned char *vals, unsigned nvals)
{
    unsigned i;
    marker(e, 0xc4, 3 + 16 + nvals);
    byte_out(e, (unsigned char)((cls << 4) | id));
    for (i = 0; i < 16; i++) byte_out(e, bits[i]);
    for (i = 0; i < nvals; i++) byte_out(e, vals[i]);
}

//-------------------------------------------------------------------

#define FIX_0_382683433  98
#define FIX_0_541196100  139
#define FIX_0_707106781  181
#define FIX_1_306562965  334
#define MUL8(v, c)       (((v) * (c)) >> 8)

static void fdct(int *d)
{
    int t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13;
    int z1, z2, z3, z4, z5, z11, z13;
    int i, s;

    for (s = 0; s < 2; s++)
        for (i = 0; i < 8; i++)
        {
            // First pass over rows (step 1, stride 8), then over columns.
            int *p = s ? d + i : d + i * 8;
            int k = s ? 8 : 1;

            t0 = p[0] + p[7*k]; t7 = p[0] - p[7*k];
            t1 = p[k] + p[6*k]; t6 = p[k] - p[6*k];
            t2 = p[2*k] + p[5*k]; t5 = p[2*k] - p[5*k];
            t3 = p[3*k] + p[4*k]; t4 = p[3*k] - p[4*k];

            t10 = t0 + t3; t13 = t0 - t3;
            t11 = t1 + t2; t12 = t1 - t2;
            p[0] = t10 + t11;
            p[4*k] = t10 - t11;
            z1 = MUL8(t12 + t13, FIX_0_707106781);
            p[2*k] = t13 + z1;
            p[6*k] = t13 - z1;

            t10 = t4 + t5; t11 = t5 + t6; t12 = t6 + t7;
            z5 = MUL8(t10 - t12, FIX_0_382683433);
            z2 = MUL8(t10, FIX_0_541196100) + z5;
            z4 = MUL8(t12, FIX_1_306562965) + z5;
            z3 = MUL8(t11, FIX_0_707106781);
            z11 = t7 + z3; z13 = t7 - z3;
            p[5*k] = z13 + z2; p[3*k] = z13 - z2;
            p[k] = z11 + z4;   p[7*k] = z11 - z4;
        }
}
static void put_value(struct jpeg_enc *e, int v, const huff_t *t, int sym_hi)
{
    int a = v < 0 ? -v : v, n = 0;
    while (a) { n++; a >>= 1; }
    put_bits(e, t->code[sym_hi | n], t->size[sym_hi | n]);
    if (n) put_bits(e, (unsigned)(v < 0 ? v - 1 : v), (unsigned)n);
}

static void block(struct jpeg_enc *e, int *d, int comp)
{
    int q[64], i, run, last, t = comp ? 1 : 0;

    fdct(d);
    for (i = 0; i < 64; i++)
    {
        long long v = (long long)d[zz[i]] * e->recip[t][zz[i]];
        // Rounded half away from zero, as the float version rounded.
        q[i] = v < 0 ? -(int)((-v + (1 << 17)) >> 18) : (int)((v + (1 << 17)) >> 18);
    }

    put_value(e, q[0] - e->pred[comp], &e->dc[t], 0);
    e->pred[comp] = q[0];

    for (last = 63; last > 0 && q[last] == 0; last--) ;
    for (i = 1, run = 0; i <= last; i++)
    {
        if (q[i] == 0) { run++; continue; }
        while (run > 15) { put_bits(e, e->ac[t].code[0xf0], e->ac[t].size[0xf0]); run -= 16; }
        put_value(e, q[i], &e->ac[t], run << 4);
        run = 0;
    }
    if (last < 63) put_bits(e, e->ac[t].code[0], e->ac[t].size[0]);
}

static void encode_strip(struct jpeg_enc *e)
{
    int y[64], cb[64], cr[64];
    unsigned bx, i, j;

    for (bx = 0; bx < e->w; bx += 8)
    {
        for (j = 0; j < 8; j++)
            for (i = 0; i < 8; i++)
            {
                unsigned x = bx + i < e->w ? bx + i : e->w - 1;
                const unsigned char *p = e->strip + (j * e->w + x) * 3;
                int r = p[0], g = p[1], b = p[2];
                // JFIF's YCbCr in 16-bit fixed point.
                y[j * 8 + i]  = ((19595 * r + 38470 * g + 7471 * b + 32768) >> 16) - 128;
                cb[j * 8 + i] = (-11059 * r - 21709 * g + 32768 * b + 32768) >> 16;
                cr[j * 8 + i] = (32768 * r - 27439 * g - 5329 * b + 32768) >> 16;
            }
        block(e, y, 0);
        block(e, cb, 1);
        block(e, cr, 2);
    }
}

//-------------------------------------------------------------------

jpeg_enc_t *jpeg_begin(unsigned w, unsigned h, int quality,
                       jpeg_write_fn out, void *ctx,
                       const char *comment, unsigned comment_len)
{
    static const float aan[8] = {
        1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
        1.0f, 0.785694958f, 0.541196100f, 0.275899379f
    };
    struct jpeg_enc *e;
    int i, t, scale;

    if (w == 0 || h == 0 || w > 65535 || h > 65535) return 0;
    e = (struct jpeg_enc *)calloc(1, sizeof(*e));
    if (!e) return 0;
    e->strip = (unsigned char *)malloc((size_t)w * 8 * 3);
    if (!e->strip) { free(e); return 0; }
    e->w = w; e->h = h; e->out = out; e->ctx = ctx; e->ok = 1;

    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (t = 0; t < 2; t++)
        for (i = 0; i < 64; i++)
        {
            int v = ((t ? std_cq[i] : std_lq[i]) * scale + 50) / 100;
            if (v < 1) v = 1;
            if (v > 255) v = 255;
            e->qt[t][i] = (unsigned char)v;
            // Set once per picture, so the float here costs nothing.
            e->recip[t][i] = (long)(262144.0 / (v * aan[i >> 3] * aan[i & 7] * 8.0) + 0.5);
        }

    build_huff(&e->dc[0], dc_l_bits, dc_vals);
    build_huff(&e->dc[1], dc_c_bits, dc_vals);
    build_huff(&e->ac[0], ac_l_bits, ac_l_vals);
    build_huff(&e->ac[1], ac_c_bits, ac_c_vals);

    byte_out(e, 0xff); byte_out(e, 0xd8);

    // JFIF, so every viewer agrees what the three components are.
    marker(e, 0xe0, 16);
    byte_out(e, 'J'); byte_out(e, 'F'); byte_out(e, 'I'); byte_out(e, 'F'); byte_out(e, 0);
    byte_out(e, 1); byte_out(e, 1); byte_out(e, 0);
    byte_out(e, 0); byte_out(e, 1); byte_out(e, 0); byte_out(e, 1);
    byte_out(e, 0); byte_out(e, 0);

    while (comment && comment_len)
    {
        unsigned n = comment_len > 65000 ? 65000 : comment_len, k;
        marker(e, 0xfe, n + 2);
        for (k = 0; k < n; k++) byte_out(e, (unsigned char)comment[k]);
        comment += n; comment_len -= n;
    }

    for (t = 0; t < 2; t++)
    {
        marker(e, 0xdb, 67);
        byte_out(e, (unsigned char)t);
        for (i = 0; i < 64; i++) byte_out(e, e->qt[t][zz[i]]);
    }

    marker(e, 0xc0, 17);
    byte_out(e, 8);
    byte_out(e, (unsigned char)(h >> 8)); byte_out(e, (unsigned char)h);
    byte_out(e, (unsigned char)(w >> 8)); byte_out(e, (unsigned char)w);
    byte_out(e, 3);
    for (i = 0; i < 3; i++) { byte_out(e, (unsigned char)(i + 1)); byte_out(e, 0x11); byte_out(e, (unsigned char)(i ? 1 : 0)); }

    dht(e, 0, 0, dc_l_bits, dc_vals, 12);
    dht(e, 1, 0, ac_l_bits, ac_l_vals, 162);
    dht(e, 0, 1, dc_c_bits, dc_vals, 12);
    dht(e, 1, 1, ac_c_bits, ac_c_vals, 162);

    marker(e, 0xda, 12);
    byte_out(e, 3);
    for (i = 0; i < 3; i++) { byte_out(e, (unsigned char)(i + 1)); byte_out(e, (unsigned char)(i ? 0x11 : 0x00)); }
    byte_out(e, 0); byte_out(e, 63); byte_out(e, 0);
    return e;
}

void jpeg_rows(jpeg_enc_t *e, const unsigned char *rgb, unsigned n)
{
    while (n-- && e->row < e->h)
    {
        memcpy(e->strip + e->have * e->w * 3, rgb, (size_t)e->w * 3);
        rgb += e->w * 3;
        e->row++;
        if (++e->have == 8) { encode_strip(e); e->have = 0; }
    }
}

int jpeg_end(jpeg_enc_t *e)
{
    int ok;
    if (e->have)
    {
        // Repeat the last row down to the strip's end; the decoder crops it.
        while (e->have < 8)
        {
            memcpy(e->strip + e->have * e->w * 3, e->strip + (e->have - 1) * e->w * 3, (size_t)e->w * 3);
            e->have++;
        }
        encode_strip(e);
    }
    if (e->bitcnt) put_bits(e, 0x7f, 7);    // pad with ones to a byte
    byte_out(e, 0xff); byte_out(e, 0xd9);
    flush_out(e);
    ok = e->ok;
    free(e->strip);
    free(e);
    return ok;
}

int jpeg_mem_write(void *ctx, const unsigned char *buf, unsigned len)
{
    jpeg_membuf_t *m = (jpeg_membuf_t *)ctx;
    if (m->len + len > m->cap)
    {
        unsigned cap = m->cap ? m->cap * 2 : 65536;
        unsigned char *nb;
        while (cap < m->len + len) cap *= 2;
        nb = (unsigned char *)realloc(m->buf, cap);
        if (!nb) { m->failed = 1; return 0; }
        m->buf = nb; m->cap = cap;
    }
    memcpy(m->buf + m->len, buf, len);
    m->len += len;
    return 1;
}
