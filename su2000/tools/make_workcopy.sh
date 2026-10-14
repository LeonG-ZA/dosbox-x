#!/bin/sh
# Recreate data/work/c from the read-only original. CONFIG.VPC is kept as it is, [NET] section included: the SMC 8013
# is emulated, and Boxing's two-player (link up) game needs it even on one cabinet (both players wait for each other
# without it).
set -e
cd "$(dirname "$0")/.."
rm -rf data/work/c
mkdir -p data/work
cp -r data/original/hdd data/work/c
chmod -R u+w data/work/c
echo "work copy ready"
