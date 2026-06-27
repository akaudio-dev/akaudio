# AK Audio

A personal VCV Rack plugin (collection of modules) by Andrey Kozlov.

Modules:

- **Ninjam** — NINJAM online jamming client; streams collaborative audio from a NINJAM server into Rack. (Port of the ideas in the `jamauv3` AUv3 project to a Rack module.)
- **Radio** — streaming internet radio (Icecast/HTTP) source feeding audio into a patch.

Both modules share the networked-audio layer in `src/net/` (HTTP/Icecast streaming, codec decode, lock-free ring buffer feeding the audio thread).

## Building

This plugin builds against a sibling source build of Rack at `../Rack`.

```bash
export RACK_DIR=/Users/akozlov/work/VCV/Rack   # or rely on the Makefile default ../Rack
make            # -> plugin.dylib
make install    # package + install into the Rack user plugins folder
make clean
```

## Adding a module

1. Add `src/<Name>.cpp` (Module + ModuleWidget + `Model* model<Name> = createModel<...>("<Name>")`).
2. Declare `extern Model* model<Name>;` in `src/plugin.hpp`.
3. Register it with `p->addModel(model<Name>);` in `src/plugin.cpp`.
4. Add a `res/<Name>.svg` panel and a module entry in `plugin.json`.

## License

Copyright © 2026 Andrey Kozlov.

AK Audio is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE) for the full text.

Bundled third-party code retains its own (GPL-compatible) license:
libogg / libvorbis (BSD), stb_vorbis and dr_mp3 (public domain).
