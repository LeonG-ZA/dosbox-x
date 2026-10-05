#!/bin/sh
# Recreate data/work/c from the read-only original, with the emulation-only config changes:
#   CONFIG.VPC: [NET] section removed (no network card emulated; the libraries support "No [NET] section").
set -e
cd "$(dirname "$0")/.."
rm -rf data/work/c
mkdir -p data/work
cp -r data/original/hdd data/work/c
chmod -R u+w data/work/c
python3 - <<'P'
import re
p='data/work/c/CONFIG.VPC'
s=open(p,'rb').read().decode('latin1')
s=re.sub(r'\[NET\].*?(?=\r?\n\[|\Z)', '', s, flags=re.S)
open(p,'wb').write(s.encode('latin1'))
P
echo "work copy ready"
