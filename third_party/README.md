Third-party libraries. Only dr_libs/ and the DXVK patch are committed; build the other two here.

- sdl3/ : SDL3 3.4.18 (zlib licence), github.com/libsdl-org/SDL, built as a shared library and
  installed into sdl3/ (sdl3/include, sdl3/lib).
- dxvk-native/ : DXVK 3.1.1 (zlib licence), github.com/doitsujin/dxvk, built natively against that
  SDL3 (meson: native_sdl3=enabled, only Direct3D 9), with dxvk-soft-shadows.patch applied first
  (a d3d9.shadowFilter setting, off unless set), and installed into dxvk-native/
  (dxvk-native/include/dxvk, dxvk-native/lib).
- dr_libs/dr_flac.h : dr_flac 0.12.42 by David Reid (public domain or MIT-0), committed.

`make` copies the two libraries into build/lib beside the program.
