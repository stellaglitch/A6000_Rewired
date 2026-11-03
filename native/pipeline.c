//-------------------------------------------------------------------
// One shot, start to finish - see pipeline.h.
//
// Two ways through, chosen by the recipe:
//
//   streamed   the matrix alone. Every source a matrix can read is within
//              128 rows of the pixel it is bending - the row above for the
//              line memory, tap_dy (a signed char) for the tap - so a ring of
//              256 rows is all that is ever held: about 4 MB, read, bent,
//              re-encoded and developed in one pass.
//
//   framed     anything in the experimental chain. Those effects address the
//              frame buffer as a whole - a stuck row address line reads rows
//              thousands apart - so the whole frame is held, 36 MB on the
//              a6000. That is the expensive path, and the one the card log
//              watches most closely.
//
// The streamed path applies a segmented matrix one row at a time, every
// segment in turn, where the framed path does one whole-frame pass per
// segment. The pixels are the same unless a bus-fed source (tap, line
// memory) reads across a segment seam, where it may see the neighbouring
// segment's pixel one pass earlier or later. Nothing else differs.
//-------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pipeline.h"
#include "arw.h"
#include "jpegenc.h"
#include "log.h"

volatile int rw_progress;
volatile int rw_stage;

#define ROM_MAX         0x00100000      // a megabyte, as CHDK_Rewired reads
#define PREV_W          1616            // the a6000's own preview width
#define THUMB_W         160
#define THUMB_H         120
#define RING_ROWS       256             // tap_dy -128..127, plus the row itself
#define IO_BUF          (256 * 1024)    // the card sits behind Sony's FUSE layer: few, large calls

// Where the time goes, for the card log: milliseconds per part of the shot.
static unsigned long t_read, t_bend, t_arw, t_dev;

//-------------------------------------------------------------------
// The frame: 12bpp, four pixels in six bytes, the PowerShot layout that
// bendx_get()/bendx_set() and the matrix fast path below both speak. With
// ring set, only RING_ROWS rows are held and row y lives in slot y % ring.

typedef struct
{
    unsigned char *base;
    unsigned w, h, rowlen;
    unsigned ring;                      // 0 = the whole frame
} frame_t;

static unsigned char *frow(const frame_t *f, unsigned y)
{
    return f->base + (unsigned long)(f->ring ? y % f->ring : y) * f->rowlen;
}

static void pack_row(unsigned char *p, const unsigned short *v, unsigned w)
{
    unsigned i;
    for (i = 0; i < w; i += 4, p += 6, v += 4)
    {
        unsigned p0 = v[0], p1 = v[1], p2 = v[2], p3 = v[3];
        p[0] = (unsigned char)(((p0 & 0x00f) << 4) | ((p1 >> 8) & 0x0f));
        p[1] = (unsigned char)((p0 >> 4) & 0xff);
        p[2] = (unsigned char)((p2 >> 4) & 0xff);
        p[3] = (unsigned char)(p1 & 0xff);
        p[4] = (unsigned char)(p3 & 0xff);
        p[5] = (unsigned char)(((p2 & 0x00f) << 4) | ((p3 >> 8) & 0x0f));
    }
}

static void unpack_row(const unsigned char *p, unsigned short *v, unsigned w)
{
    unsigned i;
    for (i = 0; i < w; i += 4, p += 6, v += 4)
    {
        v[0] = (unsigned short)(((unsigned)p[1] << 4) | (p[0] >> 4));
        v[1] = (unsigned short)(((unsigned)(p[0] & 0x0f) << 8) | p[3]);
        v[2] = (unsigned short)(((unsigned)p[2] << 4) | (p[5] >> 4));
        v[3] = (unsigned short)(((unsigned)(p[5] & 0x0f) << 8) | p[4]);
    }
}

//-------------------------------------------------------------------
// Progress. The engines call the service hook every 64 rows; each pass over
// the frame is one step of a known total, so the bar moves evenly.

static unsigned prog_rows, prog_total, prog_base, prog_span;

static void prog_set(int stage, unsigned base, unsigned span, unsigned total)
{
    rw_stage = stage;
    prog_base = base; prog_span = span; prog_total = total ? total : 1; prog_rows = 0;
    rw_progress = (int)base;
}

static void prog_rows_done(unsigned rows)
{
    prog_rows = rows;
    if (prog_rows > prog_total) prog_rows = prog_total;
    rw_progress = (int)(prog_base + (unsigned long)prog_span * prog_rows / prog_total);
}

static void prog_service(void)
{
    prog_rows_done(prog_rows + 64);
}

//-------------------------------------------------------------------
// The matrix. A port of CHDK_Rewired's bb_run() (core/raw.c) from its camera
// accessors to this frame: same order, same sources, same seam behaviour.
// One row of one segment; the callers decide the order rows and segments
// are visited in.

typedef struct
{
    bend_t        *b;
    bend_cc_t      cc;
    unsigned short lut[1u << RW_NBITS];
} seg_cc_t;

