// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
// The link box (link_box.c): the console has no browser to hand a link to, so the address is shown to type on a phone or computer.
#pragma once
#include "linux_sdl_events.h"
#include <stdbool.h>

void linkBoxShow(const char *url);
bool linkBoxVisible(void);
void linkBoxFeed(const LinuxInputSnapshot *snapshot);  // Cross or Circle closes it
void linkBoxDraw(void);                                // between overlayBegin and overlayEnd
