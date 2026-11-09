#!/bin/sh
# Profile-guided optimisation for reVC (Vice City).
#   1. docker buildx build --build-arg PGO=generate --output type=local,dest=./out-pgogen .
#   2. tools/pgo.sh install     put the instrumented build on the console
#   3. Play normally for a few minutes, then quit via the game's menu
#      (profile data is only written on a clean exit).
#   4. tools/pgo.sh collect     fetch the profile into pgo/
#   5. docker buildx build --output ...   (PGO=use LTO=1 are the defaults)
# Re-train after big code changes; a stale profile still builds, just helps less.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../goodluckOS-tools" && pwd)
dir=gtavc-pc bin=reVC out=out-pgogen prof=pgo
console() { (cd "$ROOT" && GLOS_TIMEOUT=${T:-60} python3 -I console.py "$1"); }

case "${1:-}" in
install)
    unzip -o -q "$HERE/$out/re3-port.zip" ports/$dir/$bin -d "${TMPDIR:-/tmp}/re3pgo-bin"
    cp "${TMPDIR:-/tmp}/re3pgo-bin/ports/$dir/$bin" "${TMPDIR:-/tmp}/$bin.new"
    (cd "$ROOT" && python3 -I mtp_send.py / "${TMPDIR:-/tmp}/$bin.new")
    console "/usr/local/bin/appctl kill-application; sleep 3; rm -rf /dev/shm/re3pgo; mv /home/player/$bin.new /home/player/ports/$dir/$bin; chmod +x /home/player/ports/$dir/$bin; /usr/local/bin/appctl launch-application /home/player/ports/$dir/start.sh; sleep 3; pidof $bin"
    ;;
collect)
    console "pidof $bin && echo '$bin is still running - quit it from the game menu first' && exit 1; ls /dev/shm/re3pgo | wc -l; du -sh /dev/shm/re3pgo"
    rm -rf "${HERE:?}/$prof" && mkdir -p "$HERE/$prof" && touch "$HERE/$prof/.keep"
    T=300 console 'cd /dev/shm/re3pgo && tar czf - . | base64 -w0; echo' | tr -d '\r\n' | base64 -d | tar xzf - -C "$HERE/$prof"
    echo "$(ls "$HERE/$prof" | grep -c gcda) profile files in $prof/"
    ;;
*)
    sed -n 2,9p "$0"
    exit 2
    ;;
esac