static void bend_row(const frame_t *fr, seg_cc_t *s, int layout, int size, int seg,
                     unsigned y, unsigned frame_no, unsigned ob_x)
{
    bend_t *b = s->b;
    unsigned x, w = fr->w, h = fr->h, mask = (1u << RW_NBITS) - 1;
    unsigned char *rowp = frow(fr, y);
    unsigned char  segs[BEND_SEG_SPANS];
    unsigned short xend[BEND_SEG_SPANS];
    unsigned short *lut = s->lut;
    bend_src_t src;
    unsigned noise_state;
    int nsp, i, mine = 0, chan = b->bayer;

    if (s->cc.is_identity) return;
    if (b->row_period > 1 && (y % b->row_period) != 0) return;

    nsp = bend_seg_spans(layout, size, (int)y, (int)w, (int)h, segs, xend);
    for (i = 0; i < nsp; i++) if (segs[i] == seg) { mine = 1; break; }
    if (!mine) return;

    src.frame = frame_no;
    src.ob    = (s->cc.bus_used & (1u << BUS_OB)) ? (bendx_get(rowp, ob_x, RW_NBITS) & mask) : 0;
    src.prev = src.above = src.tap = src.noise = 0;
    noise_state = bend_trash_seed(b, y, src.ob);

    for (i = 0; i < nsp; i++)
    {
        unsigned x0 = i > 0 ? xend[i - 1] : 0, x1 = xend[i];
        if (segs[i] != (unsigned char)seg) continue;

        if (s->cc.is_pure && chan == 0)
        {
            // Whole groups of four through the table, the ragged ends of a
            // span one pixel at a time.
            unsigned a0 = (x0 + 3u) & ~3u, a1 = x1 & ~3u;
            if (a0 > x1) a0 = x1;
            for (x = x0; x < a0; x++)
                bendx_set(rowp, x, lut[bendx_get(rowp, x, RW_NBITS) & mask], RW_NBITS);
            if (a1 > a0)
            {
                unsigned char *p = rowp + (a0 / 4) * 6;
                unsigned g, n = (a1 - a0) / 4;
                for (g = 0; g < n; g++, p += 6)
                {
                    unsigned p0 = ((unsigned)p[1] << 4) | (p[0] >> 4);
                    unsigned p1 = ((unsigned)(p[0] & 0x0f) << 8) | p[3];
                    unsigned p2 = ((unsigned)p[2] << 4) | (p[5] >> 4);
                    unsigned p3 = ((unsigned)(p[5] & 0x0f) << 8) | p[4];
                    p0 = lut[p0]; p1 = lut[p1]; p2 = lut[p2]; p3 = lut[p3];
                    p[0] = (unsigned char)(((p0 & 0x00f) << 4) | ((p1 >> 8) & 0x0f));
                    p[1] = (unsigned char)((p0 >> 4) & 0xff);
                    p[2] = (unsigned char)((p2 >> 4) & 0xff);
                    p[3] = (unsigned char)(p1 & 0xff);
                    p[4] = (unsigned char)(p3 & 0xff);
                    p[5] = (unsigned char)(((p2 & 0x00f) << 4) | ((p3 >> 8) & 0x0f));
                }
            }
            for (x = a1 > a0 ? a1 : a0; x < x1; x++)
                bendx_set(rowp, x, lut[bendx_get(rowp, x, RW_NBITS) & mask], RW_NBITS);
            continue;
        }

        // The held sample entering a span is the pixel to its left, already
        // bent by its own segment - the smear crosses the seam, as it does on
        // the PowerShots.
        src.prev = x0 > 0 ? (bendx_get(rowp, x0 - 1, RW_NBITS) & mask) : 0;

        for (x = x0; x < x1; x++)
        {
            unsigned v;
            if (chan && (int)(((y & 1) << 1) | (x & 1)) != chan - 1) continue;
            v = bendx_get(rowp, x, RW_NBITS) & mask;
            if (s->cc.bus_used & (1u << BUS_NOISE))
                src.noise = bend_trash_step(b, &noise_state, x);
            if (s->cc.bus_used & (1u << BUS_VHOLD))
                src.above = y > 0 ? (bendx_get(frow(fr, y - 1), x, RW_NBITS) & mask) : 0;
            if (s->cc.bus_used & (1u << BUS_TAP))
            {
                int tx = (int)x + b->tap_dx, ty = (int)y + b->tap_dy;
                int lo = 0, hi = (int)h - 1;
                // In a ring only the rows around y are held; tap_dy cannot
                // reach past them, but the clamp keeps that a promise.
                if (fr->ring)
                {
                    if (lo < (int)y - (int)fr->ring / 2) lo = (int)y - (int)fr->ring / 2;
                    if (hi > (int)y + (int)fr->ring / 2 - 1) hi = (int)y + (int)fr->ring / 2 - 1;
                }
                if (tx < 0) tx = 0;
                if (tx >= (int)w) tx = (int)w - 1;
                if (ty < lo) ty = lo;
                if (ty > hi) ty = hi;
                src.tap = bendx_get(frow(fr, (unsigned)ty), (unsigned)tx, RW_NBITS) & mask;
            }
            bendx_set(rowp, x, bend_eval(&s->cc, v, (int)x, (int)y, &src), RW_NBITS);
            src.prev = v;
        }
    }
}

