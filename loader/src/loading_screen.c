// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// The loading screen (see loading_screen.h). Virtual 1920x1080 coordinates (overlay.h), the logo's colours.
#include "loading_screen.h"
#include "overlay.h"
#include "overlay_assets.h"
#include <stdio.h>
#include <string.h>

// loader-60: the Gengar palette (deep purple and black, lavender text) replaces the old navy/blue one. The wordmark and the
// body text stay white/near-white, so the logo supplies the colour and the screen stays readable.
static const uint32_t BACKGROUND = 0x07030EFFu, TEXT = 0xFFFFFFFFu, SOFT = 0xDCD0F0FFu, DIM = 0xA695C4FFu, FAINT = 0x6F5C88FFu,
                      PURPLE = 0xA575E0FFu, TRACK = 0x2A1B3EFFu, RED = 0xE05555FFu, AMBER = 0xF0C060FFu, PANEL = 0x150B22FFu;

// One vertical rhythm for the ready screen: the gaps between the logo, the PokeMMO wordmark, the progress bar and the
// menu are all the same (90 px here), and the whole group is centred in the space above the footer.
enum { LOGO_Y = 185, LOGO_SIZE = 260, WORDMARK_Y = 535, WORDMARK_SIZE = 60, BAR_Y = 685, MENU_Y = 787 };

// Up to three lines, broken between words, centred.
static void drawWrapped(float y, const char *text, float size, float max_width, uint32_t rgba) {
    char line[256];
    const char *rest = text;
    for (int row = 0; row < 3 && *rest; ++row) {
        size_t fit = 0, last_space = 0, length = strlen(rest);
        while (fit < length && fit < sizeof(line) - 1) {
            memcpy(line, rest, fit + 1);
            line[fit + 1] = 0;
            if (overlayTextWidth(line, size) > max_width) break;
            if (rest[fit] == ' ') last_space = fit;
            ++fit;
        }
        size_t take = fit == length || !last_space ? fit : last_space;
        memcpy(line, rest, take);
        line[take] = 0;
        overlayTextCentered(OVERLAY_WIDTH / 2, y + (float)row * size * 1.25f, line, size, rgba);
        rest += take;
        while (*rest == ' ') ++rest;
    }
}
// The on-screen keyboard button players would not guess, at the bottom right.  Our loader revision sits in the centre,
// like the hints; a screen's own hint goes just above.  (The old bottom-left "Start Menu" note is gone: Options opens it.)
static void drawFooterHints(const LoadingView *view, const char *middle) {
    const char *right = "R3  On-screen keyboard";
    overlayText(OVERLAY_WIDTH - 48 - overlayTextWidth(right, 24), 1032, right, 24, FAINT);
    if (view->loader_revision && view->loader_revision[0]) {
        char line[64];
        snprintf(line, sizeof(line), "PokeMMO Rev %s", view->loader_revision);
        overlayTextCentered(OVERLAY_WIDTH / 2, 1032, line, 24, FAINT);
    }
    if (middle && middle[0]) overlayTextCentered(OVERLAY_WIDTH / 2, 996, middle, 24, FAINT);
}

// ---- the ready menu --------------------------------------------------------------------------------------------------------------------
static const char *const menu_labels[] = {"Start Game", "Settings", "Exit"};
static void drawMenu(int selected) {
    const float width = 340, height = 84, gap = 40;
    const float total = 3 * width + 2 * gap, left = (OVERLAY_WIDTH - total) / 2, top = MENU_Y;
    for (int i = 0; i < 3; ++i) {
        float x = left + (float)i * (width + gap);
        bool chosen = i == selected;
        overlayRect(x, top, width, height, chosen ? 0x2C1746FFu : PANEL);
        overlayFrame(x, top, width, height, chosen ? 3 : 1, chosen ? PURPLE : 0x3A2752FFu);
        overlayTextCentered(x + width / 2, top + 24, menu_labels[i], 34, chosen ? TEXT : SOFT);
    }
}

