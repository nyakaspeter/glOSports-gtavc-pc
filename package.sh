#!/bin/sh
# Unpack the Docker build (out/re3-port.zip) into out/ports/revc/ and add glos/,
# the driver set from goodluckOS-mali-driver, so the folder is complete for the
# console: copy out/ports/revc/ over /home/player/ports/revc/ (your Vice City game
# files stay there) and out/.local/share/applications/revc.puppy next to the
# other launcher entries.
set -eu
cd "$(dirname "$0")"
GLOS=../goodluckOS-mali-driver/glos
[ -d "$GLOS" ] || { echo "package.sh: $GLOS missing (goodluckOS-mali-driver/assemble-glos.sh)" >&2; exit 1; }
rm -rf out/ports out/.local && unzip -q -o out/re3-port.zip -d out
rm -rf out/ports/revc/glos && mkdir -p out/ports/revc/glos/lib
cp "$GLOS"/glos-gpu "$GLOS"/glos-gpu-switch "$GLOS"/mali.ko "$GLOS"/glos_fbfix.ko out/ports/revc/glos/ && cp "$GLOS"/lib/* out/ports/revc/glos/lib/
ls out/ports/revc out/ports/revc/glos