//-------------------------------------------------------------------
// Developing. Half size: each 2x2 RGGB quad is one output pixel, which needs
// no interpolation and is as honest about what is in the raw as a picture
// gets. As-shot white balance from SR2Private, the a6000's colour matrix
// (Adobe's, as dcraw and every raw developer carry it), sRGB.

// Integer per pixel: the float maths happens once, building the tables. The
// camera's armeabi build has no hardware floating point, and a float per
// pixel colour matrix cost seconds a shot.
typedef struct
{
    unsigned short wb[3][4096]; // raw value -> black-subtracted, balanced, 0..4095
    int m[3][3];                // camera -> sRGB, Q12
    unsigned char gamma[4096];
} develop_t;

static void inv3(float a[3][3], float o[3][3])
{
    float d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
            - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
            + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
        {
            int r0 = (j + 1) % 3, r1 = (j + 2) % 3, c0 = (i + 1) % 3, c1 = (i + 2) % 3;
            o[i][j] = (a[r0][c0] * a[r1][c1] - a[r0][c1] * a[r1][c0]) / d;
        }
}

static float srgb_gamma(float v)
{
    if (v <= 0.0031308f) return 12.92f * v;
    return 1.055f * (float)pow(v, 1.0 / 2.4) - 0.055f;
}

static void dev_init(develop_t *d, const arw_info_t *a)
{
    // ILCE-6000 colour matrix, XYZ -> camera, from Adobe DNG Converter.
    static const float cam_xyz[3][3] = {
        {  0.5991f, -0.1456f, -0.0455f },
        { -0.4764f,  1.2135f,  0.2980f },
        { -0.0707f,  0.1425f,  0.6701f }
    };
    static const float xyz_rgb[3][3] = {
        { 0.412453f, 0.357580f, 0.180423f },
        { 0.212671f, 0.715160f, 0.072169f },
        { 0.019334f, 0.119193f, 0.950227f }
    };
    float cam_rgb[3][3], m[3][3], range, mul[3];
    int i, j, k;

    for (i = 0; i < 3; i++)
    {
        float s = 0;
        for (j = 0; j < 3; j++)
        {
            cam_rgb[i][j] = 0;
            for (k = 0; k < 3; k++) cam_rgb[i][j] += cam_xyz[i][k] * xyz_rgb[k][j];
            s += cam_rgb[i][j];
        }
        for (j = 0; j < 3; j++) cam_rgb[i][j] /= s;
    }
    inv3(cam_rgb, m);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            d->m[i][j] = (int)(m[i][j] * 4096.f + (m[i][j] < 0 ? -0.5f : 0.5f));

    range = (float)(a->white > a->black + 64 ? a->white - a->black : 3900);
    mul[0] = (float)a->wb[0] / (float)a->wb[1];
    mul[1] = 1.0f;
    mul[2] = (float)a->wb[3] / (float)a->wb[1];
    for (k = 0; k < 3; k++)
        for (i = 0; i < 4096; i++)
        {
            float v = ((float)i - (float)a->black) * mul[k] / range * 4095.f;
            d->wb[k][i] = (unsigned short)(v < 0 ? 0 : (v > 4095.f ? 4095 : (int)(v + 0.5f)));
        }

    for (i = 0; i < 4096; i++)
    {
        float v = srgb_gamma(i / 4095.f) * 255.f + 0.5f;
        d->gamma[i] = (unsigned char)(v > 255 ? 255 : v);
    }
}

// Two raw rows (RGGB: R G / G B) to one row of RGB.
static void dev_row(const develop_t *d, const unsigned short *r0, const unsigned short *r1,
                    unsigned w2, unsigned char *out)
{
    unsigned x;
    for (x = 0; x < w2; x++, out += 3)
    {
        int c0 = d->wb[0][r0[2 * x] & 4095];
        int c1 = d->wb[1][((r0[2 * x + 1] + r1[2 * x]) >> 1) & 4095];
        int c2 = d->wb[2][r1[2 * x + 1] & 4095];
        int k;
        for (k = 0; k < 3; k++)
        {
            int i = (d->m[k][0] * c0 + d->m[k][1] * c1 + d->m[k][2] * c2) >> 12;
            out[k] = d->gamma[i < 0 ? 0 : (i > 4095 ? 4095 : i)];
        }
    }
}

//-------------------------------------------------------------------
// Area downscaling, a row at a time, for the preview.

