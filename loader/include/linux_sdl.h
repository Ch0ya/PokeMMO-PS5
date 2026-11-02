// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_sdl.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO contributors.
// Changes: linuxSdlSetDisplayAcquire (the loading screen hands over the display); link box and on-screen keyboard.
// loader-41: the pad's own overlay (the hotbar selection marker and the battle scene's hints) and its screen probe,
// overlay.c's overlayReadBack. loader-42: the marker only shows while the client's own hotbar cells are on screen.
// loader-43: the client's chat box is pinned short at start, which is what places its battle window (main_ps5.c).
// loader-45: the interface can be drawn larger than the client's own 200% cap (linuxSdlSetRenderScale): the client is
// given a smaller window (clientScreenSize) and the loader scales that picture over the whole surface on present.
// loader-47: the render scale is a continuous factor from 1.0 to 1.5 (100%..150%), not a whole number, so the interface
// can be nudged larger in 5% steps that stay smooth.
#pragma once
#include "linux_dl.h"
#include <stdbool.h>
#include <stdio.h>

// This project's own implementations of the shared libraries the client's Java layer loads by name (LWJGL binds them with
// dlsym). They are "virtual": no ELF file exists for them, the functions below are the library.
//   libSDL3   window, OpenGL context (EGL on the Switch's window), events, time, gamepads (subset)
//   libGLX    the same entry points under the name LWJGL tries first
//   libEGL    eglGetProcAddress and the GL entry points of Mesa's EGL
//   libopenal OpenAL 1.1 on a software mixer (linux_audio.c) feeding the console's audio output
extern const LinuxVirtualLibrary linuxSdlLibrary, linuxEglLibrary, linuxGlxLibrary, linuxOpenAlLibrary;

void linuxSdlStopRequest(void);  // the next SDL_PollEvent reports SDL_EVENT_QUIT (applet exit)
// The picture should get this size (the console was docked or undocked). Any thread may ask; the thread that renders does it between two frames.
void linuxSdlRequestSize(unsigned width, unsigned height);
// PS5: draws the loading screen over the game's frames until they show something (between overlayBegin and overlayEnd).
void linuxSdlSetLoadingOverlay(void (*draw)(void));
// PS5: called once, when the game's frames first show something (or after the loading screen's time limit).
void linuxSdlSetFirstPicture(void (*callback)(void));
// Called on the game's thread before its window surface is made (PS5: the loading screen owns the only surface until then).
void linuxSdlSetDisplayAcquire(void (*acquire)(void));
bool linuxSdlReset(void);  // tears down the EGL surface and context created for the window
// For the file chooser, when the game calls it on the thread that renders: whether this thread owns the GL context, and one frame of the
// chooser (input sampled, picture swapped) so that it can run its own loop.
bool linuxSdlThreadHasContext(void);
bool linuxSdlPresentFrame(void);
// The client's interface can be drawn larger than its own 200% cap: with a render scale of 1.25 the client is told its
// window is the surface divided by 1.25 and the loader scales that picture over the whole surface on present. 1.0 leaves
// it off (the client draws into the whole surface). Clamped to 1.0..1.5. Set before the client starts; the value also
// drives the loader's own marker and probes.
void linuxSdlSetRenderScale(float scale);
float linuxSdlRenderScale(void);
// loader-65: a diagnostic switch (a file in the title folder): above 100% use the old window-copy path instead of the
// loader-owned texture, so the two can be compared at the same size.
void linuxSdlSetUpscaleCopy(bool on);
// loader-46: every whole line the client writes to stdout/stderr (game.c's sink) is passed here first.  The loader reads
// the client's own UI-viewport line from it, so the hotbar marker and the battle hints follow the client if its UI Scaling
// slider is moved while it runs.
void linuxSdlClientLine(const char *line);

typedef struct {
    unsigned windows, contexts, frames, calls, unknown_lookups;
    bool gl_ready;
    unsigned width, height;
} LinuxSdlStats;
LinuxSdlStats linuxSdlStats(void);

// The client's hotbar, in the client's own design units (a design viewport 1080 units high): the theme's nine 39-unit
// slots with 5-unit gaps, 32 units of pager border on the left and a 6.5-unit border round the rest. The client scales
// this layout by client.gui.scale.ui_zoom2 itself. main_ps5.c anchors the bar bottom centre (client 34702 replaced the old
// x/.y), and linux_sdl.c draws the marker on the selected slot (frame left + 32, frame top + 6.5).
// Measured from a 3840x2160 capture at ui_zoom2=2 with the bar bottom centre (design units = capture pixels / 2):
// frame 430x53 at x 745..1175, slots 39x39 at y 14.5..53.5, first slot x 777, pitch 44.
#define LINUX_SDL_HOTBAR_FRAME_W 430.0f   // the frame's width: 32 + 8 x 44 + 39 + 6.5
#define LINUX_SDL_HOTBAR_FRAME_H 53.0f    // the frame's height, flush on the design's bottom
#define LINUX_SDL_HOTBAR_INSET_X 32.0f    // the first slot's left edge from the frame's left (the pager border)
#define LINUX_SDL_HOTBAR_INSET_Y 6.5f     // the slots' top edge from the frame's top
#define LINUX_SDL_HOTBAR_CELL 39.0f       // one slot (theme item-slot)
#define LINUX_SDL_HOTBAR_PITCH 44.0f      // one slot plus the 5-unit gap between slots