// ---- Settings: start-up details and logs ----------------------------------------------------------------------------------------------
static const char *const settings_labels[] = {"Startup details", "Log", "Check for updates", "Interface size"};
static void drawSettingsList(int item) {
    const float x = 120, y = 190, width = 370, height = 66, gap = 14;
    for (int i = 0; i < SETTINGS_ITEMS; ++i) {
        float ty = y + (float)i * (height + gap);
        bool chosen = i == item;
        overlayRect(x, ty, width, height, chosen ? 0x2C1746FFu : PANEL);
        overlayFrame(x, ty, width, height, chosen ? 3 : 1, chosen ? PURPLE : 0x3A2752FFu);
        overlayText(x + 24, ty + (height - 26) / 2, settings_labels[i], 26, chosen ? TEXT : DIM);
    }
}
static void drawDetailsTab(const LoadingView *view) {
    const float box_x = 540, box_y = 190, box_w = OVERLAY_WIDTH - box_x - 120, box_h = 430;
    overlayRect(box_x, box_y, box_w, box_h, PANEL);
    float y = box_y + 26;
    char line[300];
    snprintf(line, sizeof(line), "PokeMMO %s", view->version && view->version[0] ? view->version : "?");
    overlayTextFit(box_x + 28, y, line, 30, box_w - 56, TEXT);
    y += 48;
    snprintf(line, sizeof(line), "Client revision %s", view->revision && view->revision[0] ? view->revision : "none");
    overlayTextFit(box_x + 28, y, line, 26, box_w - 56, SOFT);
    y += 42;
    snprintf(line, sizeof(line), "Loader Rev %s", view->loader_revision && view->loader_revision[0] ? view->loader_revision : "?");
    overlayTextFit(box_x + 28, y, line, 26, box_w - 56, SOFT);
    y += 42;
    snprintf(line, sizeof(line), "Build %s", view->build && view->build[0] ? view->build : "?");
    overlayTextFit(box_x + 28, y, line, 24, box_w - 56, DIM);
    y += 42;
    snprintf(line, sizeof(line), "Log file: %s", view->log_path && view->log_path[0] ? view->log_path : "not available");
    overlayTextFit(box_x + 28, y, line, 24, box_w - 56, DIM);
    y += 52;
    overlayTextFit(box_x + 28, y, "Log shows this start, the previous start and the graphics driver log.", 24, box_w - 56, FAINT);
}
static void drawUpdatesTab(const LoadingView *view) {
    const float box_x = 540, box_y = 190, box_w = OVERLAY_WIDTH - box_x - 120, box_h = 430;
    overlayRect(box_x, box_y, box_w, box_h, PANEL);
    overlayText(box_x + 28, box_y + 26, "Updates", 30, TEXT);
    char line[300];
    snprintf(line, sizeof(line), "Installed: Rev %s", view->loader_revision && view->loader_revision[0] ? view->loader_revision : "?");
    overlayTextFit(box_x + 28, box_y + 78, line, 26, box_w - 56, SOFT);
    const char *status = view->update_status && view->update_status[0] ? view->update_status : "Press Cross to check GitHub for a newer release.";
    overlayTextFit(box_x + 28, box_y + 126, status, 26, box_w - 56, TEXT);
    overlayTextFit(box_x + 28, box_y + 196, "The loader can now replace its own image, from a private GitHub release", 23, box_w - 56, DIM);
    overlayTextFit(box_x + 28, box_y + 226, "named in /data/pokeupdate.ini: press Cross to check, then install it here.", 23, box_w - 56, DIM);
}
static void drawInterfaceTab(const LoadingView *view) {
    const float box_x = 540, box_y = 190, box_w = OVERLAY_WIDTH - box_x - 120, box_h = 430;
    overlayRect(box_x, box_y, box_w, box_h, PANEL);
    overlayText(box_x + 28, box_y + 26, "Interface size", 30, TEXT);
    overlayTextFit(box_x + 28, box_y + 76, "The client's own UI Scaling stops at 200%. The loader draws the game at a", 23, box_w - 56, SOFT);
    overlayTextFit(box_x + 28, box_y + 104, "slightly smaller size and stretches it back over the screen, so the interface", 23, box_w - 56, SOFT);
    overlayTextFit(box_x + 28, box_y + 132, "can be nudged larger in 5% steps. Larger sizes soften the picture.", 23, box_w - 56, SOFT);
    unsigned percent = view->interface_scale ? view->interface_scale : 100u;
    if (percent < 100u) percent = 100u;
    if (percent > 150u) percent = 150u;
    const float track_x = box_x + 60, track_w = box_w - 120, track_h = 16;
    const float track_y = box_y + 268;
    char label[32];
    snprintf(label, sizeof(label), "%u%%", percent);
    overlayTextCentered(box_x + box_w / 2, box_y + 178, label, 56, TEXT);
    overlayRect(track_x, track_y, track_w, track_h, 0x1C1030FFu);
    float fraction = (float)(percent - 100u) / 50.0f;
    overlayRect(track_x, track_y, track_w * fraction, track_h, PURPLE);
    for (unsigned step = 0; step <= 10; ++step) {
        float x = track_x + track_w * (float)step / 10.0f;
        bool chosen = 100u + step * 5u == percent;
        overlayRect(x - 1.0f, track_y - (chosen ? 12.0f : 6.0f), 2.0f, track_h + (chosen ? 24.0f : 12.0f), chosen ? PURPLE : 0x3A2752FFu);
    }
    snprintf(label, sizeof(label), "Interface size: %u%%", percent);
    overlayTextCentered(box_x + box_w / 2, track_y + track_h + 44, label, 28, TEXT);
    overlayTextCentered(box_x + box_w / 2, track_y + track_h + 82, "Applies when the game starts", 22, FAINT);
}
static void drawLogTab(const LoadingView *view) {
    const LogView *log = view->log;
    const char *title = log && log->title ? log->title : "Log";
    const float panel_x = 540, panel_y = 190, panel_w = OVERLAY_WIDTH - 660, panel_h = 790;
    overlayRect(panel_x, panel_y, panel_w, panel_h, 0x0B0616FFu);
    overlayFrame(panel_x, panel_y, panel_w, panel_h, 1, 0x3A2752FFu);
    overlayText(panel_x + 28, panel_y + 14, title, 28, TEXT);
    char count[64] = "";
    if (log && log->line_count) snprintf(count, sizeof(count), "%u lines", log->line_count);
    if (count[0]) overlayText(panel_x + 28 + overlayTextWidth(title, 28) + 24, panel_y + 18, count, 22, FAINT);
    if (log && log->path && log->path[0]) overlayTextFit(panel_x + 28, panel_y + 46, log->path, 21, panel_w - 56, DIM);

    const unsigned visible = 24;
    const float line_height = 27, first_y = panel_y + 78;
    if (!log || !log->line_count) {
        const char *message = log && log->missing ? "No log file has been written yet." : "The log is empty.";
        overlayTextCentered(panel_x + panel_w / 2, 500, message, 30, DIM);
        if (log && log->path && log->path[0]) overlayTextCentered(panel_x + panel_w / 2, 548, log->path, 24, FAINT);
        return;
    }
    unsigned max_first = log->line_count > visible ? log->line_count - visible : 0;
    unsigned scroll = log->scroll > max_first ? max_first : log->scroll;
    unsigned first = max_first - scroll;
    unsigned shown = log->line_count - first < visible ? log->line_count - first : visible;
    for (unsigned row = 0; row < shown; ++row) {
        unsigned index = first + row;
        float y = first_y + (float)row * line_height;
        snprintf(count, sizeof(count), "%u", index + 1);
        overlayText(panel_x + 28, y, count, 20, FAINT);
        overlayTextFit(panel_x + 92, y, log->lines[index], 21, panel_w - 120, index + 1 == log->line_count ? TEXT : SOFT);
    }
    if (scroll) {
        snprintf(count, sizeof(count), "%u newer", scroll);
        overlayText(panel_x + panel_w - 28 - overlayTextWidth(count, 22), panel_y + 18, count, 22, FAINT);
    }
}
static void drawSettings(const LoadingView *view) {
    overlayClear(0x0B0616FFu);
    overlayText(120, 78, "Settings", 44, TEXT);
    drawSettingsList(view->settings_tab);
    if (view->settings_tab == SETTINGS_LOG) {
        drawLogTab(view);
        drawFooterHints(view, "Up/Down Choose     L1/R1 Page     Square Switch log     Circle Back");
    } else if (view->settings_tab == SETTINGS_UPDATES) {
        drawUpdatesTab(view);
        drawFooterHints(view, "Cross Check / install     Up/Down Choose     Circle Back");
    } else if (view->settings_tab == SETTINGS_INTERFACE) {
        drawInterfaceTab(view);
        drawFooterHints(view, "Left/Right Change     Up/Down Choose     Circle Back");
    } else {
        drawDetailsTab(view);
        drawFooterHints(view, "Up/Down Choose     Circle Back");
    }
}