typedef struct
{
    unsigned iw, ih, ow, oh;
    unsigned cur, rows;         // output row being gathered, input rows in it
    unsigned *acc;              // ow * 3
    unsigned *cols;             // input columns per output column
    unsigned char *out;         // ow * oh * 3
} shrink_t;

static int shrink_init(shrink_t *s, unsigned iw, unsigned ih, unsigned ow, unsigned oh)
{
    unsigned x;
    memset(s, 0, sizeof(*s));
    s->iw = iw; s->ih = ih; s->ow = ow; s->oh = oh;
    s->acc  = (unsigned *)calloc(ow * 3, sizeof(unsigned));
    s->cols = (unsigned *)calloc(ow, sizeof(unsigned));
    s->out  = (unsigned char *)calloc((size_t)ow * oh * 3, 1);
    if (!s->acc || !s->cols || !s->out) return 0;
    for (x = 0; x < iw; x++) s->cols[(unsigned long)x * ow / iw]++;
    return 1;
}

static void shrink_emit(shrink_t *s)
{
    unsigned x, k;
    if (s->rows && s->cur < s->oh)
        for (x = 0; x < s->ow; x++)
            for (k = 0; k < 3; k++)
            {
                unsigned n = s->cols[x] * s->rows;
                s->out[((unsigned long)s->cur * s->ow + x) * 3 + k] =
                    (unsigned char)(n ? (s->acc[x * 3 + k] + n / 2) / n : 0);
            }
    memset(s->acc, 0, s->ow * 3 * sizeof(unsigned));
    s->rows = 0;
}

static void shrink_row(shrink_t *s, unsigned y, const unsigned char *rgb)
{
    unsigned x, oy = (unsigned)((unsigned long)y * s->oh / s->ih);
    if (oy != s->cur) { shrink_emit(s); s->cur = oy; }
    for (x = 0; x < s->iw; x++)
    {
        unsigned ox = (unsigned)((unsigned long)x * s->ow / s->iw);
        s->acc[ox * 3]     += rgb[x * 3];
        s->acc[ox * 3 + 1] += rgb[x * 3 + 1];
        s->acc[ox * 3 + 2] += rgb[x * 3 + 2];
    }
    s->rows++;
}

static void shrink_free(shrink_t *s)
{
    free(s->acc); free(s->cols); free(s->out);
    memset(s, 0, sizeof(*s));
}

//-------------------------------------------------------------------
// Encoding into a slot that is already in the file: try qualities from good
// to poor until it fits. Returns the length, or 0 if nothing fit.

static unsigned jpeg_fit(const unsigned char *rgb, unsigned w, unsigned h,
                         unsigned room, jpeg_membuf_t *m)
{
    static const int q[] = { 92, 85, 75, 65, 50, 35 };
    unsigned i, y;
    for (i = 0; i < sizeof(q) / sizeof(q[0]); i++)
    {
        jpeg_enc_t *e;
        m->len = 0; m->failed = 0;
        e = jpeg_begin(w, h, q[i], jpeg_mem_write, m, 0, 0);
        if (!e) return 0;
        for (y = 0; y < h; y++) jpeg_rows(e, rgb + (unsigned long)y * w * 3, 1);
        if (jpeg_end(e) && !m->failed && m->len <= room) return m->len;
    }
    return 0;
}

static int put_slot(FILE *f, unsigned long off, unsigned long room, unsigned long len_entry,
                    const unsigned char *data, unsigned long len)
{
    unsigned char zero[256], lb[4];
    unsigned long left;
    if (!off || !len_entry || len > room) return 0;
    memset(zero, 0, sizeof(zero));
    if (fseek(f, (long)off, SEEK_SET) != 0 || fwrite(data, 1, len, f) != len) return 0;
    for (left = room - len; left; )
    {
        unsigned long n = left > sizeof(zero) ? sizeof(zero) : left;
        if (fwrite(zero, 1, n, f) != n) return 0;
        left -= n;
    }
    lb[0] = (unsigned char)len; lb[1] = (unsigned char)(len >> 8);
    lb[2] = (unsigned char)(len >> 16); lb[3] = (unsigned char)(len >> 24);
    return fseek(f, (long)len_entry, SEEK_SET) == 0 && fwrite(lb, 1, 4, f) == 4;
}

static int copy_bytes(FILE *in, FILE *out, unsigned long from, unsigned long n, unsigned char *buf, unsigned bufsz)
{
    if (fseek(in, (long)from, SEEK_SET) != 0) return 0;
    while (n)
    {
        unsigned long k = n > bufsz ? bufsz : n;
        if (fread(buf, 1, k, in) != k || fwrite(buf, 1, k, out) != k) return 0;
        n -= k;
    }
    return 1;
}

