// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// The link box. The game opens some links in the browser (forgotten password, forgotten username, news): a PS5 title has no
// browser to give them to when the system's own browser cannot be started, so the address is shown here to type on a phone
// or computer. The controller closes it.
#include "link_box.h"
#include "diagnostics.h"
#include "overlay.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_bool visible;
static atomic_flag guard = ATOMIC_FLAG_INIT;
static char address[512];
static bool fresh;
static uint32_t before;

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) {}
}
static void unlock(void) { atomic_flag_clear_explicit(&guard, memory_order_release); }

void linkBoxShow(const char *url) {
    lock();
    snprintf(address, sizeof(address), "%s", url ? url : "");
    fresh = true;
    atomic_store(&visible, true);
    unlock();
    diagnosticsTrace("link.show url=%.200s", address);
}
bool linkBoxVisible(void) { return atomic_load(&visible); }
void linkBoxFeed(const LinuxInputSnapshot *s) {
    if (!atomic_load(&visible) || !s->gamepad) return;
    lock();
    uint32_t pressed = s->buttons & ~before;
    if (fresh) pressed = 0;  // the press that opened the link does not close it
    fresh = false;
    before = s->buttons;
    unlock();
    if (pressed & 3u) atomic_store(&visible, false);  // Cross or Circle
}

// The address in lines of at most `max_width`, broken anywhere (addresses have few spaces).
static void drawAddress(float centre_x, float y, float size, float max_width) {
    char line[160];
    const char *rest = address;
    for (int row = 0; row < 5 && *rest; ++row) {
        size_t take = 0, length = strlen(rest);
        while (take < length && take < sizeof(line) - 1) {
            memcpy(line, rest, take + 1);
            line[take + 1] = 0;
            if (overlayTextWidth(line, size) > max_width) break;
            ++take;
        }
        memcpy(line, rest, take);
        line[take] = 0;
        if (row == 4 && take < length) memcpy(line + (take > 3 ? take - 3 : 0), "...", 4);
        overlayTextCentered(centre_x, y + (float)row * size * 1.2f, line, size, 0xC8D6F0FFu);
        rest += take;
    }
}
void linkBoxDraw(void) {
    if (!atomic_load(&visible)) return;
    lock();
    const float panel_w = 1000, panel_h = 460, panel_x = (OVERLAY_WIDTH - panel_w) / 2, panel_y = (OVERLAY_HEIGHT - panel_h) / 2;
    const float centre = OVERLAY_WIDTH / 2;
    overlayRect(0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, 0x00000080u);
    overlayRect(panel_x, panel_y, panel_w, panel_h, 0x081228FFu);
    overlayFrame(panel_x, panel_y, panel_w, panel_h, 2, 0x5AA0E8FFu);
    overlayTextCentered(centre, panel_y + 34, "Open this link in a browser", 34, 0xFFFFFFFFu);
    overlayTextCentered(centre, panel_y + 88, "Type the address below on your phone or computer.", 26, 0x8A9BBFFFu);
    drawAddress(centre, panel_y + 160, 30, panel_w - 80);
    overlayTextCentered(centre, panel_y + panel_h - 52, "\x01 / \x02  Close", 26, 0xA8B8D8FFu);
    unlock();
}
