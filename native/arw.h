#ifndef REWIRED_ARW_H
#define REWIRED_ARW_H

//-------------------------------------------------------------------
// Sony ARW, as the a6000 writes it.
//
// An ARW is a TIFF. IFD0 carries the 1616x1080 preview JPEG, IFD1 the 160x120
// thumbnail, and IFD0's SubIFD the raw itself:
//
//   0x0100/0x0101   6048 x 4024, wider than the 6024 picture by a black margin
//   0x0103          32767, Sony's compressed raw ("cRAW")
//   0x0111/0x0117   one strip, exactly width * height bytes
//   0x7010          the tone curve the 11-bit codes were companded through
//
// cRAW packs each row in 16-byte blocks. A block holds sixteen pixels of one
// colour - every other column, so two neighbouring blocks interleave across 32
// columns - as an 11-bit max, an 11-bit min, the 4-bit index of each, and
// fourteen 7-bit deltas above min, shifted left by however much the range
// needs. Every block is exactly 16 bytes whatever is in it, which is what makes
// this whole project possible: a bent block re-encodes to the same size, so the
// strip can be rewritten in place and every offset in the file stays true.
//
// The codes are companded. The bend engine wants the sensor's linear values -
// that is what the bus between a sensor and its ADC carries, and what CHDK
// bends on a PowerShot - so the codes are taken through the tone curve to
// 12-bit linear on the way in and back through its inverse on the way out.
//
// White balance, the black level and the white level are not in the plain
// TIFF. Sony keeps them in SR2Private, a block encrypted with a key stored in
// the clear beside it; arw_open() decrypts it and reads them out. A file
// without it still bends - those only matter for developing a picture.
//-------------------------------------------------------------------

#include <stdio.h>

#define ARW_BLOCK_PIX   16
#define ARW_BLOCK_BYTES 16

typedef struct
{
    // geometry of the raw strip
    unsigned width, height;         // pixels; width is a multiple of 32
    unsigned long strip_off;        // file offset of the first row
    unsigned long strip_len;        // width * height

    // the companding curve, from 0x7010; a6000 default if absent
    unsigned curve_pts[6];
    unsigned short lin[2048];       // 11-bit code -> 12-bit linear
    unsigned short code[4096];      // 12-bit linear -> nearest 11-bit code

    // from SR2Private, in 12-bit linear units; defaults if absent
    unsigned black, white;
    unsigned wb[4];                 // R, G, G, B multipliers, G = 1024
    int has_sr2;

    // the embedded JPEGs: where they are, how much room they have, and the
    // file offset of the IFD entry holding each one's byte count
    unsigned long prev_off, prev_len, prev_len_entry;
    unsigned long thumb_off, thumb_len, thumb_len_entry;

    char model[32];
    unsigned long file_len;
} arw_info_t;

// Parse the header and the private block. Returns 0 on success, or a negative
// code with a readable message in err (>= 128 bytes).
int arw_open(FILE *f, arw_info_t *a, char *err);

// One row of the strip. codes[] gets width 11-bit codes in column order.
void arw_decode_row(const unsigned char *src, unsigned width, unsigned short *codes);

// Encode one row back. Blocks whose sixteen codes are all unchanged from
// orig_codes are copied byte for byte from orig_bytes, so an untouched part of
// the picture comes out bit-identical rather than through a lossy re-encode.
void arw_encode_row(const unsigned short *codes, unsigned width,
                    const unsigned short *orig_codes, const unsigned char *orig_bytes,
                    unsigned char *dst);

// The companding, as tables. arw_open() fills them; exposed so the host test
// can check that every code survives the trip to linear and back.
void arw_build_curve(arw_info_t *a);

#endif
