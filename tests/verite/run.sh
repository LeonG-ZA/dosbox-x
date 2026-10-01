#!/bin/sh
# Host-side test of the Rendition Verite core. Writes verite_2d.ppm to the directory
# given as $2 (default: current directory). $1 is the V1000 2D microcode from the X.org
# rendition driver (xf86-video-rendition/src/v10002d.uc).
set -e
here=$(dirname "$0")
out=${2:-.}
${CXX:-g++} -std=gnu++14 -O2 -Wall -Wextra "$here/verite_test.cpp" "$here/../../src/hardware/verite_core.cpp" -o "$out/verite_test" -lm
"$out/verite_test" "${1:-v10002d.uc}" "$out"