static unsigned char *read_whole(const char *path, unsigned long max, unsigned long *len)
{
    FILE *f;
    unsigned char *b;
    *len = 0;
    if (!path || !(f = fopen(path, "rb"))) return 0;
    b = (unsigned char *)malloc(max);
    if (b) *len = (unsigned long)fread(b, 1, max, f);
    fclose(f);
    if (b && !*len) { free(b); b = 0; }
    return b;
}

static int file_write(void *ctx, const unsigned char *buf, unsigned len)
{
    return fwrite(buf, 1, len, (FILE *)ctx) == len;
}

//-------------------------------------------------------------------
// The output side: the new ARW's strip, the developed JPEG and the preview,
// fed one bent row at a time by either path.

typedef struct
{
    const arw_info_t *a;
    FILE *in, *out, *jf;
    jpeg_enc_t *je;
    develop_t *dev;
    shrink_t sp;
    unsigned w, w2, h2, pw, ph;
    unsigned short *lin0, *lin1, *codes, *orig;
    unsigned char *outrow, *jrow, *copybuf;
    char *tag;
} out_t;

static int out_begin(out_t *o, const rw_job_t *job, const rw_recipe_t *r, char *err)
{
    const arw_info_t *a = o->a;
    unsigned w = a->width, h = a->height;

    o->w = w;
    // The developed picture leaves out the strip's right margin: 6048 holds
    // a 6024 picture, and the 24 columns past it are not image.
    o->w2 = (w == 6048 ? 6024 : w) / 2;
    o->h2 = h / 2;
    o->pw = PREV_W;
    o->ph = (unsigned)((unsigned long)PREV_W * o->h2 / o->w2);

    o->lin0    = (unsigned short *)malloc(w * sizeof(unsigned short));
    o->lin1    = (unsigned short *)malloc(w * sizeof(unsigned short));
    o->codes   = (unsigned short *)malloc(w * sizeof(unsigned short));
    o->orig    = (unsigned short *)malloc(w * sizeof(unsigned short));
    o->outrow  = (unsigned char *)malloc(w);
    o->jrow    = (unsigned char *)malloc((size_t)o->w2 * 3);
    o->copybuf = (unsigned char *)malloc(65536);
    o->dev     = (develop_t *)malloc(sizeof(develop_t));
    if (!o->lin0 || !o->lin1 || !o->codes || !o->orig || !o->outrow || !o->jrow || !o->copybuf || !o->dev ||
        !shrink_init(&o->sp, o->w2, o->h2, o->pw, o->ph))
    { strcpy(err, "not enough memory to write the output"); return 0; }
    dev_init(o->dev, a);

    if (job->out_arw)
    {
        o->out = fopen(job->out_arw, "w+b");
        if (!o->out) { sprintf(err, "cannot create %.200s", job->out_arw); return 0; }
        setvbuf(o->out, 0, _IOFBF, IO_BUF);
        if (!copy_bytes(o->in, o->out, 0, a->strip_off, o->copybuf, 65536))
        { strcpy(err, "writing the new ARW failed - is the card full?"); return 0; }
    }
    if (job->out_jpg)
    {
        o->tag = (char *)malloc(4096);
        if (o->tag) rw_recipe_tag(r, o->tag, 4096);
        o->jf = fopen(job->out_jpg, "wb");
        if (!o->jf) { sprintf(err, "cannot create %.200s", job->out_jpg); return 0; }
        setvbuf(o->jf, 0, _IOFBF, IO_BUF);
        o->je = jpeg_begin(o->w2, o->h2, job->jpeg_quality > 0 ? job->jpeg_quality : 92, file_write, o->jf,
                           o->tag, o->tag ? (unsigned)strlen(o->tag) : 0);
        if (!o->je) { strcpy(err, "not enough memory for the JPEG encoder"); return 0; }
    }
    // Both paths read the strip forward from here.
    if (fseek(o->in, (long)a->strip_off, SEEK_SET) != 0) { strcpy(err, "cannot seek to the raw data"); return 0; }
    return 1;
}

// One bent row (packed linear) out, beside the camera's own bytes for it.
static int out_row(out_t *o, unsigned y, const unsigned char *bent, const unsigned char *orig_bytes, char *err)
{
    const arw_info_t *a = o->a;
    unsigned short *lin = (y & 1) ? o->lin1 : o->lin0;
    unsigned x, w = o->w;

    unsigned long t = rw_ms(), t2;

    unpack_row(bent, lin, w);
    if (o->out)
    {
        // Back to codes. Where the linear value is untouched the original
        // code is kept, so a pixel the bend left alone round-trips exactly
        // even in the shadows, where two codes share one linear value.
        arw_decode_row(orig_bytes, w, o->orig);
        for (x = 0; x < w; x++)
            o->codes[x] = (a->lin[o->orig[x]] == lin[x]) ? o->orig[x] : a->code[lin[x]];
        arw_encode_row(o->codes, w, o->orig, orig_bytes, o->outrow);
        if (fwrite(o->outrow, 1, w, o->out) != w) { strcpy(err, "writing the new ARW failed - is the card full?"); return 0; }
    }
    t2 = rw_ms();
    t_arw += t2 - t;
    if ((y & 1) && (y >> 1) < o->h2)
    {
        dev_row(o->dev, o->lin0, o->lin1, o->w2, o->jrow);
        if (o->je) jpeg_rows(o->je, o->jrow, 1);
        shrink_row(&o->sp, y >> 1, o->jrow);
        t_dev += rw_ms() - t2;
    }
    return 1;
}

