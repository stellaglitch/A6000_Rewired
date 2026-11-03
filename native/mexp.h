#ifndef REWIRED_MEXP_H
#define REWIRED_MEXP_H

//-------------------------------------------------------------------
// Multiple exposure: CHDK_Rewired's double exposure (core/mexp.c there), for
// a camera that has already written every exposure to the card.
//
// A PowerShot has one raw buffer and no room for a second, so CHDK_Rewired
// keeps a running accumulator file and folds each exposure into it inside the
// raw hook. Here every exposure is an ARW on the card already, so nothing is
// accumulated between shots: the last shot opens the earlier ones beside it
// and the rows are combined as they are read, one row of each file at a time.
// That costs one extra row decode per exposure and no memory worth the name,
// and it lets the average be a true mean of all of them rather than a running
// one rounded at every step.
//
// The same four modes, named for what they do to the light:
//   ADD      the exposures summed, the black pedestal counted once - highlights
//            build and clip
//   LIGHTEN  the brighter pixel - lights on black
//   DARKEN   the darker pixel - the inverse
//   AVERAGE  the mean - the classic ghost
//
// The values combined are 12-bit linear, the sensor's own light, so the modes
// mean what they say; blending the companded codes would not.
//-------------------------------------------------------------------

#include <stdio.h>
#include "arw.h"

#define RW_MEXP_ADD        0
#define RW_MEXP_LIGHTEN    1
#define RW_MEXP_DARKEN     2
#define RW_MEXP_AVERAGE    3
#define RW_MEXP_MODES      4

// Exposures in a sequence, the last one included: CHDK_Rewired's range.
#define RW_MEXP_MIN        2
#define RW_MEXP_MAX        9

const char *rw_mexp_name(int mode);

// Combine n rows of w linear values into dst, which may be rows[0].
void rw_mexp_blend(int mode, unsigned short *dst, const unsigned short *const *rows, int n,
                   unsigned w, unsigned black, unsigned white);

// The earlier exposures, open and read forward a row at a time alongside the
// file being bent.
typedef struct
{
    int n, mode;                        // n earlier files; 0 = not a multiple exposure
    FILE *f[RW_MEXP_MAX - 1];
    arw_info_t *a[RW_MEXP_MAX - 1];
    unsigned short *lin[RW_MEXP_MAX - 1];
    unsigned char *bytes;
    unsigned short *codes;
} rw_mexp_t;

// Open the n earlier exposures. Each must be the same size as base. Returns 1,
// or 0 with err (>= 256 bytes) saying why.
int rw_mexp_open(rw_mexp_t *m, const char *const *paths, int n, int mode, const arw_info_t *base, char *err);

// Read the next row of every earlier exposure and blend it into lin[0..w),
// the current file's row, in place.
int rw_mexp_row(rw_mexp_t *m, unsigned short *lin, unsigned w, unsigned black, unsigned white, char *err);

void rw_mexp_close(rw_mexp_t *m);

#endif
