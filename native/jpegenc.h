#ifndef REWIRED_JPEGENC_H
#define REWIRED_JPEGENC_H

//-------------------------------------------------------------------
// A baseline JPEG encoder that takes its picture a row at a time.
//
// The developed frame is 3024 x 2012 and the camera has no room to hold it
// whole as a bitmap, so the encoder keeps one 8-row strip and nothing else.
// 4:4:4, the standard Annex K tables scaled by quality, one optional comment
// segment after the SOI. Output goes through a callback, so the same encoder
// writes to a file or to memory.
//-------------------------------------------------------------------

typedef int (*jpeg_write_fn)(void *ctx, const unsigned char *buf, unsigned len);

typedef struct jpeg_enc jpeg_enc_t;

// quality 1..100. comment may be null. Returns null if out of memory.
jpeg_enc_t *jpeg_begin(unsigned w, unsigned h, int quality,
                       jpeg_write_fn out, void *ctx,
                       const char *comment, unsigned comment_len);

// rgb holds n rows of w RGB triplets.
void jpeg_rows(jpeg_enc_t *e, const unsigned char *rgb, unsigned n);

// Pads a short last strip, writes the EOI, frees e. Returns 1 if every write
// succeeded.
int jpeg_end(jpeg_enc_t *e);

// Convenience: an in-memory sink that grows.
typedef struct { unsigned char *buf; unsigned len, cap; int failed; } jpeg_membuf_t;
int jpeg_mem_write(void *ctx, const unsigned char *buf, unsigned len);

#endif
