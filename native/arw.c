//-------------------------------------------------------------------
// Sony ARW reader and cRAW codec - see arw.h.
//
// The decoder is the one every raw developer uses (dcraw's
// sony_arw2_load_raw), so a block this writes is read back the same way by
// Lightroom, darktable, RawTherapee and the rest. The encoder is its inverse,
// written to the same bit layout.
//-------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include "arw.h"

//-------------------------------------------------------------------
// Little-endian helpers. ARW is always "II".

static unsigned g16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long g32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int rd(FILE *f, unsigned long off, void *buf, unsigned long n)
{
    if (fseek(f, (long)off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, n, f) == n;
}

static unsigned tsize(unsigned type)
{
    static const unsigned char sz[14] = { 0,1,1,2,4,8,1,1,2,4,8,4,8,4 };
    return type < 14 ? sz[type] : 1;
}

//-------------------------------------------------------------------
// One IFD, read whole into memory. Entries are 12 bytes; the value is inline
// when it fits in four, otherwise the four bytes are its offset.

#define IFD_MAX 256

typedef struct
{
    unsigned long off;              // where the IFD itself is
    unsigned      n;
    unsigned char e[IFD_MAX * 12];
    unsigned long next;
} ifd_t;

static int ifd_read(FILE *f, unsigned long off, ifd_t *d)
{
    unsigned char b[2], nx[4];
    if (!off || !rd(f, off, b, 2)) return 0;
    d->off = off;
    d->n = g16(b);
    if (d->n == 0 || d->n > IFD_MAX) return 0;
    if (!rd(f, off + 2, d->e, d->n * 12)) return 0;
    d->next = rd(f, off + 2 + d->n * 12, nx, 4) ? g32(nx) : 0;
    return 1;
}

// Index of a tag's entry, or -1.
static int ifd_find(const ifd_t *d, unsigned tag)
{
    unsigned i;
    for (i = 0; i < d->n; i++)
        if (g16(d->e + i * 12) == tag) return (int)i;
    return -1;
}

// First value of a SHORT or LONG tag, or def.
static unsigned long ifd_val(const ifd_t *d, unsigned tag, unsigned long def)
{
    int i = ifd_find(d, tag);
    const unsigned char *e;
    if (i < 0) return def;
    e = d->e + i * 12;
    switch (g16(e + 2))
    {
        case 3: case 8: return g16(e + 8);
        case 4: case 9: case 13: return g32(e + 8);
        case 1: case 7: return e[8];
    }
    return def;
}

// File offset of a tag's value: inline (inside the entry) or out of line.
static unsigned long ifd_valoff(const ifd_t *d, unsigned tag, unsigned long *count)
{
    int i = ifd_find(d, tag);
    const unsigned char *e;
    unsigned long cnt;
    if (i < 0) return 0;
    e = d->e + i * 12;
    cnt = g32(e + 4);
    if (count) *count = cnt;
    if (tsize(g16(e + 2)) * cnt <= 4) return d->off + 2 + (unsigned long)i * 12 + 8;
    return g32(e + 8);
}

//-------------------------------------------------------------------
// The tone curve.
//
// Four knee points in 0x7010, in 14-bit units. Between knees the slope
// doubles: 1, 2, 4, 8, 16. dcraw builds the same table and reads a pixel as
// curve[code << 1] >> 2, which is 12-bit linear with the a6000's black at 128.

void arw_build_curve(arw_info_t *a)
{
    static unsigned curve[4096 + 2];
    unsigned i, j, v;
    int best, bd, d;

    for (i = 0; i < 4096; i++) curve[i] = i;
    for (i = 0; i < 5; i++)
        for (j = a->curve_pts[i] + 1; j <= a->curve_pts[i + 1] && j < 4096; j++)
            curve[j] = curve[j - 1] + (1u << i);

    for (v = 0; v < 2048; v++)
    {
        unsigned l = curve[v << 1] >> 2;
        a->lin[v] = (unsigned short)(l > 4095 ? 4095 : l);
    }

    // Inverse by nearest code. lin[] is monotone, so a walking pointer finds
    // it in one pass. Ties go to the lower code, which is the one dcraw's
    // own reading of a shadow value lands on.
    best = 0;
    for (v = 0; v < 4096; v++)
    {
        while (best < 2047 && a->lin[best + 1] <= v) best++;
        bd = best;
        if (best < 2047)
        {
            d = (int)a->lin[best + 1] - (int)v;
            if (d < (int)v - (int)a->lin[best]) bd = best + 1;
        }
        a->code[v] = (unsigned short)bd;
    }
}

//-------------------------------------------------------------------
// SR2Private. Sony's cipher is a lagged-Fibonacci keystream XORed over the
// block a 32-bit word at a time - the same routine dcraw calls sony_decrypt.

static void sr2_decrypt(unsigned char *buf, unsigned long len, unsigned long key)
{
    unsigned long pad[128];
    unsigned p, w, words = (unsigned)(len / 4);

    for (p = 0; p < 4; p++)
        pad[p] = key = (key * 48828125UL + 1) & 0xffffffffUL;
    pad[3] = ((pad[3] << 1) | ((pad[0] ^ pad[2]) >> 31)) & 0xffffffffUL;
    for (p = 4; p < 127; p++)
        pad[p] = (((pad[p - 4] ^ pad[p - 2]) << 1) | ((pad[p - 3] ^ pad[p - 1]) >> 31)) & 0xffffffffUL;

    // The keystream is defined on big-endian words and the data on
    // little-endian ones; dcraw gets that with an htonl over the pad. Doing
    // the XOR bytewise in big-endian order is the same thing on any host.
    for (w = 0; w < words; w++, p++)
    {
        unsigned long k = pad[p & 127] = pad[(p + 1) & 127] ^ pad[(p + 65) & 127];
        unsigned char *q = buf + w * 4;
        q[0] ^= (unsigned char)(k >> 24);
        q[1] ^= (unsigned char)(k >> 16);
        q[2] ^= (unsigned char)(k >> 8);
        q[3] ^= (unsigned char)k;
    }
}

static void read_sr2(FILE *f, const ifd_t *ifd0, arw_info_t *a)
{
    unsigned char ob[4], *buf = 0;
    unsigned long sr2_ifd, off, len, key, cnt;
    unsigned i, n;
    ifd_t sr2;

    off = ifd_valoff(ifd0, 0xc634, &cnt);       // DNGPrivateData -> SR2Private IFD
    if (!off || !rd(f, off, ob, 4)) return;
    sr2_ifd = g32(ob);
    if (!ifd_read(f, sr2_ifd, &sr2)) return;

    off = ifd_val(&sr2, 0x7200, 0);
    len = ifd_val(&sr2, 0x7201, 0);
    key = ifd_val(&sr2, 0x7221, 0);
    if (!off || len < 2 || len > 0x40000) return;

    if (ifd_find(&sr2, 0x7221) >= 0)
    {
        // The key is a LONG stored out of line on some bodies.
        unsigned long ko = ifd_valoff(&sr2, 0x7221, 0);
        unsigned char kb[4];
        if (ko && rd(f, ko, kb, 4)) key = g32(kb);
    }

    buf = (unsigned char *)malloc(len);
    if (!buf) return;
    if (!rd(f, off, buf, len)) { free(buf); return; }
    sr2_decrypt(buf, len, key);

    // An IFD at the start of the block, whose out-of-line offsets are file
    // offsets - so subtract where the block came from.
    n = g16(buf);
    if (n == 0 || n > 400 || 2 + n * 12UL > len) { free(buf); return; }
    for (i = 0; i < n; i++)
    {
        const unsigned char *e = buf + 2 + i * 12;
        unsigned tag = g16(e), type = g16(e + 2);
        unsigned long c = g32(e + 4), vo;
        const unsigned char *v;
        if (tsize(type) * c <= 4) v = e + 8;
        else
        {
            vo = g32(e + 8);
            if (vo < off || vo - off + tsize(type) * c > len) continue;
            v = buf + (vo - off);
        }
        // SHORT or SSHORT: the a6000 stores its WB levels signed.
        if ((type != 3 && type != 8) || c < 3) continue;
        if (tag == 0x7310 && c >= 4)        // black level, RGGB, 14-bit
            a->black = (g16(v) + g16(v + 2) + g16(v + 4) + g16(v + 6)) / 16;
        else if (tag == 0x787f)             // white level, 14-bit
        {
            unsigned w = g16(v) >> 2;
            if (w > 1024 && w <= 4095) a->white = w;
        }
        else if (c < 4) continue;
        else if (tag == 0x7303)     // WB as G R B G
        {
            a->wb[1] = g16(v); a->wb[0] = g16(v + 2); a->wb[3] = g16(v + 4); a->wb[2] = g16(v + 6);
            a->has_sr2 = 1;
        }
        else if (tag == 0x7313)     // WB as R G G B
        {
            a->wb[0] = g16(v); a->wb[1] = g16(v + 2); a->wb[2] = g16(v + 4); a->wb[3] = g16(v + 6);
            a->has_sr2 = 1;
        }
    }
    free(buf);
}

//-------------------------------------------------------------------

int arw_open(FILE *f, arw_info_t *a, char *err)
{
    unsigned char hdr[8];
    ifd_t ifd0, ifd1, raw;
    unsigned long sub, cnt, o;
    int found = 0;

    memset(a, 0, sizeof(*a));
    strcpy(err, "");

    if (fseek(f, 0, SEEK_END) != 0) { strcpy(err, "cannot seek"); return -1; }
    a->file_len = (unsigned long)ftell(f);

    if (!rd(f, 0, hdr, 8) || hdr[0] != 'I' || hdr[1] != 'I' || g16(hdr + 2) != 42)
    { strcpy(err, "not a little-endian TIFF, so not an ARW"); return -1; }

    if (!ifd_read(f, g32(hdr + 4), &ifd0)) { strcpy(err, "IFD0 unreadable"); return -1; }

    o = ifd_valoff(&ifd0, 0x0110, &cnt);
    if (o && cnt > 0)
    {
        if (cnt > sizeof(a->model) - 1) cnt = sizeof(a->model) - 1;
        rd(f, o, a->model, cnt);
        a->model[cnt] = 0;
    }

    // The preview, and the IFD entry that records its length. The entry is
    // what gets rewritten when a bent preview goes in.
    a->prev_off = ifd_val(&ifd0, 0x0201, 0);
    a->prev_len = ifd_val(&ifd0, 0x0202, 0);
    a->prev_len_entry = a->prev_len ? ifd_valoff(&ifd0, 0x0202, 0) : 0;

    if (ifd_read(f, ifd0.next, &ifd1))
    {
        a->thumb_off = ifd_val(&ifd1, 0x0201, 0);
        a->thumb_len = ifd_val(&ifd1, 0x0202, 0);
        a->thumb_len_entry = a->thumb_len ? ifd_valoff(&ifd1, 0x0202, 0) : 0;
    }

    // The raw IFD: a SubIFD of IFD0 with Sony's compression.
    sub = ifd_valoff(&ifd0, 0x014a, &cnt);
    if (sub)
    {
        unsigned long k, n = cnt > 8 ? 8 : cnt;
        for (k = 0; k < n && !found; k++)
        {
            unsigned char b[4];
            unsigned long so = (cnt == 1) ? ifd_val(&ifd0, 0x014a, 0) : (rd(f, sub + k * 4, b, 4) ? g32(b) : 0);
            if (ifd_read(f, so, &raw) && ifd_val(&raw, 0x0103, 0) == 32767) found = 1;
        }
    }
    if (!found) { strcpy(err, "no Sony compressed raw in this file"); return -2; }

    a->width     = (unsigned)ifd_val(&raw, 0x0100, 0);
    a->height    = (unsigned)ifd_val(&raw, 0x0101, 0);
    a->strip_off = ifd_val(&raw, 0x0111, 0);
    a->strip_len = ifd_val(&raw, 0x0117, 0);

    if (ifd_val(&raw, 0x0102, 0) != 12 && ifd_val(&raw, 0x0102, 0) != 8)
    { strcpy(err, "this ARW is not the compressed kind (uncompressed raw is not supported)"); return -3; }
    if (a->width < 64 || a->width % 32 || a->height < 16 ||
        a->strip_len != (unsigned long)a->width * a->height ||
        a->strip_off + a->strip_len > a->file_len)
    { strcpy(err, "raw strip geometry is not cRAW"); return -3; }

    // Curve knees, 14-bit, to the 12-bit table dcraw builds.
    a->curve_pts[0] = 0;
    a->curve_pts[5] = 4095;
    o = ifd_valoff(&raw, 0x7010, &cnt);
    if (o && cnt >= 4)
    {
        unsigned char b[8]; int i;
        rd(f, o, b, 8);
        for (i = 0; i < 4; i++) a->curve_pts[i + 1] = (g16(b + i * 2) >> 2) & 0xfff;
    }
    else
    {
        a->curve_pts[1] = 2000; a->curve_pts[2] = 2600;
        a->curve_pts[3] = 3225; a->curve_pts[4] = 3525;
    }
    arw_build_curve(a);

    // Defaults for the a6000, overwritten by SR2Private when it is readable.
    a->black = 128;
    a->white = 4075;
    a->wb[0] = 2600; a->wb[1] = a->wb[2] = 1024; a->wb[3] = 1600;
    read_sr2(f, &ifd0, a);
    if (a->wb[1] == 0) a->wb[1] = 1024;
    if (a->wb[2] == 0) a->wb[2] = a->wb[1];
    return 0;
}

//-------------------------------------------------------------------
// The block codec.

void arw_decode_row(const unsigned char *src, unsigned width, unsigned short *codes)
{
    const unsigned char *dp = src;
    unsigned col = 0;

    // dcraw's bound: the odd block of the last pair starts at width - 31.
    while (col + 30 < width)
    {
        unsigned long val = g32(dp);
        unsigned max  = val & 0x7ff;
        unsigned min  = (val >> 11) & 0x7ff;
        unsigned imax = (val >> 22) & 0x0f;
        unsigned imin = (val >> 26) & 0x0f;
        unsigned sh, bit, i;

        for (sh = 0; sh < 4 && (0x80u << sh) <= max - min; sh++) ;
        for (bit = 30, i = 0; i < 16; i++, col += 2)
        {
            unsigned pix;
            if (i == imax) pix = max;
            else if (i == imin) pix = min;
            else
            {
                pix = (((g16(dp + (bit >> 3)) >> (bit & 7)) & 0x7f) << sh) + min;
                if (pix > 0x7ff) pix = 0x7ff;
                bit += 7;
            }
            codes[col] = (unsigned short)pix;
        }
        col -= (col & 1) ? 1 : 31;
        dp += 16;
    }
}

static void put_bits(unsigned char *b, unsigned pos, unsigned n, unsigned v)
{
    unsigned i;
    for (i = 0; i < n; i++, pos++)
        if (v & (1u << i)) b[pos >> 3] |= (unsigned char)(1u << (pos & 7));
}

static void encode_block(const unsigned short *pix, unsigned char *out)
{
    unsigned max = 0, min = 0x7ff, imax = 0, imin = 0, sh, bit, i;

    for (i = 0; i < 16; i++)
    {
        if (pix[i] > max) { max = pix[i]; imax = i; }
        if (pix[i] < min) { min = pix[i]; imin = i; }
    }
    // The decoder tests imax before imin and only skips a delta for the one it
    // matched, so the two must differ - a flat block names two positions.
    if (imax == imin) { imax = 0; imin = 1; }

    for (sh = 0; sh < 4 && (0x80u << sh) <= max - min; sh++) ;

    memset(out, 0, 16);
    put_bits(out, 0, 11, max);
    put_bits(out, 11, 11, min);
    put_bits(out, 22, 4, imax);
    put_bits(out, 26, 4, imin);
    for (bit = 30, i = 0; i < 16; i++)
    {
        unsigned d;
        if (i == imax || i == imin) continue;
        d = pix[i] - min;
        if (sh) d = (d + (1u << (sh - 1))) >> sh;
        if (d > 0x7f) d = 0x7f;
        put_bits(out, bit, 7, d);
        bit += 7;
    }
}

void arw_encode_row(const unsigned short *codes, unsigned width,
                    const unsigned short *orig_codes, const unsigned char *orig_bytes,
                    unsigned char *dst)
{
    unsigned base, half, i;
    unsigned short pix[16];

    // Blocks come in pairs over 32 columns: the even columns, then the odd.
    for (base = 0; base + 31 < width; base += 32)
        for (half = 0; half < 2; half++)
        {
            unsigned blk = (base / 32) * 2 + half;
            int same = 1;
            for (i = 0; i < 16; i++)
            {
                unsigned c = base + half + i * 2;
                pix[i] = codes[c];
                if (codes[c] != orig_codes[c]) same = 0;
            }
            if (same)
                memcpy(dst + blk * 16, orig_bytes + blk * 16, 16);
            else
                encode_block(pix, dst + blk * 16);
        }
}
