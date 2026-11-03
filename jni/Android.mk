LOCAL_PATH := $(call my-dir)

# librewired.so: the CHDK_Rewired engine, the ARW codec, the pipeline, JNI.
# Nothing Sony-specific is linked; it needs only the camera's libc and libm.
include $(CLEAR_VARS)
LOCAL_MODULE    := rewired
LOCAL_SRC_FILES := rewired_jni.c \
                   ../native/arw.c ../native/recipe.c ../native/jpegenc.c ../native/pipeline.c ../native/log.c ../native/mexp.c \
                   ../native/engine/bend.c ../native/engine/bendx.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../native $(LOCAL_PATH)/../native/engine
LOCAL_CFLAGS    += -std=gnu99 -O2 -Wall -Wno-unused-parameter -fvisibility=default
LOCAL_LDLIBS    := -lm
include $(BUILD_SHARED_LIBRARY)