static int out_finish(out_t *o, char *err)
{
    const arw_info_t *a = o->a;

    shrink_emit(&o->sp);
    if (o->je)
    {
        int ok = jpeg_end(o->je);
        o->je = 0;
        if (!ok || fclose(o->jf) != 0) { o->jf = 0; strcpy(err, "writing the JPEG failed - is the card full?"); return 0; }
        o->jf = 0;
    }
    if (o->out)
    {
        jpeg_membuf_t m;
        unsigned char *thumb;
        unsigned len, tw = THUMB_W, th, y;

        if (!copy_bytes(o->in, o->out, a->strip_off + a->strip_len, a->file_len - (a->strip_off + a->strip_len), o->copybuf, 65536))
        { strcpy(err, "writing the new ARW failed - is the card full?"); return 0; }

        // The bent preview into the slot the camera's preview came out of, so
        // a raw viewer's thumbnail shows the bend. If no quality fits, the
        // original preview stays.
        memset(&m, 0, sizeof(m));
        len = jpeg_fit(o->sp.out, o->pw, o->ph, (unsigned)a->prev_len, &m);
        if (len) put_slot(o->out, a->prev_off, a->prev_len, a->prev_len_entry, m.buf, len);
        rw_log("preview %u bytes into a %lu byte slot", len, a->prev_len);

        // The thumbnail: letterboxed to 160x120, as Sony's is.
        th = (unsigned)((unsigned long)tw * o->ph / o->pw);
        thumb = (unsigned char *)calloc(THUMB_W * THUMB_H * 3, 1);
        if (thumb && th <= THUMB_H)
        {
            shrink_t st;
            if (shrink_init(&st, o->pw, o->ph, tw, th))
            {
                for (y = 0; y < o->ph; y++) shrink_row(&st, y, o->sp.out + (unsigned long)y * o->pw * 3);
                shrink_emit(&st);
                memcpy(thumb + (size_t)((THUMB_H - th) / 2) * THUMB_W * 3, st.out, (size_t)tw * th * 3);
                len = jpeg_fit(thumb, THUMB_W, THUMB_H, (unsigned)a->thumb_len, &m);
                if (len) put_slot(o->out, a->thumb_off, a->thumb_len, a->thumb_len_entry, m.buf, len);
            }
            shrink_free(&st);
        }
        free(thumb);
        free(m.buf);

        if (fclose(o->out) != 0) { o->out = 0; strcpy(err, "closing the new ARW failed - is the card full?"); return 0; }
        o->out = 0;
    }
    return 1;
}

static void out_free(out_t *o)
{
    if (o->je) jpeg_end(o->je);
    if (o->jf) fclose(o->jf);
    if (o->out) fclose(o->out);
    shrink_free(&o->sp);
    free(o->lin0); free(o->lin1); free(o->codes); free(o->orig);
    free(o->outrow); free(o->jrow); free(o->copybuf); free(o->dev); free(o->tag);
    memset(o, 0, sizeof(*o));
}

//-------------------------------------------------------------------

static int compile_segs(rw_recipe_t *r, seg_cc_t **cc, int n, char *err)
{
    int i;
    for (i = 0; i < n; i++)
    {
        cc[i] = (seg_cc_t *)malloc(sizeof(seg_cc_t));
        if (!cc[i]) { strcpy(err, "not enough memory for the matrix"); return 0; }
        cc[i]->b = rw_recipe_seg(r, i);
        bend_sanitize(cc[i]->b, RW_NBITS);
        bend_compile(cc[i]->b, &cc[i]->cc, cc[i]->lut, RW_NBITS);
    }
    return 1;
}

