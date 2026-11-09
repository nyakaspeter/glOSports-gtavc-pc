# Third-party components

The port builds on and ships with:

| Component | Licence | Text |
|---|---|---|
| [re3](https://github.com/Cai1Hsu/re3) (fork `Cai1Hsu/re3`, `miami` at `e19f406`), the engine, with the data files it ships (`TEXT/`, `neo/`, the files in `data/` and `models/` the port puts there) | none published: the re3 project releases its source without a licence | – |
| [librw](https://github.com/aap/librw), built into the binary | MIT | `librw.LICENSE.txt` |
| OpenAL Soft 1.19.1 (`lib/libopenal.so.1`, Debian bookworm `libopenal1` 1:1.19.1-2) | LGPL-2.0 or later | `openal-soft.copyright.txt` |
| mpg123 (`lib/libmpg123.so.0`, Debian bookworm `libmpg123-0`) | LGPL-2.1 | `mpg123.copyright.txt`, `LGPL-2.1.txt` |
| sndio, libbsd, libmd (`lib/`, Debian bookworm, OpenAL Soft's dependencies) | ISC, BSD-3-Clause | `sndio.copyright.txt`, `libbsd.copyright.txt`, `libmd.copyright.txt` |
| [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) (`gamecontrollerdb.txt`) | zlib | `SDL_GameControllerDB.LICENSE.txt` |
| ARM's Mali user-space driver (`glos/lib/libMali.so`) | ARM proprietary | `libMali.txt` |
| ARM's Mali kernel driver (`glos/mali.ko`) | GPL-2.0 | goodluckOS-mali-driver |

The port's own code is MIT (`LICENSE.txt`, the repo's `LICENSE`).

The game itself is not included: its files come from your own copy.
