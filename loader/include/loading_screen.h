// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
// The loading screen (loading_screen.c): the logo and the start-up state; once the client is ready, the Start Game /
// Settings / Exit menu. Settings shows the start-up steps and the log of this or the previous start. The platform
// decides what to show (main_ps5.c), this only draws it, between overlayBegin and overlayEnd.
#pragma once
#include <stdbool.h>

enum { LOADING_NOT_RUN = 0, LOADING_PASS, LOADING_FAIL, LOADING_INFO, LOADING_RUNNING };
typedef struct {
    const char *name;
    int state;
} LoadingStep;
enum { SETTINGS_DETAILS = 0, SETTINGS_LOG = 1, SETTINGS_UPDATES = 2, SETTINGS_INTERFACE = 3, SETTINGS_ITEMS = 4 };
typedef struct {
    const char *title;       // "This start's log" or "Previous start's log"
    const char *path;        // the file being shown
    const char *const *lines;
    unsigned line_count;
    unsigned scroll;         // lines scrolled up from the newest line (0: the end)
    bool missing;            // the file could not be read
} LogView;
typedef struct {
    float fraction;       // 0..1
    const char *status;   // "Installing PokeMMO" (animated dots are added)
    const char *detail;   // second line, may be empty
    const char *problem;  // set when starting cannot go on: the bar turns red
    const char *advice;   // what to do about the problem
    const char *warning;  // a note that does not stop anything
    const char *question; // a choice for the player (an update), shown instead of the status
    const char *choices;  // its buttons
    const char *version;  // the PokeMMO client version, e.g. "v1.0.2" (Settings > Startup details)
    const char *build;    // the loader's build identity, e.g. "v1.0.2-1-g76239f7-dirty"
    const char *revision; // the PokeMMO client revision, e.g. "32920" (Settings > Startup details)
    const char *loader_revision;  // our own revision, e.g. "v1.0": bottom centre and Settings > Startup details
    const char *notice;   // loader-39: a newer release is out (a small banner at the top), or NULL
    const char *log_path; // the log file (Settings > Startup details)
    const LoadingStep *steps;
    unsigned step_count;
    bool ready;           // the client is ready: show the Start Game / Settings / Exit menu
    int menu;             // selected menu entry: 0 Start Game, 1 Settings, 2 Exit
    bool settings;        // the Settings screen is up
    int settings_tab;     // the selected Settings entry (SETTINGS_DETAILS, SETTINGS_LOG, SETTINGS_UPDATES, SETTINGS_INTERFACE)
    unsigned interface_scale;  // loader-47: the interface size as a percentage, 100..150 (Settings > Interface size)
    const LogView *log;   // the log being shown when settings_tab is SETTINGS_LOG
    const char *update_status;  // "Check for updates" result: idle, checking, up to date, newer release
    bool no_input;        // the client is starting and the controller is not read: no hints
    unsigned frame;
} LoadingView;
void loadingScreenDraw(const LoadingView *view);
