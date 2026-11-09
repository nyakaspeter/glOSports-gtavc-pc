#!/bin/sh
# Profile reVC on the console while you play.
#   1. Launch with the sampler on (kills a running re3):
#        tools/profile.sh start
#   2. Get to the scene you care about, then:
#        tools/profile.sh sample [seconds]     (default 30)
# Prints the share per library and the hottest functions in re3, libMali and
# libc. Needs the console on USB serial, Docker, and a re3 build in out/.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../goodluckOS-tools" && pwd)        # goodluckOS-tools, the folder with console.py
WORK=${TMPDIR:-/tmp}/re3prof
console() { (cd "$ROOT" && GLOS_TIMEOUT=${T:-60} python3 -I console.py "$1"); }

case "${1:-}" in
start)
    console '/usr/local/bin/appctl kill-application; sleep 3; /usr/local/bin/appctl launch-application "env REVC_PROFILE=1 REVC_FPS_LOG=1 REVC_DRAW_STATS=1 /home/player/ports/gtavc-pc/start.sh"; sleep 5; pidof reVC'
    ;;
sample)
    secs=${2:-30}
    T=$((secs + 30)) console "p=\$(pidof reVC); kill -USR2 \$p; sleep $secs; kill -USR2 \$p; sleep 3; grep 'glos: FPS' /dev/shm/reVC.log | tail -$((secs * 2)) | awk '{s+=\$3; d+=\$5; n++} END {if(n) printf \"avg FPS %.1f, avg draws %.0f\\n\", s/n, d/n}'"
    rm -rf "$WORK" && mkdir -p "$WORK"
    console 'cat /dev/shm/re3prof.txt' > "$WORK/re3prof.txt"
    python3 -I "$HERE/split.py" "$WORK/re3prof.txt" "$WORK/libs"
    rm -rf "$WORK/x" && mkdir -p "$WORK/x"
    unzip -q "$HERE/../out/re3-port.zip" ports/gtavc-pc/reVC -d "$WORK/x"
    cp "$WORK/x/ports/gtavc-pc/reVC" "$HERE/libs/libc.so.6" "$WORK/libs/"
    cp "$ROOT/../goodluckOS-mali-driver/stubs/libMali.so.orig" "$WORK/libs/libMali.so"
    cp "$HERE/sym.py" "$WORK/"
    docker run --rm -v "$WORK":/p -w /p/libs debian:bookworm sh -c '
        apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends binutils-arm-linux-gnueabihf python3 >/dev/null 2>&1
        for l in reVC libMali.so libc.so.6; do [ -s $l.txt ] && { echo; python3 ../sym.py $l $l.txt 20; }; done'
    ;;
*)
    sed -n 2,8p "$0"
    exit 2
    ;;
esac
