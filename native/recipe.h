#ifndef REWIRED_RECIPE_H
#define REWIRED_RECIPE_H

//-------------------------------------------------------------------
// Everything a shot is bent with, in CHDK_Rewired's own structures and its
// own file formats - so a bend dialled in on a PowerShot loads on the a6000,
// and a picture taken here carries a record a PowerShot can read back.
//
//   BSH1 record   the shot record (CHDK core/bend_shot.c): what goes in the
//                 JPEG comment and in the .BND sidecar beside a picture
//   BND1 file     a saved preset (CHDK core/bend_store.c), BENDnn.BND
//
// Both are the structs written byte for byte. Every field of bend_t,
// bendx_chain_t and bend_segs_t is naturally aligned, so the layout is the
// same on the PowerShots' ARM946, the a6000's ARM and a desktop.
//-------------------------------------------------------------------

#include "engine/bend.h"
#include "engine/bendx.h"

#define RW_NBITS        12      // linear sensor values, as the engine sees them

typedef struct
{
    bend_t        bend;         // segment 0's matrix
    bendx_chain_t bendx;
    bend_segs_t   segs;         // the layout, and segments 1..3's matrices
    int           bend_on;
    int           bendx_on;
} rw_recipe_t;

#define RW_NAME_MAX     24
#define RW_DESC_MAX     80

#define RW_REC_MAX      (16 + sizeof(bend_t) + sizeof(bendx_chain_t) + sizeof(bend_segs_t))

void rw_recipe_reset(rw_recipe_t *r);
void rw_recipe_sanitize(rw_recipe_t *r);

// The matrix for one segment (0 = the main one), clamped to the layout.
bend_t *rw_recipe_seg(rw_recipe_t *r, int seg);

// Whether applying it would change anything.
int rw_recipe_active(const rw_recipe_t *r);

// BSH1, exactly as bend_shot_pack()/bend_shot_unpack() write and read it.
int rw_recipe_pack(const rw_recipe_t *r, unsigned char *out, int outlen);
int rw_recipe_unpack(const unsigned char *rec, int len, rw_recipe_t *r);

// BND1 preset files. name/desc may be null. Return 1 on success.
int rw_preset_load(const char *path, rw_recipe_t *r, char *name, char *desc);
int rw_preset_save(const char *path, const rw_recipe_t *r, const char *name, const char *desc);

// The human half of the comment CHDK_Rewired puts in a picture: the matrix,
// the chain and the layout as text, then the BSH1 record in hex. Returns the
// length written into out (always NUL-terminated).
int rw_recipe_tag(const rw_recipe_t *r, char *out, int outlen);

// Read a record back out of any file that carries a tag - a JPEG this app or
// CHDK_Rewired wrote, or a .BND sidecar of either kind.
int rw_recipe_from_file(const char *path, rw_recipe_t *r);

#endif
