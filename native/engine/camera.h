#ifndef REWIRED_CAMERA_STUB_H
#define REWIRED_CAMERA_STUB_H
// Stand-in for CHDK's camera.h: bendx.c only asks whether the experimental
// engine is built in. On the a6000 it always is.
#define CAM_BEND_EXPERIMENTAL 1
#endif

