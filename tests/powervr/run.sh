#!/bin/sh
# Host-side test of the PowerVR PCX1/PCX2 emulation. Writes pvr_pcx1.ppm / pvr_pcx2.ppm
# to the directory given as $1 (default: current directory).
set -e
here=$(dirname "$0")
out=${1:-.}
${CXX:-g++} -std=gnu++14 -O2 -Wall -Wextra -I"$here/shim" "$here/pvr_test.cpp" -o "$out/pvr_test" -lm
"$out/pvr_test" "$out"
