#!/usr/bin/env bash
# A spread of bends over the sample, for looking at: each one a different
# part of the engine. Output in ../out/host/demo.
set -euo pipefail
cd "$(dirname "$0")"
S=../samples/DSC01542.ARW
O=../out/host/demo
mkdir -p "$O"
./rewire "$S" -r 3 -d 4 -s "Quarters" -j "$O/quarters.JPG"
./rewire "$S" -x "Lock loss:8:6" -x "Bit slip:5:4" -j "$O/lockslip.JPG"
./rewire "$S" -x "DMA stride:12:3" -o "$O/stride.ARW" -j "$O/stride.JPG"
./rewire "$S" -x "JPEG bus:3:6" -j "$O/jpegbus.JPG"
./rewire "$S" -x "Pixel sort:9:9" -j "$O/sort.JPG"
./rewire "$S" -x "Aerochrome:10:8" -j "$O/aero.JPG"
./rewire "$S" -r 11 -B -d 3 -s "Truchet" -S 5 -x "Melt:12:10" -j "$O/truchet.JPG" -b "$O/truchet.BND"
# A recipe read back out of a tagged picture must reproduce it.
./rewire "$S" -p "$O/truchet.JPG" -S 5 -j "$O/truchet_again.JPG"
cmp "$O/truchet.JPG" "$O/truchet_again.JPG" && echo "recipe round trip: identical"
