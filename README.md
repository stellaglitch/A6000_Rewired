# Soft Bending For The α6000

[CHDK_Rewired](https://github.com/lr-m/CHDK_Rewired)'s circuit bending, made as an app for the Sony α6000.

On a PowerShot, CHDK_Rewired rewires the sensor's data bus inside the camera's capture path, before Canon develops the picture. The a6000's imaging pipeline cannot be patched that way. What a PlayMemories app can do is read the raw the camera has just written. So this app runs CHDK_Rewired's own bend engine (`bend.c` and `bendx.c`, unchanged) one step later, on the ARW's sensor data:

```
shutter ─► camera writes DCIM/100MSDCF/DSC01542.ARW (untouched)
              │
              ▼  cRAW decoded to 12-bit linear, into a PowerShot-layout raw buffer
          bend matrix, per segment  ─►  experimental chain (Melt, DMA stride, Lock loss, JPEG bus ...)
              │
              ▼
          REWIRED/RW01542.ARW   the bent raw, re-encoded in place: opens in any raw developer
          REWIRED/RW01542.JPG   developed in the camera, tagged with its recipe
          REWIRED/RW01542.BND   the recipe, the same record CHDK_Rewired writes
```

The values the engine sees are linear sensor values: what an ADC bus carries, and what CHDK bends. Presets move between the two cameras. `BENDnn.BND` files from a PowerShot's `CHDK/BENDS` load here, presets saved here go in `REWIRED/BENDS` in the same format.

## Using The App

Use **RAW+JPEG**. Never use RAW alone: on the a6000 a shutter fired from an app while Quality is RAW alone restarts the camera (Recipe Lab's issue #100, and confirmed here). The app guards against it: before every shot it switches its own shots to RAW+JPEG and refuses to fire unless the camera confirms the change. Your setting comes back when you leave the app. Then open **Rewired** from the Application list.

| key | |
|---|---|
| up / down | row: **matrix**, **scope**, **chain**, **multi**, **preset** |
| left / right | field in the row (on the matrix, the output bit) |
| wheel / dial | change the field (on the matrix, patch a source into the bit) |
| centre | matrix: reroll · scope: mutate · chain: lock this effect and start another · multi: cancel the sequence · preset: load or save |
| trash | matrix: straight wire · scope: reset the region's matrix · chain: drop an effect · multi: cancel the sequence |
| Fn | hide or show the panel |
| shutter | shoot, then bend. A bar shows progress, then the bent picture appears. Centre, MENU or a half-press returns |
| MENU | leave |

The live view is the camera's own and is not bent. You see the bend in the review after each shot. CHDK_Rewired's live preview depends on reading the PowerShot's viewport buffer, and this app has no equivalent yet.

### Multiple exposure

This is CHDK_Rewired's double exposure. Two to nine shots are blended into one picture, and the shots taken so far stay over the live view as a see-through **ghost**, so you can line up the next one. Turn it on in the **MULTI** row:

| field | |
|---|---|
| MULTI | on / off. Turning it off during a sequence cancels the sequence |
| BLEND | **ADD** sums the light, so highlights build up and clip. **LIGHTEN** keeps the brighter pixel (lights on black). **DARKEN** keeps the darker one. **AVERAGE** is the classic ghost |
| SHOTS | 2–9 |
| BEND | **each**: every shot is bent as it is taken, then the bent shots are blended. **composite**: the shots are blended first and the blend is bent once |
| GHOST | how strongly the shots so far show over the live view: OFF, 25%, 50%, 75% |
| SCREEN | **LCD** or **EVF**, whichever you are looking through. The LCD shows 640×480 pixels on a 16:9 panel, so its pixels are wider than they are tall. The finder is 4:3 with square pixels. This setting keeps the review, the ghost and the menu text at 3:2 on either |
| NOW | which shot is next |

The blend uses the raw sensor values, so ADD and AVERAGE behave as they would on film. The ghost is a quick preview built from JPEGs, so it is close to the result but not exact. With BEND *each*, every shot except the last is saved as its own bent picture, and the last shot gives the composite. With BEND *composite*, the shots in between are only the camera's own files in `DCIM`. A sequence uses the BLEND, SHOTS and BEND settings it started with. Changing them during a sequence takes effect from the next one.

## The desktop tool

`host/rewire` is the same C, built for a computer. It is the test bench, and it can also bend ARWs you already have:

```
rewire IN.ARW -o OUT.ARW -j OUT.JPG -r 7                    random matrix, seed 7
rewire IN.ARW -x "Melt:12:10" -x "Lock loss:8:6" -j OUT.JPG  an effect chain
rewire IN.ARW -p BEND07.BND -s Quarters -j OUT.JPG           a PowerShot preset, segmented
rewire IN.ARW -p RW01542.JPG -j AGAIN.JPG                    the recipe out of a tagged picture
rewire B.ARW -e A.ARW -M lighten -r 7 -j OUT.JPG             double exposure, bent after blending
rewire B.ARW -e RW_A.ARW -M add -A -r 7 -j OUT.JPG           B bent, then laid over an already bent A
rewire --list | --info IN.ARW | --selftest IN.ARW
```

Build it with MSYS2 (`C:\msys64`, `MSYSTEM=UCRT64`): `cd host && make test`. Put an a6000 ARW at `samples/DSC01542.ARW` first. `host/demo.sh` renders a spread of bends into `out/host/demo`. `tools/verify_arw.py` checks a bent ARW with LibRaw (`pip install rawpy pillow numpy`).

## Building the app

Needs JDK 17, NDK **r16b** (the last NDK whose GCC targets the camera's armeabi / Android 2.3), and Android SDK build-tools 30.0.3 with platform 28. In this workspace they live in `..\tools`.

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

This produces `Rewired.apk`, v1-signed with a throwaway key. Install it the same way Recipe Lab installs: [Sony-PMCA-RE](https://github.com/ma1co/Sony-PMCA-RE), connect your camera in **Mass Storage** USB mode, then `pmca-console install -f Rewired.apk` (or *Install app from file* in pmca-gui).

`tools\sync-engine.ps1` re-copies the engine from `..\CHDK_Rewired` after it changes.

## Layout

```
native/engine/   CHDK_Rewired's bend.c / bendx.c / bend.h / bendx.h, verbatim; camera.h is a stand-in
native/arw.c     ARW: TIFF walk, SR2Private decryption (WB, black, white), the cRAW block codec
native/pipeline.c  read -> bend -> bendx -> re-encode + develop + preview/thumbnail replacement
native/jpegenc.c streaming baseline JPEG encoder (8 rows in memory)
native/mexp.c    multiple exposure: the four blends, and the earlier exposures read row by row beside the current one
native/recipe.c  BSH1 records, BND1 presets, @rewired_optics tags - byte-compatible with CHDK_Rewired
jni/             JNI bridge, ndk-build files
src/             the app: MainActivity (camera, keys, shot flow), PatchView (screen), Card (files), Native
host/            rewire, the desktop build, and its tests
```

## Credits

The bend engine is CHDK_Rewired's, under its GPL; this project is GPL to match. The camera-side approach (CameraEx by reflection, key scan codes, 8.3 file names, the build steps) follows [Recipe Lab](https://github.com/voxivoid/recipe-lab-sony-pmca). The cRAW decoder follows dcraw's `sony_arw2_load_raw`, and the SR2 cipher follows dcraw's `sony_decrypt`. The ILCE-6000 colour matrix is Adobe's.
