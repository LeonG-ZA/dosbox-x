#!/bin/sh
# Run one SU2000 program under the patched DOSBox-X for a fixed time and keep its trace.
# Usage: tools/run_game.sh NAME DIR "COMMAND" [SECONDS]
#   e.g. tools/run_game.sh sfl SFL "sfl" 60
# Output: logs/NAME.su2k (bus trace), logs/NAME.dosbox.log (emulator log), logs/NAME.screen.txt (text screen)
set -e
cd "$(dirname "$0")/.."
name=$1; dir=$2; cmd=$3; secs=${4:-60}
DOSBOX=${DOSBOX:-../src/dosbox-x}
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=dummy timeout -s KILL "$secs" \
  "$DOSBOX" -conf dosbox-su2000.conf -nopromptfolder -set "su2000 logfile=logs/$name.su2k" -set "su2000 screen dump=logs/$name.screen.txt" \
  -c "cd \\$dir" -c "$cmd " < /dev/null 2>&1 | head -c 2000000 > "logs/$name.dosbox.log" || true
ls -la "logs/$name.su2k"
