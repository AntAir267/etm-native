/* port.h: Enter the Matrix as the AntiStatic runtime sees it (the toolkit's
   runtime/win32/port_example.h describes each value). */
#pragma once

#define PORT_TITLE "Enter the Matrix"
#define PORT_SLUG "etm-native"
#define PORT_ENV "ETM"

/* the game's folder as the program sees it, and its program */
#define PORT_VROOT "C:\\Matrix"
#define PORT_START_DIR ""
#define PORT_EXE "Matrix.exe"
#define PORT_CMDLINE_ARGS ""
#define PORT_VOLUME_LABEL "MATRIX"

/* program memory above Matrix.exe's image (0x400000..0x9e8000) and its tables; the DivX
   libraries sit at 0x10000000..0x100b0000, which the allocator steps around */
#define PORT_ARENA_START 0x01000000u

/* --test runs: a finished thread's place is taken again and the main thread's Sleep moves the
   virtual clock, as this port's own runtime did */
#define PORT_TEST_SCHEDULER 2

/* Miles 6.5: AIL_set_3D_sample_volume takes a float 0..1 */
#define PORT_MILES_VERSION 65

/* the game plays no Miles streams (its films have their own ADX sound): no FFmpeg */
#define PORT_MILES_NO_FFMPEG 1

/* the Direct3D 9 bridge: anti-aliasing 4x by default */
#define PORT_D3D9_ANTIALIASING "4x"

/* the game starts full screen, as this port always did, unless options.txt says window */
#define PORT_DEFAULT_DISPLAY "fullscreen"
