#!/bin/sh
# Unpack the Docker build (out/re3-port.zip) into out/ports/gtavc-pc/ and add glos/,
# the driver set from goodluckOS-mali-driver, so the folder is complete for the
# console: copy out/ports/gtavc-pc/ over /home/player/ports/gtavc-pc/ (your Vice City game
# files stay there) and out/.local/share/applications/gtavc-pc.puppy next to the
# other launcher entries.
set -eu
cd "$(dirname "$0")"
GLOS=../goodluckOS-mali-driver/glos
[ -d "$GLOS" ] || { echo "package.sh: $GLOS missing (goodluckOS-mali-driver/assemble-glos.sh)" >&2; exit 1; }
rm -rf out/ports out/.local && unzip -q -o out/re3-port.zip -d out
rm -rf out/ports/gtavc-pc/glos && mkdir -p out/ports/gtavc-pc/glos/lib
cp "$GLOS"/glos-gpu "$GLOS"/glos-gpu-switch "$GLOS"/mali.ko "$GLOS"/glos_fbfix.ko out/ports/gtavc-pc/glos/ && cp "$GLOS"/lib/* out/ports/gtavc-pc/glos/lib/
rm -rf out/ports/gtavc-pc/licenses && cp -R licenses out/ports/gtavc-pc/licenses
ls out/ports/gtavc-pc out/ports/gtavc-pc/glos
