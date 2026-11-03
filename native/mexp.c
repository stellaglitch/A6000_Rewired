//-------------------------------------------------------------------
// Multiple exposure - see mexp.h.
//-------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include "mexp.h"

#define MEXP_IO_BUF     (64 * 1024)     // per earlier file; up to eight of them are open at once

const char *rw_mexp_name(int mode)
{
    static const char *names[RW_MEXP_MODES] = { "add", "lighten", "darken", "average" };
    return mode >= 0 && mode < RW_MEXP_MODES ? names[mode] : "?";
}

void rw_mexp_blend(int mode, unsigned short *dst, const unsigned short *const *rows, int n,
                   unsigned w, unsigned black, unsigned white)
{
    unsigned x;
    int i;

    // The mode test is outside the pixel loop, as CHDK_Rewired's
    // mexp_row_blend() has it: this runs 24 million times a shot.
    switch (mode)
    {
    case RW_MEXP_LIGHTEN:
        for (x = 0; x < w; x++)
        {
            unsigned r = rows[0][x];
            for (i = 1; i < n; i++) if (rows[i][x] > r) r = rows[i][x];
            dst[x] = (unsigned short)r;
        }
        break;

    case RW_MEXP_DARKEN:
        for (x = 0; x < w; x++)
        {
            unsigned r = rows[0][x];
            for (i = 1; i < n; i++) if (rows[i][x] < r) r = rows[i][x];
            dst[x] = (unsigned short)r;
        }
        break;

    case RW_MEXP_ADD:
        // The sum of the light, not of the numbers: every reading carries the
        // black pedestal, so it is counted once, the correction CHDK_Rewired
        // makes. Two exposures of a dark scene stay dark instead of going grey.
        for (x = 0; x < w; x++)
        {
            unsigned r = black;
            for (i = 0; i < n; i++) if (rows[i][x] > black) r += rows[i][x] - black;
            dst[x] = (unsigned short)(r > white ? white : r);
        }
        break;

    case RW_MEXP_AVERAGE:
    default:
        for (x = 0; x < w; x++)
        {
            unsigned r = 0;
            for (i = 0; i < n; i++) r += rows[i][x];
            dst[x] = (unsigned short)((r + (unsigned)n / 2) / (unsigned)n);
        }
        break;
    }
}

int rw_mexp_open(rw_mexp_t *m, const char *const *paths, int n, int mode, const arw_info_t *base, char *err)
{
    int i;

    memset(m, 0, sizeof(*m));
    if (n <= 0) return 1;
    if (n > RW_MEXP_MAX - 1) n = RW_MEXP_MAX - 1;
    m->mode = (mode >= 0 && mode < RW_MEXP_MODES) ? mode : RW_MEXP_AVERAGE;

    m->bytes = (unsigned char *)malloc(base->width);
    m->codes = (unsigned short *)malloc(base->width * sizeof(unsigned short));
    if (!m->bytes || !m->codes) { strcpy(err, "not enough memory for the multiple exposure"); return 0; }

    for (i = 0; i < n; i++)
    {
        m->a[i] = (arw_info_t *)malloc(sizeof(arw_info_t));
        m->lin[i] = (unsigned short *)malloc(base->width * sizeof(unsigned short));
        m->n = i + 1;                   // so a failure part way closes what is open
        if (!m->a[i] || !m->lin[i]) { strcpy(err, "not enough memory for the multiple exposure"); return 0; }
        m->f[i] = fopen(paths[i], "rb");
        if (!m->f[i]) { sprintf(err, "exposure %d of the sequence is gone: %.180s", i + 1, paths[i]); return 0; }
        setvbuf(m->f[i], 0, _IOFBF, MEXP_IO_BUF);
        if (arw_open(m->f[i], m->a[i], err) != 0) return 0;
        if (m->a[i]->width != base->width || m->a[i]->height != base->height)
        {
            sprintf(err, "exposure %d is %ux%u and this one %ux%u - keep the image size the same for the whole sequence",
                    i + 1, m->a[i]->width, m->a[i]->height, base->width, base->height);
            return 0;
        }
        if (fseek(m->f[i], (long)m->a[i]->strip_off, SEEK_SET) != 0)
        { sprintf(err, "cannot seek in exposure %d", i + 1); return 0; }
    }
    return 1;
}

int rw_mexp_row(rw_mexp_t *m, unsigned short *lin, unsigned w, unsigned black, unsigned white, char *err)
{
    const unsigned short *rows[RW_MEXP_MAX];
    unsigned x;
    int i;

    if (!m->n) return 1;
    rows[0] = lin;
    for (i = 0; i < m->n; i++)
    {
        const unsigned short *tab = m->a[i]->lin;
        if (fread(m->bytes, 1, w, m->f[i]) != w)
        { sprintf(err, "exposure %d of the sequence ends early", i + 1); return 0; }
        arw_decode_row(m->bytes, w, m->codes);
        for (x = 0; x < w; x++) m->lin[i][x] = tab[m->codes[x]];
        rows[i + 1] = m->lin[i];
    }
    rw_mexp_blend(m->mode, lin, rows, m->n + 1, w, black, white);
    return 1;
}

void rw_mexp_close(rw_mexp_t *m)
{
    int i;
    for (i = 0; i < m->n; i++)
    {
        if (m->f[i]) fclose(m->f[i]);
        free(m->a[i]);
        free(m->lin[i]);
    }
    free(m->bytes);
    free(m->codes);
    memset(m, 0, sizeof(*m));
}