int rw_process(const rw_job_t *job, const rw_recipe_t *recipe, char *err)
{
    static arw_info_t a;                    // 12KB of tables; not on the stack
    rw_recipe_t r = *recipe;
    frame_t fr;
    out_t o;
    FILE *in = 0;
    seg_cc_t *cc[BEND_SEG_MAX] = { 0, 0, 0, 0 };
    unsigned char *rowbuf = 0, *origring = 0, *scratch = 0, *jpg_env = 0, *rom_env = 0;
    unsigned short *codes = 0, *lin = 0;
    unsigned long jpg_env_len = 0, rom_env_len = 0, t0 = rw_ms();
    unsigned y, x, w, h, ob_x;
    int rc = -1, i, n, nx, framed, steps, step = 0;

    memset(&fr, 0, sizeof(fr));
    memset(&o, 0, sizeof(o));
    err[0] = 0;
    rw_progress = 0;
    rw_stage = RW_STAGE_READ;

    rw_recipe_sanitize(&r);
    n  = r.bend_on ? bend_seg_count(r.segs.layout) : 0;
    nx = r.bendx_on ? bendx_chain_count(&r.bendx) : 0;
    framed = nx > 0 || job->force_frame;

    rw_log("shot %u: %s", job->frame, job->in_arw);
    rw_log("recipe: matrix %s, %d segment(s), %d effect(s), %s path", r.bend_on ? "on" : "off", n, nx,
           framed ? "whole-frame" : "streamed");
    rw_log_mem("at start");

    in = fopen(job->in_arw, "rb");
    if (!in) { sprintf(err, "cannot open %.200s", job->in_arw); goto done; }
    setvbuf(in, 0, _IOFBF, IO_BUF);
    t_read = t_bend = t_arw = t_dev = 0;
    if (arw_open(in, &a, err) != 0) goto done;
    rw_log("arw %s %ux%u, SR2 %s, black %u white %u", a.model, a.width, a.height,
           a.has_sr2 ? "read" : "missing", a.black, a.white);

    w = a.width; h = a.height;
    // The a6000's strip is 6048 wide for a 6024 picture; the margin on the
    // right is where the optical-black bus is sampled.
    ob_x = w - 8;
    fr.w = w; fr.h = h; fr.rowlen = w * 3 / 2;
    fr.ring = framed ? 0 : RING_ROWS;

    rowbuf = (unsigned char *)malloc(w);
    codes  = (unsigned short *)malloc(w * sizeof(unsigned short));
    lin    = (unsigned short *)malloc(w * sizeof(unsigned short));
    if (!rowbuf || !codes || !lin || (n && !compile_segs(&r, cc, n, err)))
    { if (!err[0]) strcpy(err, "not enough memory"); goto done; }

    rw_log("allocating %lu KB for the %s", (unsigned long)fr.rowlen * (fr.ring ? fr.ring : h) / 1024,
           framed ? "whole frame" : "row ring");
    fr.base = (unsigned char *)malloc((size_t)fr.rowlen * (fr.ring ? fr.ring : h));
    if (!framed) origring = (unsigned char *)malloc((size_t)w * RING_ROWS);
    if (!fr.base || (!framed && !origring))
    {
        sprintf(err, "not enough memory for a %ux%u frame (%lu MB) - try the matrix without effects, which needs far less",
                w, h, (unsigned long)fr.rowlen * h >> 20);
        goto done;
    }
    rw_log_mem("after allocating");

    o.a = &a; o.in = in;

    if (!framed)
    {
        //-----------------------------------------------------------
        // Streamed: read ahead, bend, write, one row at a time.
        unsigned loaded = 0;
        int ok = 1;

        bend_seg_set_seed(job->seed);
        prog_set(RW_STAGE_BEND, 0, 1000, h);
        if (!out_begin(&o, job, &r, err)) goto done;
        for (y = 0; y < h && ok; y++)
        {
            unsigned long t = rw_ms();
            while (loaded < h && loaded < y + RING_ROWS / 2)
            {
                unsigned char *ob = origring + (unsigned long)(loaded % RING_ROWS) * w;
                if (fread(ob, 1, w, in) != w) { strcpy(err, "the raw data ends early - is the file complete?"); goto done; }
                arw_decode_row(ob, w, codes);
                for (x = 0; x < w; x++) lin[x] = a.lin[codes[x]];
                pack_row(frow(&fr, loaded), lin, w);
                loaded++;
            }
            t_read += rw_ms() - t;
            t = rw_ms();
            for (i = 0; i < n; i++)
                bend_row(&fr, cc[i], r.segs.layout, r.segs.size, i, y, job->frame, ob_x);
            t_bend += rw_ms() - t;
            rw_stage = RW_STAGE_WRITE;
            ok = out_row(&o, y, frow(&fr, y), origring + (unsigned long)(y % RING_ROWS) * w, err);
            if ((y & 63) == 0) prog_rows_done(y);
        }
        if (!ok) goto done;
        rw_log("bent and written in %lu ms", rw_ms() - t0);
    }
    else
    {
        //-----------------------------------------------------------
        // Whole frame: read it all, the matrix pass by pass, the chain,
        // then write.
        bendx_buf_t buf;
        bendx_env_t env;

        steps = 2 + n + nx;
        prog_set(RW_STAGE_READ, 0, 1000 / steps, h);
        if (fseek(in, (long)a.strip_off, SEEK_SET) != 0) { strcpy(err, "cannot seek to the raw data"); goto done; }
        for (y = 0; y < h; y++)
        {
            if (fread(rowbuf, 1, w, in) != w) { strcpy(err, "the raw data ends early - is the file complete?"); goto done; }
            arw_decode_row(rowbuf, w, codes);
            for (x = 0; x < w; x++) lin[x] = a.lin[codes[x]];
            pack_row(frow(&fr, y), lin, w);
            if ((y & 63) == 0) prog_rows_done(y);
        }
        step++;
        rw_log("read in %lu ms", rw_ms() - t0);

        bend_seg_set_seed(job->seed);
        for (i = 0; i < n; i++)
        {
            prog_set(RW_STAGE_BEND, 1000 * step / steps, 1000 / steps, h);
            for (y = 0; y < h; y++)
            {
                bend_row(&fr, cc[i], r.segs.layout, r.segs.size, i, y, job->frame, ob_x);
                if ((y & 63) == 0) prog_rows_done(y);
            }
            step++;
        }

        buf.base = fr.base; buf.rowlen = fr.rowlen; buf.rowpix = w; buf.rows = h;
        buf.ob_x = ob_x; buf.nbits = RW_NBITS;

        memset(&env, 0, sizeof(env));
        // The JPEG bus reads the camera's own JPEG encoder output: the preview
        // it embedded in this very file.
        if (a.prev_off && a.prev_len && a.prev_len < 0x400000 && (jpg_env = (unsigned char *)malloc(a.prev_len)) != 0)
        {
            if (fseek(in, (long)a.prev_off, SEEK_SET) == 0 && fread(jpg_env, 1, a.prev_len, in) == a.prev_len)
                jpg_env_len = a.prev_len;
        }
        rom_env = read_whole(job->rom_path, ROM_MAX, &rom_env_len);
        env.jpg = jpg_env_len ? jpg_env : 0; env.jpg_len = (unsigned)jpg_env_len;
        env.rom = rom_env; env.rom_len = (unsigned)rom_env_len;
        env.frame = job->frame;
        env.black = a.black;
        env.white = a.white;
        env.g_even = 0;             // RGGB
        env.r_odd = 0;
        // Daylight white in raw terms, green = 1000, from the same matrix the
        // developer uses: the reciprocal of the D65 white-balance multipliers.
        env.neutral[0] = 345; env.neutral[1] = 1000; env.neutral[2] = 742;

        scratch = (unsigned char *)malloc(bendx_chain_scratch_bytes(&r.bendx, fr.rowlen));
        if (!scratch) { strcpy(err, "not enough memory for the experimental chain"); goto done; }

        bendx_set_service(prog_service);
        for (i = 0; i < nx; i++)
        {
            prog_set(RW_STAGE_BENDX, 1000 * step / steps, 1000 / steps, h);
            rw_log("effect %d: %s (%lu ms in)", i + 1, bendx_name(r.bendx.item[i].kind), rw_ms() - t0);
            bendx_apply(&r.bendx.item[i], &buf, &env, scratch);
            step++;
        }
        bendx_set_service(0);
        free(jpg_env); jpg_env = 0;
        free(rom_env); rom_env = 0;
        free(scratch); scratch = 0;

        prog_set(RW_STAGE_WRITE, 1000 * step / steps, 1000 - 1000 * step / steps, h);
        rw_log("writing (%lu ms in)", rw_ms() - t0);
        if (!out_begin(&o, job, &r, err)) goto done;
        for (y = 0; y < h; y++)
        {
            if (fread(rowbuf, 1, w, in) != w) { strcpy(err, "the raw data ends early"); goto done; }
            if (!out_row(&o, y, frow(&fr, y), rowbuf, err)) goto done;
            if ((y & 63) == 0) prog_rows_done(y);
        }
    }

    {
        unsigned long t = rw_ms();
        if (!out_finish(&o, err)) goto done;
        rw_log("time: read %lu ms, matrix %lu ms, ARW encode+write %lu ms, develop+JPEG %lu ms, finish %lu ms",
               t_read, t_bend, t_arw, t_dev, rw_ms() - t);
    }

    if (job->out_bnd)
    {
        unsigned char rec[RW_REC_MAX];
        int len = rw_recipe_pack(&r, rec, sizeof(rec));
        FILE *bf = fopen(job->out_bnd, "wb");
        if (bf) { fwrite(rec, 1, (size_t)len, bf); fclose(bf); }
    }

    rw_progress = 1000;
    rw_stage = RW_STAGE_DONE;
    rc = 0;
    rw_log("done in %lu ms", rw_ms() - t0);

done:
    bendx_set_service(0);
    out_free(&o);
    if (in) fclose(in);
    if (rc != 0)
    {
        rw_log("FAILED: %s", err);
        // Never leave half a file behind for a person to find on the card.
        if (job->out_arw) remove(job->out_arw);
        if (job->out_jpg) remove(job->out_jpg);
        rw_stage = RW_STAGE_IDLE;
    }
    for (i = 0; i < BEND_SEG_MAX; i++) free(cc[i]);
    free(fr.base); free(origring); free(rowbuf); free(codes); free(lin);
    free(scratch); free(jpg_env); free(rom_env);
    return rc;
}