// ---- the loading screen ----------------------------------------------------------------------------------------------------------------
void loadingScreenDraw(const LoadingView *view) {
    if (view->settings) {
        drawSettings(view);
        return;
    }
    overlayClear(BACKGROUND);
    if (view->notice && view->notice[0]) {  // loader-39: a small banner at the top
        float w = overlayTextWidth(view->notice, 28) + 48;
        overlayRect(OVERLAY_WIDTH / 2 - w / 2, 36, w, 50, 0x2A1B0EFFu);
        overlayTextCentered(OVERLAY_WIDTH / 2, 46, view->notice, 28, AMBER);
    }
    overlayLogo(OVERLAY_WIDTH / 2 - LOGO_SIZE * (float)overlay_logo_width / (float)overlay_logo_height / 2, LOGO_Y, LOGO_SIZE);
    overlayTextCentered(OVERLAY_WIDTH / 2, WORDMARK_Y, "PokeMMO", WORDMARK_SIZE, TEXT);
    const float bar_x = 560, bar_y = BAR_Y, bar_w = 800, bar_h = 12;
    float fraction = view->fraction < 0 ? 0 : view->fraction > 1 ? 1 : view->fraction;
    overlayRect(bar_x, bar_y, bar_w, bar_h, TRACK);
    if (view->problem) {
        overlayRect(bar_x, bar_y, bar_w * (fraction > 0.05f ? fraction : 0.05f), bar_h, RED);
        overlayTextCentered(OVERLAY_WIDTH / 2, bar_y + 36, view->problem, 44, TEXT);
        if (view->advice) drawWrapped(bar_y + 96, view->advice, 32, 1100, 0xC9B6E4FFu);
    } else if (view->question) {
        overlayRect(bar_x, bar_y, bar_w * fraction, bar_h, PURPLE);
        overlayTextCentered(OVERLAY_WIDTH / 2, bar_y + 36, view->question, 40, TEXT);
        if (view->choices) overlayTextCentered(OVERLAY_WIDTH / 2, bar_y + 96, view->choices, 34, SOFT);
    } else if (view->ready) {
        overlayRect(bar_x, bar_y, bar_w * fraction, bar_h, PURPLE);  // the menu below is the state: no label over the bar
    } else {
        overlayRect(bar_x, bar_y, bar_w * fraction, bar_h, PURPLE);
        const char *status = view->status ? view->status : "";
        char line[128];
        snprintf(line, sizeof(line), "%s%.*s", status, (int)((view->frame / 30) % 4), "...");
        overlayText(OVERLAY_WIDTH / 2 - overlayTextWidth(status, 40) / 2, bar_y + 36, line, 40, SOFT);  // the dots do not move the words
        if (view->detail && view->detail[0]) overlayTextCentered(OVERLAY_WIDTH / 2, bar_y + 96, view->detail, 32, DIM);
    }
    if (view->warning && !view->problem && !view->question)
        overlayTextCentered(OVERLAY_WIDTH / 2, view->ready ? BAR_Y + 50 : BAR_Y + 150, view->warning, 30, AMBER);
    if (view->ready) {
        drawMenu(view->menu);
        char size[64];
        snprintf(size, sizeof(size), "Interface size: %u%%     L1/R1 Change (100%%-150%%)", view->interface_scale ? view->interface_scale : 100u);
        overlayTextCentered(OVERLAY_WIDTH / 2, MENU_Y + 118, size, 24, SOFT);
    }
    const char *middle = view->ready ? NULL : view->no_input ? NULL : "\x04 Settings";
    drawFooterHints(view, middle);
}
