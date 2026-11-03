#ifndef REWIRED_PIPELINE_H
#define REWIRED_PIPELINE_H

//-------------------------------------------------------------------
// One shot, start to finish.
//
//   1. the camera's ARW is read and its cRAW decoded to 12-bit linear, into a
//      frame buffer laid out exactly as a PowerShot's raw buffer is (12bpp,
//      four pixels in six bytes) - so CHDK_Rewired's engines run on it
//      unchanged
//   2. the bend matrix, segment by segment, then the experimental chain
//   3. a new ARW: the input copied, its strip re-encoded in place, its
//      preview and thumbnail replaced with the bent picture
//   4. a JPEG, developed at half size from the bent raw, with the recipe in
//      its comment segment the way CHDK_Rewired tags its pictures
//
// The original is only ever read.
//-------------------------------------------------------------------

#include "recipe.h"

typedef struct
{
    const char *in_arw;         // the camera's file
    const char *out_arw;        // the bent raw; null to skip
    const char *out_jpg;        // the developed picture; null to skip
    const char *out_bnd;        // BSH1 sidecar; null to skip
    const char *rom_path;       // any file, read as the ROM bus; null for none
    int         jpeg_quality;   // for out_jpg
    unsigned    seed;           // re-rolls the pattern layouts
    unsigned    frame;          // shot counter, BUS_FRAME and drifting effects
    int         force_frame;    // tests only: hold the whole frame even for a matrix
} rw_job_t;

// Progress, for a UI polling from another thread. 0..1000.
extern volatile int rw_progress;
extern volatile int rw_stage;   // RW_STAGE_*

#define RW_STAGE_IDLE    0
#define RW_STAGE_READ    1
#define RW_STAGE_BEND    2
#define RW_STAGE_BENDX   3
#define RW_STAGE_WRITE   4
#define RW_STAGE_DONE    5

// Returns 0 on success. On failure err (>= 256 bytes) says why, in words a
// person can act on.
int rw_process(const rw_job_t *job, const rw_recipe_t *r, char *err);

#endif
