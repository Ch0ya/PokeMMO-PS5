// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, include/linux_sdl_cursor.h)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO contributors.
// Changes: PS5: the controller's touchpad is also a trackpad (loader-27, linux_sdl_cursor.c).
#pragma once
#include "linux_sdl_events.h"

// A mouse cursor driven by the controller, for the client's own windows (loader-60). PS5: the touchpad moves it too, as a
// trackpad, without taking the controller. The R2 pointer is held: press R2, then move the left stick past its dead zone,
// and the pointer appears and follows that stick. Cross is the left click while it is up, and releasing R2 puts it away.
// A short R2 tap stays Return and a 600 ms hold stays Tab (padUiShortcuts), so one button has three uses by how it is held.
void linuxSdlCursorUpdate(LinuxInputSnapshot *snapshot, unsigned width, unsigned height);  // between the sampling and linuxSdlEventsUpdate
void linuxSdlCursorDraw(unsigned width, unsigned height);                                  // on the rendering thread, before the buffers are swapped
// loader-60: true while the R2 pointer is up or the touchpad is being used, and true once R2's hold has armed it (padUiShortcuts
// checks this so a hold that becomes a pointer sends no Tab). The battle probe also checks it: the player's pointer must win
// over the move-summary parking.
bool linuxSdlCursorActive(void);
bool linuxSdlCursorR2Armed(void);
