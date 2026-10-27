Third-party builds used here (not committed), copied from larush-native's (2026-10-26):
- dxvk-native/ : DXVK 3.1.1 (zlib licence), github.com/doitsujin/dxvk, built natively with SDL3
  (meson: native_sdl3=enabled, only d3d9), with dxvk-soft-shadows.patch (committed; larush-native's
  d3d9.shadowFilter setting, off unless set).
- sdl3/ : SDL3 3.4.18 (zlib licence), built as a shared library.
- dr_libs/dr_flac.h : dr_flac 0.12.42 by David Reid (public domain or MIT-0), committed.
