Third-party libraries. Only dr_libs/ is committed here; the AntiStatic toolkit is the submodule
antistatic/; build SDL3 and DXVK here.

- antistatic/ : the AntiStatic recompilation toolkit (the Unlicense), a submodule pinned at a
  release tag.
- sdl3/ : SDL3 3.4.18 (zlib licence), github.com/libsdl-org/SDL, built as a shared library and
  installed into sdl3/ (sdl3/include, sdl3/lib).
- dxvk-native/ : DXVK 3.1.1 (zlib licence), github.com/doitsujin/dxvk (git clone --recursive
  --branch v3.1.1), built and installed with the toolkit's script and this port's patch set:
    antistatic/tools/d3d9/build_dxvk.sh --src <dxvk> --sdl3 third_party/sdl3 \
        --prefix third_party/dxvk-native --patches soft-shadows
- dr_libs/dr_flac.h : dr_flac 0.12.42 by David Reid (public domain or MIT-0), committed.

`make` copies the two libraries into build/lib beside the program.
