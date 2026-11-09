# syntax=docker/dockerfile:1
# Cross-builds re3 (GTA III) for goodluckOS on the Allwinner A33 (armhf).
# Output: out/re3-port.zip, to unzip into the HOME partition.
# GAME selects the game (iii: GTA III from the main branch, vc: Vice City from
# the miami branch with main's librw); this repo builds one of them. It is
# the fork's miami branch plus the same librw, so every patch here applies.

FROM debian:bookworm AS builder

RUN dpkg --add-architecture armhf && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates git cmake make pkg-config zip file \
        crossbuild-essential-armhf \
        libsdl2-dev:armhf \
        libopenal-dev:armhf \
        libmpg123-dev:armhf && \
    rm -rf /var/lib/apt/lists/*

ENV PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabihf/pkgconfig
ENV PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig
ENV PKG_CONFIG_ALLOW_CROSS=1

ARG RE3_REPO=https://github.com/Cai1Hsu/re3
ARG RE3_COMMIT=19d2dea
ARG REVC_COMMIT=e19f406
ARG GAME=vc

WORKDIR /build
RUN git clone --recursive "$RE3_REPO" re3 && cd re3 && \
    if [ "$GAME" = vc ]; then \
      git checkout "$REVC_COMMIT" && git submodule update --init --recursive && \
      git archive "$RE3_COMMIT" vendor/librw | tar -x; \
    else \
      git checkout "$RE3_COMMIT" && git submodule update --init --recursive; \
    fi

COPY toolchain-armhf.cmake patch-re3.sh /build/
COPY shim /build/shim
WORKDIR /build/re3
ARG MAX_LIGHTS=4
RUN MAX_LIGHTS=$MAX_LIGHTS sh /build/patch-re3.sh /build/shim
# Vice City reads REVC_* instead of RE3_* (RE3_FPS_COUNTER -> REVC_FPS_COUNTER, ...).
RUN if [ "$GAME" = vc ]; then \
      files=$(grep -rlE '"RE3_[A-Z_]+"' src vendor/librw/src); \
      echo "REVC_ env names in:" $files; \
      sed -i -E 's/"RE3_([A-Z_]+)"/"REVC_\1"/g' $files; \
      ! grep -rqE '"RE3_[A-Z_]+"' src vendor/librw/src; \
    fi

# Optimisation switches:
#   (default: LTO=1 PGO=use, i.e. link-time + profile-guided optimisation
#    using the training profile committed in pgo/)
#   --build-arg LTO=0          no link-time optimisation
#   --build-arg PGO=generate   instrumented build; play, then collect the
#                              profile from /dev/shm/re3pgo (tools/pgo.sh)
#   --build-arg PGO=off        ignore the profile
ARG LTO=1
ARG PGO=use
#   --build-arg EXTRA_CFLAGS=...  e.g. -fno-strict-aliasing
ARG EXTRA_CFLAGS=-fno-strict-aliasing
COPY pgo /build/pgo
RUN set -e; flags="$EXTRA_CFLAGS"; \
    profile=/build/pgo; \
    [ "$LTO" = 1 ] && flags="$flags -flto=auto -fuse-linker-plugin"; \
    case "$PGO" in \
      generate) flags="$flags -fprofile-generate=/dev/shm/re3pgo -fprofile-update=atomic" ;; \
      use) flags="$flags -fprofile-use=$profile -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch" ;; \
    esac; \
    echo "optimisation flags:$flags"; \
    cmake -B build -S . \
        -DCMAKE_TOOLCHAIN_FILE=/build/toolchain-armhf.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
        -DCMAKE_EXE_LINKER_FLAGS="$flags -Wl,--export-dynamic-symbol=ioctl" \
        -DCMAKE_AR=/usr/bin/arm-linux-gnueabihf-gcc-ar \
        -DCMAKE_RANLIB=/usr/bin/arm-linux-gnueabihf-gcc-ranlib \
        -DLIBRW_PLATFORM=GL3 \
        -DLIBRW_GL3_GFXLIB=SDL2 \
        -DLIBRW_TOOLS=OFF \
        -DRE3_AUDIO=OAL -DREVC_AUDIO=OAL && \
    cmake --build build -j"$(nproc)"

# Package: binary, the port's own data overrides, launcher and the audio libs
# goodluckOS lacks (in lib/, which the binary finds through an $ORIGIN rpath).
COPY start.sh alsoft.conf gtavc-pc.puppy icon.png /build/pkg/
RUN set -e; \
    if [ "$GAME" = vc ]; then dir=gtavc-pc bin=reVC name="Vice City"; else dir=gta3-pc bin=re3 name="GTA III"; fi; \
    out=/output/ports/$dir; mkdir -p $out/lib /output/.local/share/applications; \
    cp build/src/$bin $out/; \
    apt-get update -qq && apt-get install -y -qq --no-install-recommends patchelf >/dev/null; \
    patchelf --force-rpath --set-rpath '$ORIGIN/lib' $out/$bin; \
    cp -r gamefiles/* $out/; \
    for l in libopenal.so.1 libmpg123.so.0 libsndio.so.7.0 libbsd.so.0 libmd.so.0; do cp -L /usr/lib/arm-linux-gnueabihf/$l $out/lib/; done; \
    cp /build/pkg/start.sh $out/; cp /build/pkg/alsoft.conf $out/; \
    cp /build/pkg/$dir.puppy /output/.local/share/applications/; \
    cp /build/pkg/icon.png $out/; \
    chmod +x $out/start.sh $out/$bin; \
    echo "Copy your $name PC game files into this folder (anim, audio, data, models, txd, ...). Don't overwrite files the port already put here." > $out/place-game-files-here.txt; \
    file $out/$bin; \
    arm-linux-gnueabihf-readelf -d $out/$bin $out/lib/* | grep NEEDED | sort -u

WORKDIR /output
RUN zip -qr /re3-port.zip .

FROM scratch AS export
COPY --from=builder /re3-port.zip /
