// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// Runs the official client inside the loader on a Linux PC: the same ELF loader and Linux ABI adapters as the PS5 title, on the
// host platform layer. Used to develop the adapters with ordinary tools before anything is tried on a console.
//
//   pokemmo-host --client <folder with bin/linux/x64/PokeMMO, data/, config/> --root <scratch folder> [--timeout seconds] [-- client options]
#include "diagnostics.h"
#include "game.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    const char *client = NULL, *root = NULL;
    unsigned timeout = 120;
    const char *options[16] = {"-XX:MaxHeapSize=2048m", "-XX:MaxNewSize=512m", "-XX:MaxGCPauseMillis=10", NULL};
    unsigned option_count = 3;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--client") && i + 1 < argc)
            client = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
            timeout = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--")) {
            option_count = 0;
            for (++i; i < argc && option_count < 15; ++i) options[option_count++] = argv[i];
            options[option_count] = NULL;
        } else {
            fprintf(stderr, "usage: %s --client DIR --root DIR [--timeout SECONDS] [-- client options]\n", argv[0]);
            return 2;
        }
    }
    if (!client || !root) {
        fprintf(stderr, "--client and --root are required\n");
        return 2;
    }
    mkdir(root, 0700);
    char game[512], executable[600];
    snprintf(game, sizeof(game), "%s/game", root);
    mkdir(game, 0700);  // the mount point must exist for chdir("/game")
    snprintf(executable, sizeof(executable), "%s/bin/linux/x64/PokeMMO", client);
    GameConfig config = {.root = root, .client_path = executable, .mounts = {{"/game", client}}, .mount_count = 1, .arguments = options,
                         .timeout_seconds = timeout};
    diagnosticsTrace("host.start client=%s root=%s", client, root);
    bool ok = gameRun(&config);
    diagnosticsTrace("host.end ok=%d failure=%s", ok, ok ? "" : gameFailure());
    fflush(stdout);
    _Exit(ok ? 0 : 1);  // the client's own threads are still running: leave without running anything else
}
