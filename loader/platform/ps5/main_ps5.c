// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// The PS5 title: startup self-checks, then the official client inside the loader. The screen shows the loading state,
// and once the client is ready a Start Game / Settings / Exit menu; Settings shows the startup steps and a live log viewer.
// Everything is logged over UDP (port 18194, tools/udplog.py) and to the platform log file.
//
// None of the checks can take the title down: risky calls run under a fault guard, and calls that might block run on their own
// thread with a time limit.
#include "diagnostics.h"
#include "game.h"
#include "linux_audio.h"
#include "linux_gtk.h"
#include "linux_sdl.h"
#include "loading_screen.h"
#include "overlay.h"
#include "platform.h"
#include "slots.h"
#include "updater.h"
#include "release_check.h"
#include "pokeupdate.h"  // loader-49: updating the loader image itself from a private GitHub release
#include "pokemmo_version.h"  // POKEMMO_VERSION (git), POKEMMO_RELEASE (VERSION), POKEMMO_TITLE_ID (generated at build time)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#define LOADER_MILESTONE "loader-86"
// ps5-opengl's app heap (malloc of the loader and of everything the client allocates with malloc): from direct memory.
const size_t ps5_opengl_heap_size = 768u << 20;

int sceKernelUsleep(unsigned int microseconds);
int sceKernelLoadStartModule(const char *path, size_t argument_size, const void *arguments, uint32_t flags, void *options, int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceUserServiceInitialize(void *parameters);
int sceUserServiceGetInitialUser(int *user);
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}

// ---- steps shown on screen ------------------------------------------------------------------------------------------------------
enum { NOT_RUN = 0, PASS, FAIL, INFO, RUNNING };
typedef struct {
    const char *name;
    _Atomic int state;
} Step;
// loader-8: the loading screen shows one bar; these are the details behind it (Settings). The ROMs are preloaded in the title
// (assets/roms) and only listed, not identified: the client uses the files directly.
static Step steps[] = {
    {"fs.list.app0", NOT_RUN}, {"fs.list.roms", NOT_RUN}, {"client.update", NOT_RUN}, {"client.map", NOT_RUN}, {"client.start", NOT_RUN}, {"client.end", NOT_RUN},
};
#define STEP_COUNT (sizeof(steps) / sizeof(*steps))
static void mark(const char *name, int state) {
    for (size_t i = 0; i < STEP_COUNT; ++i)
        if (!strcmp(steps[i].name, name)) atomic_store(&steps[i].state, state);
    if (state != RUNNING)
        say("RESULT %s %s", name, state == PASS ? "PASS" : state == FAIL ? "FAIL" : state == INFO ? "INFO" : "NOT_RUN");
    else
        say("BEGIN %s", name);
}

// ---- fault guard (as in the probe): a fault inside a guarded call returns here instead of ending the title -------------------------
static __thread sigjmp_buf guard_point;
static __thread volatile sig_atomic_t guard_armed;
static _Atomic int fatal_signals;
// A fault outside a guarded call (the client itself, most likely): report where, then park the faulting thread for good instead of
// ending the title, so the screen and the log stay up. Other threads keep running.
static void guardHandler(int signal_number, siginfo_t *info, void *context) {
    if (guard_armed) {
        guard_armed = 0;
        siglongjmp(guard_point, signal_number);
    }
    ucontext_t *state = context;
    char line[200];
    snprintf(line, sizeof(line), "FATAL signal %d address=%p rip=0x%llx rsp=0x%llx (thread parked)", signal_number, info ? info->si_addr : NULL,
             state ? (unsigned long long)state->uc_mcontext.mc_rip : 0ull, state ? (unsigned long long)state->uc_mcontext.mc_rsp : 0ull);
    platformLogLine(line);
    atomic_fetch_add(&fatal_signals, 1);
    for (;;) sceKernelUsleep(1000000);
}
static void guardInstall(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guardHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NODEFER | SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
}
// Runs `body` (a statement block) under the guard; `faulted` receives the signal number or 0.
#define GUARDED(faulted, body)                                     \
    do {                                                           \
        int signal_number_ = sigsetjmp(guard_point, 1);            \
        if (!signal_number_) {                                     \
            guard_armed = 1;                                       \
            body;                                                  \
            guard_armed = 0;                                       \
        }                                                          \
        (faulted) = signal_number_;                                \
    } while (0)

// ---- file checks -------------------------------------------------------------------------------------------------------------------
static unsigned listFolder(const char *step, const char *path, char names[][256], unsigned max) {
    mark(step, RUNNING);
    int error = 0;
    PlatformDirectory *directory = platformDirectoryOpen(path, &error);
    if (!directory) {
        say("fs list %s: open failed errno=%d (opendir is refused in titles; no .pokemmo-index fallback either)", path, error);
        mark(step, FAIL);
        return 0;
    }
    char name[256], shown[600] = "";
    uint8_t type;
    uint64_t inode;
    unsigned count = 0;
    int result;
    while ((result = platformDirectoryRead(directory, name, &type, &inode, &error)) == 1) {
        if (names && count < max) snprintf(names[count], 256, "%s", name);
        if (count < 20 && strlen(shown) + strlen(name) + 4 < sizeof(shown)) {
            strcat(shown, name);
            strcat(shown, type == 4 ? "/ " : " ");
        }
        ++count;
    }
    platformDirectoryClose(directory);
    say("fs list %s: %u entries%s: %s", path, count, result < 0 ? " (stopped by an error)" : "", shown);
    if (result < 0) say("fs list %s: read error errno=%d", path, error);
    mark(step, result < 0 ? FAIL : PASS);
    return count;
}

// ---- ROMs ----------------------------------------------------------------------------------------------------------------------------
// The build can copy .nds/.gba files into assets/roms, and the client mounts that folder as /game/roms. A published package ships none
// (they are copyrighted), and an image install is read-only, so the loader also accepts a folder the player fills over FTP. The title's
// own folder wins when it has game files; otherwise this one is mounted, which is what a new user sees.
#define ROM_FOLDER_TITLE "/app0/assets/roms"
#define ROM_FOLDER_USER "/data/PokeMMO-roms"
static char rom_folder[192] = ROM_FOLDER_TITLE;
static bool romFileName(const char *name) {
    size_t length = strlen(name);
    if (length < 5) return false;
    const char *suffix = name + length - 4;
    char a = suffix[0], b = suffix[1], c = suffix[2], d = suffix[3];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (d >= 'A' && d <= 'Z') d += 32;
    return a == '.' && ((b == 'n' && c == 'd' && d == 's') || (b == 'g' && c == 'b' && d == 'a'));
}
static bool folderHasRoms(const char *path) {
    int error = 0;
    PlatformDirectory *directory = platformDirectoryOpen(path, &error);
    if (!directory) return false;
    char name[256];
    uint8_t type;
    uint64_t inode;
    bool found = false;
    while (!found && platformDirectoryRead(directory, name, &type, &inode, &error) == 1) found = type != 4 && romFileName(name);
    platformDirectoryClose(directory);
    return found;
}
static void chooseRomFolder(void) {
    if (folderHasRoms(ROM_FOLDER_TITLE)) {
        snprintf(rom_folder, sizeof(rom_folder), "%s", ROM_FOLDER_TITLE);
        return;
    }
    mkdir(ROM_FOLDER_USER, 0755);
    snprintf(rom_folder, sizeof(rom_folder), "%s", ROM_FOLDER_USER);
    say("roms: the title has none; using %s (%s)", rom_folder,
        folderHasRoms(ROM_FOLDER_USER) ? "game files found" : "put your .nds/.gba files there");
}
static bool title_writable;
static void chooseStorage(void) {
    int fd = open("/app0/.pokemmo-write-check", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    title_writable = fd >= 0;
    if (fd >= 0) {
        close(fd);
        unlink("/app0/.pokemmo-write-check");
        unlink("/app0/pokemmo-write-test.bin");  // loader-1's probe file
    } else {
        mkdir("/download0/root", 0755);
    }
    // loader-54: a folder install must be the only install. A leftover .ffpfsc for this title would be a second candidate
    // ShadowMountPlus keeps ignoring as a "duplicate"; the loader removes it (and any interrupted staging file) itself.
    if (title_writable) pokeupdateRemoveLeftoverImage();
    say("storage: title folder %s; ROMs in %s; log in %s", title_writable ? "writable (folder install)" : "read-only (image install)", rom_folder,
        platformLogPath()[0] ? platformLogPath() : "(none)");
}
// ---- ps5-opengl's log ---------------------------------------------------------------------------------------------------------------
// The driver's own log is the one place a rendering fault names itself (unsupported formats, skipped draws, shader
// warnings), and it lives on /download0, which FTP does not expose. Copy the last 4 MiB of the previous start's log into the
// title folder at every start, so it can be fetched exactly like pokemmo.log.
#define DRIVER_LOG_PATH "/download0/ps5-opengl.log"
#define DRIVER_LOG_COPY "/app0/ps5-opengl.log"
static void copyDriverLog(void) {
    if (!title_writable) return;
    int input = open(DRIVER_LOG_PATH, O_RDONLY);
    if (input < 0) return;
    struct stat info;
    off_t length = 0;
    if (!fstat(input, &info) && info.st_size > 0) length = info.st_size;
    if (!length) {
        close(input);
        return;
    }
    off_t start = length > (4 << 20) ? length - (4 << 20) : 0;
    if (start) lseek(input, start, SEEK_SET);
    int output = open(DRIVER_LOG_COPY, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output < 0) {
        close(input);
        return;
    }
    static char buffer[16384];
    ssize_t got;
    bool ok = true;
    while (ok && (got = read(input, buffer, sizeof(buffer))) > 0) {
        ssize_t done = 0;
        while (done < got) {
            ssize_t wrote = write(output, buffer + done, (size_t)(got - done));
            if (wrote <= 0) {
                ok = false;
                break;
            }
            done += wrote;
        }
    }
    close(output);
    close(input);
    say("driver log copied to %s (%lld bytes, from the start before this one)", DRIVER_LOG_COPY, (long long)length);
}
// The driver truncates its log at start, so the copy taken at startup is only its banner. Copy it again once a minute while
// the title runs (the loader keeps running the client on its own thread), which is what makes the live log readable.
static void *driverLogCopyMain(void *argument) {
    (void)argument;
    for (unsigned minute = 0; minute < 240; ++minute) {
        sceKernelUsleep(60000000);
        copyDriverLog();
    }
    return NULL;
}
static char release_notice[160];  // loader-39
static _Atomic bool release_notice_set;

// ---- the client: in the title storage, where it can write next to itself --------------------------------------------------------------
#define ROOT "/download0/root"
// loader-18: the client lives in one of two slots (slots.c); `active_slot` is the one that starts, `target_slot` receives a
// new client (the other one) before it is switched to.
static SlotState slot_state;
static pthread_mutex_t slot_lock = PTHREAD_MUTEX_INITIALIZER;
static char active_dir[300];
static char active_revision[64];  // of the active slot, "" when there is no client
static bool readSmall(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t got = read(fd, out, size - 1);
    close(fd);
    out[got > 0 ? got : 0] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return got > 0;
}
static bool makeParents(const char *path) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *slash = copy + 1; (slash = strchr(slash, '/')); ++slash) {
        *slash = 0;
        if (mkdir(copy, 0755) && errno != EEXIST) return false;
        *slash = '/';
    }
    return true;
}
static bool copyFile(const char *from, const char *to, char *buffer, size_t size) {
    int in = open(from, O_RDONLY);
    if (in < 0) {
        say("client copy: open %s errno=%d", from, errno);
        return false;
    }
    if (!makeParents(to)) {
        close(in);
        return false;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = out >= 0;
    while (ok) {
        ssize_t got = read(in, buffer, size);
        if (got < 0) ok = false;
        if (got <= 0) break;
        for (ssize_t done = 0; done < got && ok;) {
            ssize_t wrote = write(out, buffer + done, (size_t)(got - done));
            if (wrote <= 0)
                ok = false;
            else
                done += wrote;
        }
    }
    if (!ok) say("client copy: %s -> %s failed errno=%d", from, to, errno);
    close(in);
    if (out >= 0) close(out);
    return ok;
}
static char client_revision[64];                                  // shown on the loading screen
static void noteRevision(const char *revision) {
    size_t length = strcspn(revision, "\r\n");
    snprintf(client_revision, sizeof(client_revision), "%.*s", (int)(length < 60 ? length : 60), revision);
}
static void switchTo(char slot, const char *revision);
// ---- the client updater (loader-15): the published client, read in place on PokeMMO's server (updater.c) ------------------------
// A newer revision is offered on the loading screen (Cross: download, Circle: skip; download after a few seconds); without any
// client installed it is downloaded straight away. The published ETag of the installed revision is kept, so an unchanged
// client costs one HEAD request.
#define UPDATE_ETAG ROOT "/update-etag"  // the published zip's ETag when the active slot holds its revision
#define PROMPT_SECONDS 6
enum { UPDATE_IDLE, UPDATE_CHECKING, UPDATE_PROMPT, UPDATE_DOWNLOADING, UPDATE_APPLYING };
enum { CHOICE_NONE, CHOICE_DOWNLOAD, CHOICE_SKIP };
static _Atomic int update_phase, update_choice;
static _Atomic uint64_t prompt_deadline_ns;
static UpdaterProgress update_progress;
static char update_question[128];
static const char *_Atomic start_problem, *_Atomic start_advice;  // why the game cannot start, for the loading screen
static char update_warning[200];
static _Atomic bool update_warning_set;

// ---- the loader image's own update (loader-49; folder installs loader-52) -------------------------------------------------------------
// The title can be updated in place, from a private GitHub release named by /data/pokeupdate.ini. The whole feature is
// inert without that file (main_ps5.c never even starts the check): the token lives only in the file, and nothing is
// asked until the client is ready. In a folder install (title_writable) the executable itself is replaced and the next
// launch runs the new build; in an image install a full .ffpfsc image only takes effect at the next start, so after a
// success the player is told to restart rather than the running title changing underfoot.
enum { IMAGE_IDLE, IMAGE_PROMPT, IMAGE_DOWNLOADING, IMAGE_APPLYING };
static _Atomic int image_phase;
static _Atomic bool image_available, image_asked;
static char image_question[160];
static const char *const image_choices = "\x01 Yes        \x02 No";  // \x01 and \x02 are the Cross and Circle glyphs
static PokeUpdateConfig image_config;
static PokeUpdateRemote image_remote;
static PokeUpdateProgress image_progress;

static bool writeSmall(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}
// ---- slots (loader-18) ------------------------------------------------------------------------------------------------------------
// A new client (download or developer copy) goes into the other slot and is switched to "on trial"; the trial ends when the
// game shows its first picture (gameShowedPicture). A start that finds a trial still open goes back to the previous slot.
static void switchTo(char slot, const char *revision) {
    pthread_mutex_lock(&slot_lock);
    if (slot_state.active && slot_state.active != slot) slot_state.previous = slot_state.active;
    slot_state.active = slot;
    slot_state.trial = true;
    slotsSave(&slot_state);
    slotsPath(slot, active_dir, sizeof(active_dir));
    snprintf(active_revision, sizeof(active_revision), "%s", revision);
    pthread_mutex_unlock(&slot_lock);
    noteRevision(revision);
    say("slots: switched to slot %c (revision %s), on trial until the game shows a picture", slot, revision);
}
// loader-21: once a new revision has proven itself, the previous client is deleted (on its own thread: a couple of hundred files),
// so the second copy only exists while an update is on trial.
static void *removeSlotMain(void *argument) {
    slotsRemove((char)(uintptr_t)argument);
    return NULL;
}
static void gameShowedPicture(void) {
    pthread_mutex_lock(&slot_lock);
    char old = 0;
    if (slot_state.trial) {
        slot_state.trial = false;
        slot_state.failed[0] = 0;
        old = slot_state.previous;
        slot_state.previous = 0;
        slotsSave(&slot_state);
        say("slots: revision %s started: slot %c confirmed%s", active_revision, slot_state.active, old ? ", the previous client is removed" : "");
    }
    pthread_mutex_unlock(&slot_lock);
    pthread_t thread;
    if (old && !pthread_create(&thread, NULL, removeSlotMain, (void *)(uintptr_t)old)) pthread_detach(thread);
}
// The game ended before its first picture while on trial: back to the previous slot for the next start.
static bool revertTrial(const char *why) {
    pthread_mutex_lock(&slot_lock);
    char revision[64] = "";
    bool reverted = false;
    if (slot_state.trial && slot_state.previous && slotsRevision(slot_state.previous, revision, sizeof(revision))) {
        snprintf(slot_state.failed, sizeof(slot_state.failed), "%s", active_revision);
        char failed = slot_state.active;
        slot_state.active = slot_state.previous;
        slot_state.previous = failed;
        slot_state.trial = false;
        slotsSave(&slot_state);
        say("slots: revision %s did not start (%s): back to slot %c, revision %s", active_revision, why, slot_state.active, revision);
        reverted = true;
    }
    pthread_mutex_unlock(&slot_lock);
    return reverted;
}
static char slot_notice[160];
static void prepareSlots(void) {
    slotsInit(ROOT);
    slotsMigrate();
    if (!slotsLoad(&slot_state)) memset(&slot_state, 0, sizeof(slot_state));
    if (slot_state.trial) {
        char failed[64];
        snprintf(failed, sizeof(failed), "%s", "");
        slotsRevision(slot_state.active, failed, sizeof(failed));
        snprintf(active_revision, sizeof(active_revision), "%s", failed);
        if (revertTrial("the previous start ended before the game showed a picture")) {
            char now[64] = "";
            slotsRevision(slot_state.active, now, sizeof(now));
            snprintf(slot_notice, sizeof(slot_notice), "PokeMMO revision %s did not start (it may need a newer loader): back to %s.", failed, now);
        }
    }
    active_revision[0] = 0;
    if (slot_state.active && !slotsRevision(slot_state.active, active_revision, sizeof(active_revision))) slot_state.active = 0;
    if (slot_state.active && !slot_state.trial && slot_state.previous) {  // a confirmed client from before loader-21 kept the old one
        say("slots: removing the previous client (slot %c)", slot_state.previous);
        slotsRemove(slot_state.previous);
        slot_state.previous = 0;
        slotsSave(&slot_state);
    }
    if (slot_state.active) slotsPath(slot_state.active, active_dir, sizeof(active_dir));
    noteRevision(active_revision);
    say("slots: active %c (revision %s), previous %c, trial %d, failed %s", slot_state.active ? slot_state.active : '-',
        active_revision[0] ? active_revision : "none", slot_state.previous ? slot_state.previous : '-', slot_state.trial, slot_state.failed[0] ? slot_state.failed : "-");
}

static void updateClient(bool forget_installed) {
    mark("client.update", RUNNING);
    atomic_store(&update_phase, UPDATE_CHECKING);
    char installed[64] = "", known_etag[160] = "", error[256] = "";
    bool have_client = !forget_installed && active_revision[0];
    if (have_client) snprintf(installed, sizeof(installed), "%s", active_revision);
    readSmall(UPDATE_ETAG, known_etag, sizeof(known_etag));
    UpdaterRemote remote;
    int checked = updaterCheck(UPDATER_URL, have_client ? known_etag : NULL, &remote, error, sizeof(error));
    say("client update: installed %s, check %d, published %s (etag %s)%s%s", have_client ? installed : "(none)", checked, remote.revision[0] ? remote.revision : "-",
        remote.etag, error[0] ? ": " : "", error);
    if (checked < 0) {
        atomic_store(&update_phase, UPDATE_IDLE);
        if (have_client) {
            snprintf(update_warning, sizeof(update_warning), "Could not check for updates: starting revision %s.", installed);
            atomic_store(&update_warning_set, true);
            mark("client.update", INFO);
        } else {
            atomic_store(&start_advice, "Check the console's internet connection, then close the title and start it again.");
            atomic_store(&start_problem, "PokeMMO could not be downloaded");
            mark("client.update", FAIL);
        }
        updaterFree(&remote);
        return;
    }
    bool newer = checked == 0 && updaterCompareRevisions(remote.revision, installed) > 0 && strcmp(remote.revision, slot_state.failed);
    if (checked == 0 && !strcmp(remote.revision, slot_state.failed)) say("client update: revision %s did not start before: not offered again", remote.revision);
    if (!newer) {
        if (checked == 0 && !updaterCompareRevisions(remote.revision, installed)) writeSmall(UPDATE_ETAG, remote.etag);
        say("client update: up to date");
        atomic_store(&update_phase, UPDATE_IDLE);
        mark("client.update", PASS);
        updaterFree(&remote);
        return;
    }
    if (have_client) {
        snprintf(update_question, sizeof(update_question), "PokeMMO update: revision %s to %s (%llu MB)", installed, remote.revision,
                 (unsigned long long)(remote.download_bytes >> 20));
        atomic_store(&update_choice, CHOICE_NONE);
        atomic_store(&prompt_deadline_ns, platformMonotonicNs() + PROMPT_SECONDS * 1000000000ull);
        atomic_store(&update_phase, UPDATE_PROMPT);
        while (atomic_load(&update_choice) == CHOICE_NONE && platformMonotonicNs() < atomic_load(&prompt_deadline_ns)) sceKernelUsleep(20000);
        if (atomic_load(&update_choice) == CHOICE_SKIP) {
            say("client update: skipped by the player");
            snprintf(update_warning, sizeof(update_warning), "Update skipped: starting revision %s.", installed);
            atomic_store(&update_warning_set, true);
            atomic_store(&update_phase, UPDATE_IDLE);
            mark("client.update", INFO);
            updaterFree(&remote);
            return;
        }
    }
    atomic_store(&update_phase, UPDATE_DOWNLOADING);
    char slot = slotsOther(slot_state.active ? slot_state.active : 'b'), target[300];
    slotsPath(slot, target, sizeof(target));
    slotsClear(slot);
    uint64_t start = platformMonotonicNs();
    int result = updaterDownload(UPDATER_URL, &remote, target, &update_progress, error, sizeof(error));
    double seconds = (double)(platformMonotonicNs() - start) / 1e9;
    say("client update: download %s, %llu MB in %.0f s%s%s", result ? "FAILED" : "done", (unsigned long long)(atomic_load(&update_progress.done) >> 20), seconds,
        error[0] ? ": " : "", error);
    if (!result) {
        atomic_store(&update_phase, UPDATE_APPLYING);
        result = slotsMarkComplete(slot, remote.revision) ? 0 : -1;
        if (!result) switchTo(slot, remote.revision);
        say("client update: slot %c %s", slot, result ? "could not be marked complete" : "ready");
    }
    atomic_store(&update_phase, UPDATE_IDLE);
    if (!result) {
        writeSmall(UPDATE_ETAG, remote.etag);
        mark("client.update", PASS);
    } else if (have_client) {
        snprintf(update_warning, sizeof(update_warning), "The update failed: starting revision %s.", installed);
        atomic_store(&update_warning_set, true);
        mark("client.update", INFO);
    } else {
        atomic_store(&start_advice, "Check the console's internet connection and free space, then close the title and start it again.");
        atomic_store(&start_problem, "PokeMMO could not be downloaded");
        mark("client.update", FAIL);
    }
    updaterFree(&remote);
}

// ---- the game's settings: a copy where FTP can see it ---------------------------------------------------------------------------
// The client keeps its settings in config/main.properties next to itself, in the title storage (not visible over FTP). A copy
// goes to the title folder at start and whenever the file changes (checked every minute): a backup, and the way to read it.
// loader-34: each console profile has its own settings (and PokeMMO's remembered login), in ROOT/users/<profile id>/config,
// mounted over the game's /game/config. The client and the ROMs stay shared. A profile's first start gets the defaults.
static char player_config[200] = ROOT "/users/default/config";
static char settings_path[240], settings_copy[240], defaults_mark[240];
#define SETTINGS settings_path
#define SETTINGS_COPY settings_copy
static char player_folder[64] = "default";  // the profile id: its backup is profile-<id>.tar
static void choosePlayer(void) {
    int id = -1;
    char name[64] = "", folder[64] = "default";
    if (platformUser(&id, name, sizeof(name))) snprintf(folder, sizeof(folder), "%d", id);
    snprintf(player_config, sizeof(player_config), "%s/users/%s/config", ROOT, folder);
    snprintf(player_folder, sizeof(player_folder), "%s", folder);
    snprintf(settings_path, sizeof(settings_path), "%s/main.properties", player_config);
    snprintf(defaults_mark, sizeof(defaults_mark), "%s/.pokemmo-defaults", player_config);
    snprintf(settings_copy, sizeof(settings_copy), "/app0/settings/%s/main.properties", folder);
    mkdir(ROOT, 0755);
    slotsMakeFolders(player_config);
    if (title_writable) {
        char copy_folder[200];
        snprintf(copy_folder, sizeof(copy_folder), "/app0/settings/%s", folder);
        slotsMakeFolders(copy_folder);
    }
    // The single settings folder of loader-18 to loader-33 belonged to whoever played: everyone starts fresh instead.
    struct stat info;
    if (!stat(ROOT "/shared", &info)) say("settings: the old shared settings were removed%s", slotsRemoveFolder(ROOT "/shared") ? "" : " (some files stayed)");
    say("settings: profile %s: %s", folder, player_config);
}
static void backupSettings(void) {
    if (!title_writable) return;  // an image install: there is nowhere to put the copy
    static time_t last_time;
    static off_t last_size = -1;
    struct stat info;
    if (stat(SETTINGS, &info) || (info.st_mtime == last_time && info.st_size == last_size)) return;
    static char buffer[65536];
    bool ok = copyFile(SETTINGS, SETTINGS_COPY, buffer, sizeof(buffer));
    say("settings: %s copied to %s (%lld bytes)%s", SETTINGS, SETTINGS_COPY, (long long)info.st_size, ok ? "" : " FAILED");
    if (ok) {
        last_time = info.st_mtime;
        last_size = info.st_size;
    }
}

// Default settings (assets/settings/defaults.properties, shipped in the title): written in full when the game has no settings yet,
// and applied once per DEFAULTS_VERSION to settings that exist, so that what the player changes afterwards stays. The version
// applied is kept in config/.pokemmo-defaults. Lines of other keys are left exactly as they were.
#define DEFAULTS "/app0/assets/defaults.properties"
#define DEFAULTS_MARK defaults_mark
static char *readWhole(const char *path) {
    int fd = open(path, O_RDONLY);
    struct stat info;
    char *text = NULL;
    if (fd >= 0 && !fstat(fd, &info) && info.st_size < (1 << 20) && (text = malloc((size_t)info.st_size + 1))) {
        ssize_t got = read(fd, text, (size_t)info.st_size);
        text[got > 0 ? got : 0] = 0;
    }
    if (fd >= 0) close(fd);
    return text;
}
static bool writeWhole(const char *path, const char *text) {
    char temporary[512];
    snprintf(temporary, sizeof(temporary), "%s.pokemmo-new", path);
    if (!makeParents(path)) return false;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t length = strlen(text), done = 0;
    while (done < length) {
        ssize_t wrote = write(fd, text + done, length - done);
        if (wrote <= 0) break;
        done += (size_t)wrote;
    }
    close(fd);
    if (done != length || rename(temporary, path)) {
        unlink(temporary);
        return false;
    }
    return true;
}
static void applyDefaults(void) {
    char *defaults = readWhole(DEFAULTS);
    if (!defaults) {
        say("settings defaults: %s missing", DEFAULTS);
        return;
    }
    int version = 0, applied = 0;
    const char *found = strstr(defaults, "DEFAULTS_VERSION=");
    if (found) version = atoi(found + 17);
    char mark[32] = "";
    if (readSmall(DEFAULTS_MARK, mark, sizeof(mark))) applied = atoi(mark);
    if (applied >= version) {
        free(defaults);
        return;
    }
    char *current = readWhole(SETTINGS);
    size_t capacity = (current ? strlen(current) : 0) + strlen(defaults) + 64;
    char *result = malloc(capacity);
    if (!result) {
        free(defaults);
        free(current);
        return;
    }
    result[0] = 0;
    // The player's lines, except those the defaults set.
    unsigned replaced = 0, added = 0;
    for (const char *line = current ? current : ""; *line;) {
        size_t length = strcspn(line, "\n");
        size_t key_length = strcspn(line, "=\n");
        bool overridden = false;
        if (line[0] != '#' && key_length < length) {
            for (const char *d = defaults; *d;) {
                size_t d_length = strcspn(d, "\n"), d_key = strcspn(d, "=\n");
                if (d[0] != '#' && d_key == key_length && !strncmp(d, line, key_length)) overridden = true;
                d += d_length;
                d += *d == '\n';
            }
        }
        if (!overridden) strncat(result, line, length), strcat(result, "\n");
        replaced += overridden;
        line += length;
        line += *line == '\n';
    }
    for (const char *d = defaults; *d;) {  // then the defaults' own lines
        size_t d_length = strcspn(d, "\n");
        if (d[0] != '#' && d[0] != '\r' && d_length && strncmp(d, "DEFAULTS_VERSION=", 17)) {
            strncat(result, d, d_length);
            strcat(result, "\n");
            ++added;
        }
        d += d_length;
        d += *d == '\n';
    }
    bool ok = writeWhole(SETTINGS, result);
    if (ok) {
        snprintf(mark, sizeof(mark), "%d\n", version);
        ok = writeWhole(DEFAULTS_MARK, mark);
    }
    say("settings defaults: version %d applied to %s settings (%u lines set, %u replaced)%s", version, current ? "existing" : "new", added, replaced,
        ok ? "" : " FAILED");
    free(result);
    free(defaults);
    free(current);
}

// Replace one key's value in the player's settings, appending it when the client has not written it yet. Used for the
// values that depend on the display size the loader chose at this start (the client's interface does not scale by itself).
static bool setSettingAt(const char *path, const char *key, const char *value) {
    char *current = readWhole(path);
    if (!current) return false;
    size_t key_length = strlen(key);
    char *result = malloc(strlen(current) + key_length + strlen(value) + 32);
    if (!result) {
        free(current);
        return false;
    }
    result[0] = 0;
    bool found = false;
    for (const char *line = current; *line;) {
        size_t length = strcspn(line, "\n");
        if (!found && length > key_length && !strncmp(line, key, key_length) && line[key_length] == '=') {
            strcat(result, key);
            strcat(result, "=");
            strcat(result, value);
            strcat(result, "\n");
            found = true;
        } else {
            strncat(result, line, length);
            strcat(result, "\n");
        }
        line += length;
        line += *line == '\n';
    }
    if (!found) {
        strcat(result, key);
        strcat(result, "=");
        strcat(result, value);
        strcat(result, "\n");
    }
    bool ok = writeWhole(path, result);
    free(result);
    free(current);
    return ok;
}
static bool setSetting(const char *key, const char *value) { return setSettingAt(SETTINGS, key, value); }
// The client draws its interface at client.gui.scale.ui_zoom2.  Its own UI Scaling slider stops at 200%, but the stored
// value is honoured a little above it, and the loader uses that for a slightly larger HUD (loader-59): the design viewport
// the client works in is its window / zoom, and the HUD is laid out in that space.  Everything below follows the zoom the
// client reports in its own log (linux_sdl.c's client_ui_scale, read from "uiViewportScale:"), so the loader's marker and
// battle hints stay on it.
#define UI_ZOOM 2.1
static unsigned interface_scale = 100;  // the interface size in percent (loader-45); defined here for applyUiScale below
static double ui_zoom = UI_ZOOM;  // the zoom the client is actually asked for (loader-76: a lower output can carry the size itself)
static void interfaceViewport(int surface_width, int surface_height, double *vw, double *vh) {
    double scale = (double)linuxSdlRenderScale();
    *vw = (double)surface_width / (ui_zoom * scale);
    *vh = (double)surface_height / (ui_zoom * scale);
}
static void applyUiScale(int surface_width, int surface_height) {
    if (surface_width < 1 || surface_height < 1) return;
    double requested = (double)interface_scale / 100.0;
    double zoom, scale;
    if (surface_height < 2000) {
        // loader-76: at a lower output (1440p) the client's own zoom can carry the interface size - its cap is 2.0, and
        // 2.0 * 1440/2160 * 1.5 = 2.0 exactly. The window is then the whole surface (render scale 1), so there is no
        // upscale at all and the frame renders a third fewer pixels; the apparent size matches 4K at the same setting.
        zoom = 2.0 * ((double)surface_height / 2160.0) * requested;
        if (zoom > 2.0) zoom = 2.0;
        scale = 1.0;
    } else {
        // 4K: the client clamps its zoom at 2.0, so the smaller window plus the upscale is what enlarges the interface.
        zoom = UI_ZOOM;
        scale = requested;
    }
    ui_zoom = zoom;
    linuxSdlSetRenderScale((float)scale);
    double vw, vh;
    interfaceViewport(surface_width, surface_height, &vw, &vh);
    char zoom_text[32], surface_w[16], surface_h[16];
    snprintf(zoom_text, sizeof(zoom_text), "%.4g", zoom);
    // loader-66: the same even rounding clientScreenSize gives the client's window (see linux_sdl.c).
    snprintf(surface_w, sizeof(surface_w), "%d", (int)((double)surface_width / scale + 0.5) & ~1);
    snprintf(surface_h, sizeof(surface_h), "%d", (int)((double)surface_height / scale + 0.5) & ~1);
    bool ok = setSetting("client.gui.scale.ui_zoom2", zoom_text) && setSetting("client.graphics.width", surface_w) &&
              setSetting("client.graphics.height", surface_h);
    say("settings: ui_zoom2=%s for %dx%d (interface %.0f%%, design viewport %.0fx%.0f)%s", zoom_text, surface_width, surface_height, scale * 100.0,
        vw, vh, ok ? "" : " (some values not written)");
}
// The login screen's "Remember Me" (client.ui.login.auto) does not stay set: the client rewrites main.properties itself
// when it saves its settings, and that rewrite leaves the key out, so the next start's login screen comes back unticked.
// The defaults set it too, but only once per DEFAULTS_VERSION, which is not enough for a key the client drops by itself;
// writing it at every start is what keeps the box ticked.
static void applyLoginDefaults(void) {
    bool ok = setSetting("client.ui.login.auto", "true");
    say("settings: remember me %s", ok ? "on" : "not written");
}
// The chat box height is what places the client's own battle window.  The client draws the battle scene as a fixed
// 758-unit-high window in its design units and puts the panel's bottom edge at viewport height - chatframe.height - 127,
// so the chat box's height is what places the whole battle scene.  A 1080-high viewport with a 34-unit chat box puts the
// scene at y 161..919, centred vertically.  The height is kept at the same fraction of the design viewport (which a render
// scale above 1 makes smaller), so the scene stays centred at every interface size.  The client is free to rewrite its own
// settings, so this is written at every start, like the login default.
// The hotbar's place is an anchor (client 34702 replaced the x/.y pair): the client stores anchors as "key:inset" pairs
// (its own chat box default is "left:0,bottom:0"), so "centre:0,bottom:0" keeps the bar centred on the design's bottom
// edge at every scale, which is where the pad's selection marker expects it. The colon is escaped the way the client
// escapes it in its own file.
// loader-86: the client limits its own frame rate to 60 as well as asking for vsync. The two pacers fight: the client
// sleeps until 16.67 ms has passed and only then presents, so a frame whose work ends just after that deadline misses
// the display's vblank and lands on the next one - a 33 ms frame. The interval log shows exactly that shape (frames at
// 16-17 ms or at 31-36 ms, nothing in between). The cap is raised out of the way so the client presents the moment its
// frame is ready and the vblank is the only pacer; vsync still holds the game at 60.
static void applyFrameCap(void) {
    bool ok = setSetting("client.graphics.max_fps", "120");
    say("settings: frame cap 120 (the display's vsync paces the game)%s", ok ? "" : " (not written)");
}
static void applyHudDefaults(int surface_height) {
    // loader-77: the stutter grows with the number of players on screen - each one is a set of model draws, and this
    // driver is draw-call bound. Cap how many the client draws; /app0/fewer-players turns it on for a comparison.
    char flag[8];
    if (readSmall("/app0/fewer-players", flag, sizeof(flag)) && setSetting("client.graphics.max_visible_players", "10"))
        say("settings: max visible players 10");
    double vh = (double)surface_height / (ui_zoom * (double)linuxSdlRenderScale());
    char chatframe[32];
    snprintf(chatframe, sizeof(chatframe), "%.0f", 34.0 * vh / 1080.0);
    bool ok = setSetting("client.gui.hud.chatframe.height", chatframe);
    bool anchored = setSetting("client.gui.hud.hotkeybar.anchor", "centre\\:0,bottom\\:0");
    say("settings: chat box height %s, hotbar anchored bottom centre%s (design viewport %.0f high)", ok ? chatframe : "not written",
        anchored ? "" : " FAILED", vh);
}
// The party hotbar is per character (the client keeps client.gui.hud.party_frame_anchor in <account id>.properties next to
// main.properties), so its anchor is written into every character file.  With no anchor the client puts the frame against
// the top of the right edge; "right:0,centre:0" centres it vertically on the right.
static void applyPartyFrameAnchor(void) {
    int error = 0;
    PlatformDirectory *directory = platformDirectoryOpen(player_config, &error);
    if (!directory) {
        say("settings: party frame anchor: could not list %s (errno=%d)", player_config, error);
        return;
    }
    char name[256], path[320];
    uint8_t type;
    uint64_t inode;
    unsigned changed = 0;
    while (platformDirectoryRead(directory, name, &type, &inode, &error) == 1) {
        size_t length = strlen(name);
        if (length < 12 || strcasecmp(name + length - 11, ".properties") || !strcasecmp(name, "main.properties") ||
            !strcasecmp(name, "savedcredentials.properties"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", player_config, name);
        if (setSettingAt(path, "client.gui.hud.party_frame_anchor", "right\\:0,centre\\:0")) ++changed;
    }
    platformDirectoryClose(directory);
    say("settings: party frame anchored right centre in %u character file%s", changed, changed == 1 ? "" : "s");
}
// loader-45: the interface size.  The client's own UI Scaling stops at 200%; a render scale above 1 gives the client a
// smaller window and stretches its picture back over the surface, so the interface (and the world) is drawn larger.  The
// client rewrites main.properties itself, so the choice lives in the title's own small file.  loader-47: the size is a
// percentage, 100..150 in 5% steps (a slight enlargement; a large one was laggy because the whole frame is copied and
// scaled every present), and the old whole-number values 1..5 are mapped onto it.
#define INTERFACE_SCALE_MIN 100u
#define INTERFACE_SCALE_MAX 150u
#define INTERFACE_SCALE_STEP 5u
#define INTERFACE_SCALE ROOT "/interface-scale"

static unsigned clampInterfacePercent(unsigned value) {
    if (value < INTERFACE_SCALE_MIN) value = INTERFACE_SCALE_MIN;
    if (value > INTERFACE_SCALE_MAX) value = INTERFACE_SCALE_MAX;
    return (value + INTERFACE_SCALE_STEP / 2) / INTERFACE_SCALE_STEP * INTERFACE_SCALE_STEP;
}
static void loadInterfaceScale(void) {
    char text[16];
    unsigned percent = INTERFACE_SCALE_MIN;
    if (readSmall(INTERFACE_SCALE, text, sizeof(text))) {
        char *end = NULL;
        unsigned long parsed = strtoul(text, &end, 10);
        // A value of 20 or less is loader-46's whole-number render scale 1..5: map it onto the percent range.
        if (end != text) percent = parsed && parsed <= 20 ? (unsigned)(100 + (parsed - 1) * 10) : (unsigned)parsed;
    }
    interface_scale = clampInterfacePercent(percent);
    linuxSdlSetRenderScale((float)interface_scale / 100.0f);
    say("interface size: %u%%", interface_scale);
    // loader-65: a diagnostic switch for the two upscale paths: /app0/upscale-copy forces the old window copy (no texture),
    // so the same interface size can be rendered both ways and compared.
    char flag[8];
    bool copy_path = readSmall("/app0/upscale-copy", flag, sizeof(flag));
    linuxSdlSetUpscaleCopy(copy_path);
    if (copy_path) say("upscale: copy path forced by /app0/upscale-copy");
}
static void saveInterfaceScale(unsigned percent) {
    interface_scale = clampInterfacePercent(percent);
    linuxSdlSetRenderScale((float)interface_scale / 100.0f);
    char text[8];
    snprintf(text, sizeof(text), "%u", interface_scale);
    if (!writeSmall(INTERFACE_SCALE, text)) say("interface size: could not save %u%%", interface_scale);
    say("interface size: %u%%", interface_scale);
}

// ---- the client run -----------------------------------------------------------------------------------------------------------------
static _Atomic bool game_finished, release_screen, screen_released;
static _Atomic bool game_ready, start_requested;  // the Start Game / Settings / Exit menu waits for a choice
static _Atomic uint64_t client_started_ns;
// The SDL layer calls this on the game's thread just before it makes its window surface (ps5-opengl has one): the loading screen
// stays up through the client's start and gives the display away only then.
static void acquireDisplay(void) {
    if (atomic_load(&screen_released)) return;
    atomic_store(&release_screen, true);
    while (!atomic_load(&screen_released)) sceKernelUsleep(5000);
}
static void drawLoadingInGame(void);
static void *gameThread(void *argument) {
    (void)argument;
    atomic_store(&game_ready, false);
    // This project's SDL3, EGL/GLX, OpenAL and GTK (the file chooser), adapted from PokeMMO-NX.
    static LinuxVirtualLibrary virtual_libraries[8];
    virtual_libraries[0] = linuxSdlLibrary;
    virtual_libraries[1] = linuxEglLibrary;
    virtual_libraries[2] = linuxOpenAlLibrary;
    virtual_libraries[3] = linuxGlxLibrary;
    for (unsigned i = 0; i < 4; ++i) virtual_libraries[4 + i] = linuxGtkLibraries[i];
    linuxAudioOutAttach();
    linuxSdlSetDisplayAcquire(acquireDisplay);
    linuxSdlSetLoadingOverlay(drawLoadingInGame);
    atomic_store(&client_started_ns, platformMonotonicNs());
    mark("client.map", RUNNING);
    // loader-82: the client ran with a 640m heap and a 128m young generation - the smallest that a Switch-class port
    // could get away with - and on this hardware that means the collector running constantly and, when it falls behind,
    // pausing the whole client for 40-140 ms: the frame loss the player could see. The console has memory to spare, so the
    // heap is three times larger and the young generation is big enough to absorb a zone load.
    // The bundled runtime rejects pause-goal flags and -Xlog (loader-82's first build would not start: "Could not find
    // option 'MaxGCPauseMillis'"), so only the two sizes it already accepted are passed.
    static const char *const options[] = {"-XX:MaxHeapSize=2048m", "-XX:MaxNewSize=512m", NULL};
    char client_path[400];
    snprintf(client_path, sizeof(client_path), "%s/bin/linux/x64/PokeMMO", active_dir);
    say("client: starting slot %c, revision %s", slot_state.active, active_revision);
    // loader-80's shader-cache experiment is gone: ps5-opengl's cache faults with signal 11 on the client's first compile.
    GameConfig config = {.root = ROOT,
                         .client_path = client_path,
                         // The game sees its folder as /game: the active slot, with the shared settings over /game/config. The C++
                         // runtime the client's native libraries need (libstdc++, libgcc_s) ships with the title.
                         .mounts = {{"/game", NULL}, {"/game/config", player_config}, {"/game/roms", rom_folder}, {"/lib", "/app0/assets/lib"}},
                         .mount_count = 4,
                         .arguments = options,
                         .timeout_seconds = 0,
                         .virtual_libraries = virtual_libraries,
                         .virtual_count = 8};
    config.mounts[0].native = active_dir;
    mkdir(ROOT, 0755);
    linuxSdlSetFirstPicture(gameShowedPicture);
    bool ok = gameRun(&config);
    if (!ok && revertTrial(gameFailure())) {
        atomic_store(&start_advice, "It may need a newer loader. Close the title and start it again to use the previous revision.");
        atomic_store(&start_problem, "This PokeMMO update could not start");
    }
    say("client: %s%s", ok ? "ended normally" : "ended: ", ok ? "" : gameFailure());
    if (ok) {  // loader-21: the player chose Exit in the game: the title closes and the console goes back to the home screen
        backupSettings();
        say("client: exited from the game: closing the title");
        platformQuit();
    }
    mark("client.map", strstr(gameFailure(), "cannot be read") ? FAIL : PASS);
    mark("client.start", strstr(gameFailure(), "cannot be read") ? NOT_RUN : PASS);
    mark("client.end", ok ? PASS : INFO);
    atomic_store(&game_finished, true);
    return NULL;
}

// ---- screen ---------------------------------------------------------------------------------------------------------------------
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext screen_context = EGL_NO_CONTEXT;
static EGLint width, height;
static bool screenOpen(void) {
    static const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
                                               EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0, major = 0, minor = 0;
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return false;
    // ps5-opengl can present natively at 2160p and 1440p when built with PS5_DYNAMIC_SCANOUT. The mode may only change
    // before the first eglInitialize; ask for the largest one the runtime accepts (the console scales the output to the
    // TV) and fall back to the title's 1080p if it cannot be set up. applyUiScale then sizes the client for the surface.
    typedef EGLBoolean (*SetDisplayModePS5)(EGLDisplay, EGLint, EGLint);
    SetDisplayModePS5 set_mode = (SetDisplayModePS5)eglGetProcAddress("eglSetDisplayModePS5");
    static const EGLint modes[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}};
    // loader-74: a 1440p output is the cheapest way to give the frame budget back: the client and the upscale both
    // shrink, and the console/TV scales 1440p to the panel. /app0/output-1440 selects it before the display comes up.
    // loader-85: /app0/client-1080 goes one step further, to test whether the frame time is fill (the same interface
    // size, forty percent fewer pixels for the client to draw) before anything is decided from it.
    EGLint preferred = 0;
    {
        char flag[8];
        if (readSmall("/app0/client-1080", flag, sizeof(flag)))
            preferred = 2;
        else if (readSmall("/app0/output-1440", flag, sizeof(flag)))
            preferred = 1;
    }
    bool native_mode = false;
    if (set_mode) {
        for (size_t turn = 0; turn < sizeof(modes) / sizeof(modes[0]) && !native_mode; ++turn) {
            size_t i = ((size_t)preferred + turn) % (sizeof(modes) / sizeof(modes[0]));
            native_mode = set_mode(display, modes[i][0], modes[i][1]);
            if (native_mode) say("screen: native display mode %dx%d", (int)modes[i][0], (int)modes[i][1]);
        }
    }
    if (!eglInitialize(display, &major, &minor)) {
        if (!native_mode) return false;
        eglTerminate(display);  // the larger buffer did not come up: back to the title's default before giving up
        if (set_mode) set_mode(display, 1920, 1080);
        if (!eglInitialize(display, &major, &minor)) return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1) return false;
    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
    screen_context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    if (surface == EGL_NO_SURFACE || screen_context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, screen_context)) return false;
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    return true;
}
static int stepState(const char *name) {
    for (size_t i = 0; i < STEP_COUNT; ++i)
        if (!strcmp(steps[i].name, name)) return atomic_load(&steps[i].state);
    return NOT_RUN;
}
// Without the overlay (no OpenGL entry points): the coloured tiles of the earlier loaders, drawn with scissored clears.
static void drawTilesPlain(unsigned frame) {
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, width, height);
    glClearColor(0.03f, 0.07f, 0.16f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    const int columns = 5, size = width / 12, gap = size / 6;
    const int left = (width - columns * size - (columns - 1) * gap) / 2;
    const int rows = (int)((STEP_COUNT + columns - 1) / columns);
    const int top = (height + rows * size + (rows - 1) * gap) / 2;
    for (size_t i = 0; i < STEP_COUNT; ++i) {
        int column = (int)(i % columns), row = (int)(i / columns);
        glScissor(left + column * (size + gap), top - (row + 1) * size - row * gap, size, size);
        switch (atomic_load(&steps[i].state)) {
            case PASS: glClearColor(0.15f, 0.75f, 0.30f, 1); break;
            case FAIL: glClearColor(0.85f, 0.20f, 0.20f, 1); break;
            case INFO: glClearColor(0.25f, 0.45f, 0.85f, 1); break;
            case RUNNING: glClearColor((frame / 20) % 2 ? 0.95f : 0.75f, (frame / 20) % 2 ? 0.80f : 0.60f, 0.10f, 1); break;
            default: glClearColor(0.35f, 0.35f, 0.38f, 1); break;
        }
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}
// What the loading screen says, from the steps and the install progress.
static void loadingView(LoadingView *view, LoadingStep *step_view, char *detail, size_t detail_size, unsigned frame) {
    static char version[64], loader_revision[32];
    snprintf(version, sizeof(version), "v%s", POKEMMO_RELEASE);  // PokeMMO's own version, shown in Settings
    snprintf(loader_revision, sizeof(loader_revision), "v%s", POKEMMO_REVISION);  // ours: bottom centre
    for (size_t i = 0; i < STEP_COUNT; ++i) step_view[i] = (LoadingStep){steps[i].name, atomic_load(&steps[i].state)};
    *view = (LoadingView){.status = "Getting ready", .detail = detail, .version = version, .build = POKEMMO_VERSION,
                          .revision = client_revision, .loader_revision = loader_revision, .log_path = platformLogPath(),
                          .steps = step_view, .step_count = STEP_COUNT, .frame = frame};
    detail[0] = 0;
    view->no_input = atomic_load(&client_started_ns) != 0;
    view->notice = atomic_load(&release_notice_set) ? release_notice : NULL;  // from the client's start on, nothing reads the controller here
    static const char *const checks[] = {"fs.list.app0", "fs.list.roms"};
    unsigned checked = 0;
    for (unsigned i = 0; i < 2; ++i) checked += stepState(checks[i]) != NOT_RUN && stepState(checks[i]) != RUNNING;
    view->fraction = 0.08f * (float)checked / 2.0f;
    if (checked < 2) view->status = "Checking files";
    int update = stepState("client.update"), phase = atomic_load(&update_phase);
    if (update == RUNNING) {
        view->fraction = 0.08f;
        if (phase == UPDATE_CHECKING)
            view->status = "Checking for updates";
        else if (phase == UPDATE_PROMPT) {
            static char choices[96];
            uint64_t now = platformMonotonicNs(), deadline = atomic_load(&prompt_deadline_ns);
            unsigned left = deadline > now ? (unsigned)((deadline - now) / 1000000000ull) + 1 : 0;
            snprintf(choices, sizeof(choices), "\x01 Download     \x02 Skip          (downloading in %u)", left);
            view->question = update_question;
            view->choices = choices;
        } else if (phase == UPDATE_DOWNLOADING) {
            uint64_t done = atomic_load(&update_progress.done), total = atomic_load(&update_progress.total);
            view->status = "Downloading PokeMMO";
            if (total) {
                view->fraction = 0.08f + 0.52f * (float)((double)done / (double)total);
                snprintf(detail, detail_size, "%llu of %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
            }
        } else if (phase == UPDATE_APPLYING) {
            view->fraction = 0.6f;
            view->status = "Installing the update";
        }
    } else if (update != NOT_RUN && update != FAIL) {
        uint64_t started = atomic_load(&client_started_ns);
        float seconds = started ? (float)((double)(platformMonotonicNs() - started) / 1e9) : 0.0f;
        view->fraction = 0.6f + 0.38f * (1.0f - expf(-seconds / 20.0f));  // the client's start has no progress to report: an easing guess
        view->status = "Starting PokeMMO";
        if (atomic_load(&update_warning_set)) view->warning = update_warning;
    }
    // loader-49: the loader image's own update takes the bar over while it downloads (the client is already ready, so
    // nothing else is using it). The prompt is shown below, once the ready menu would otherwise be up.
    int image = atomic_load(&image_phase);
    bool image_busy = image == IMAGE_DOWNLOADING || image == IMAGE_APPLYING;
    if (image_busy) {
        uint64_t done = atomic_load(&image_progress.done), total = atomic_load(&image_progress.total);
        view->fraction = total ? (float)((double)done / (double)total) : 0.0f;
        view->status = image == IMAGE_APPLYING ? "Installing the loader update" : "Downloading the loader update";
        if (total) snprintf(detail, detail_size, "%llu of %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
    }
    const char *problem = atomic_load(&start_problem);
    if (stepState("fs.list.app0") == FAIL) {
        view->problem = "The title's files cannot be read";
        view->advice = "Install the title again.";
    } else if (problem) {
        view->problem = problem;
        view->advice = atomic_load(&start_advice);
    } else if (stepState("fs.list.roms") == FAIL) {
        view->problem = "The ROM folder cannot be read";
        view->advice = "Put your .nds/.gba game files in /data/PokeMMO-roms (FTP works), then start the title again.";
    } else if (atomic_load(&game_finished) || atomic_load(&fatal_signals)) {
        view->problem = "PokeMMO stopped while starting";
        static char advice[360];
        snprintf(advice, sizeof(advice), "Close the title with the PS button and start it again. Settings > Log has this start's log (%s).", platformLogPath());
        view->advice = advice;
    }
    // The Start Game / Settings / Exit menu: only when the client is there and nothing is loading, asking or failing.
    view->ready = atomic_load(&game_ready) && !atomic_load(&start_requested) && !view->no_input && !view->problem && !view->question;
    // loader-49: with the client ready, a newer loader image is offered here, in the same question style as the client's
    // own update. The menu is suppressed while the question is up (view->ready clears it), so the prompt's Cross/Circle
    // cannot also choose Start Game. The download and install keep the menu hidden until they are done.
    if (image == IMAGE_PROMPT && !atomic_load(&image_asked) && view->ready) {
        view->question = image_question;
        view->choices = image_choices;
        view->status = NULL;
        view->ready = false;
    }
    if (image_busy) view->ready = false;
    if (view->ready) view->fraction = 1.0f;  // installed and started: without its label the bar must read as complete
}
// ---- the log on screen (Settings > Log) ----------------------------------------------------------------------------------------------
#define LOG_READ_LIMIT (64u * 1024u)
#define LOG_MAX_LINES 4096
#define LOG_VISIBLE_LINES 24
static char log_buffer[LOG_READ_LIMIT + 1];
static const char *log_ring[LOG_MAX_LINES];
static const char *log_lines[LOG_MAX_LINES];
static unsigned log_line_count;
static bool log_missing;
static int log_source;  // 0 this start, 1 the previous start
static unsigned log_scroll;
static uint64_t log_read_ns;
// Three sources: this start, the start before it, and ps5-opengl's own log (Mesa's texture/shader/memory warnings).
enum { LOG_THIS_START = 0, LOG_PREVIOUS_START = 1, LOG_DRIVER = 2 };
#define LOG_SOURCE_COUNT 3
static const char *logFilePath(void) {
    const char *current = platformLogPath();
    if (log_source == LOG_DRIVER) return "/download0/ps5-opengl.log";
    if (log_source == LOG_THIS_START) return current;
    static char previous[192];
    size_t length = strlen(current);
    if (length > 4 && !strcmp(current + length - 4, ".log"))
        snprintf(previous, sizeof(previous), "%.*s-previous.log", (int)(length - 4), current);
    else
        snprintf(previous, sizeof(previous), "%s-previous", current);
    return previous;
}
static const char *logTitle(void) {
    return log_source == LOG_DRIVER ? "ps5-opengl driver log" : log_source == LOG_PREVIOUS_START ? "Previous start's log" : "This start's log";
}
static void readLogFile(void) {
    const char *path = logFilePath();
    log_line_count = 0;
    log_missing = false;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_missing = true;
        return;
    }
    struct stat info;
    if (fstat(fd, &info)) {
        close(fd);
        log_missing = true;
        return;
    }
    off_t length = info.st_size;
    if (length <= 0) {
        close(fd);
        return;
    }
    off_t start = length > (off_t)LOG_READ_LIMIT ? length - (off_t)LOG_READ_LIMIT : 0;
    if (start && lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        log_missing = true;
        return;
    }
    ssize_t got = read(fd, log_buffer, LOG_READ_LIMIT);
    close(fd);
    if (got <= 0) return;
    log_buffer[got] = 0;
    char *text = log_buffer;
    if (start) {  // the first line of the tail is probably partial: start at the next newline
        char *newline = strchr(text, '\n');
        if (!newline) return;
        text = newline + 1;
    }
    unsigned total = 0, oldest = 0;
    for (char *line = text; *line;) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = 0;
        size_t line_length = strlen(line);
        if (line_length && line[line_length - 1] == '\r') line[line_length - 1] = 0;
        if (total < LOG_MAX_LINES)
            log_ring[total] = line;
        else {
            log_ring[oldest] = line;
            oldest = (oldest + 1) % LOG_MAX_LINES;
        }
        ++total;
        if (!newline) break;
        line = newline + 1;
    }
    if (total <= LOG_MAX_LINES) {
        log_line_count = total;
        for (unsigned i = 0; i < total; ++i) log_lines[i] = log_ring[i];
    } else {
        log_line_count = LOG_MAX_LINES;
        for (unsigned i = 0; i < LOG_MAX_LINES; ++i) log_lines[i] = log_ring[(oldest + i) % LOG_MAX_LINES];
    }
}
static void refreshLog(bool force) {
    uint64_t now = platformMonotonicNs();
    if (!force && now - log_read_ns < 1000000000ull) return;
    readLogFile();
    log_read_ns = now;
    unsigned max_scroll = log_line_count > LOG_VISIBLE_LINES ? log_line_count - LOG_VISIBLE_LINES : 0;
    if (log_scroll > max_scroll) log_scroll = max_scroll;
}

// The same screen over the game's first (black) frames, on the game's thread and context.
static void drawLoadingInGame(void) {
    static unsigned frame;
    LoadingView view;
    LoadingStep step_view[STEP_COUNT];
    char detail[96];
    loadingView(&view, step_view, detail, sizeof(detail), ++frame);
    loadingScreenDraw(&view);
}
static bool settings_open;
static int settings_tab;
static int menu_choice;
static char update_status[180] = "";
static atomic_bool update_running;
// "Check for updates" in Settings: asks GitHub for the newest release.  It cannot install anything (a title cannot replace the
// image it runs from); it only says whether a newer release exists.  The staging updater on the PC does the rest.
static void *updateCheckMain(void *argument) {
    (void)argument;
    char newest[64];
    if (!releaseNewest(RELEASES_URL, newest, sizeof(newest)))
        snprintf(update_status, sizeof(update_status), "Could not reach GitHub. Check the network and try again.");
    else if (releaseCompare(newest, "v" POKEMMO_RELEASE) > 0)
        snprintf(update_status, sizeof(update_status), "Newer release available: %s (installed v%s).", newest, POKEMMO_RELEASE);
    else
        snprintf(update_status, sizeof(update_status), "Up to date (v%s).", POKEMMO_RELEASE);
    atomic_store(&update_running, false);
    return NULL;
}
static void updateCheckStart(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&update_running, &expected, true)) return;
    snprintf(update_status, sizeof(update_status), "Checking GitHub ...");
    pthread_t thread;
    if (pthread_create(&thread, NULL, updateCheckMain, NULL)) {
        atomic_store(&update_running, false);
        snprintf(update_status, sizeof(update_status), "Could not start the check.");
    } else
        pthread_detach(thread);
}

// loader-49: ask the private release for a newer image. The result is cached for the whole start: the prompt only shows
// once the client is ready, and an unreachable or unconfigured source stays silent (it must not disturb a start that
// does not use it). The token is never logged.
static void *imageCheckMain(void *argument) {
    (void)argument;
    char error[256] = "";
    int checked = pokeupdateCheck(&image_config, title_writable, "v" POKEMMO_REVISION, &image_remote, error, sizeof(error));
    if (checked <= 0) {
        say("image update: %s", checked < 0 ? error : "up to date");
        atomic_store(&image_phase, IMAGE_IDLE);
        return NULL;
    }
    const char *revision = image_remote.tag[0] == 'v' || image_remote.tag[0] == 'V' ? image_remote.tag + 1 : image_remote.tag;
    unsigned long long megabytes = (unsigned long long)((image_remote.size + (1u << 20) - 1) >> 20);
    snprintf(image_question, sizeof(image_question), "An update is available: Rev %s (%llu MB). Would you like to update?", revision, megabytes);
    atomic_store(&image_available, true);
    atomic_store(&image_phase, IMAGE_PROMPT);
    say("image update: %s is available (%s, %llu MB), asking the player", image_remote.tag, image_remote.asset, megabytes);
    return NULL;
}
// loader-49: download and install on its own thread, so the ready menu keeps drawing and the bar can move. The update is
// installed for the next start; the player is told to restart, and the prompt is not shown again this session.
// loader-51 (image install): once the image is staged the loader also asks ShadowMountPlus (its loopback API) for a full
// scan with reset_attempts, so the stale mount is dropped and the new image is taken up after this title closes - "close
// and start again" is then enough, with no console or ShadowMount+ restart. When the API cannot be reached the wording
// falls back to opening ShadowMount+ once.
// loader-52 (folder install): the executable was replaced in place, so nothing is remounted or rescanned and the wording
// is simply "close and start again".
static void *imageInstallMain(void *argument) {
    (void)argument;
    char error[256] = "";
    if (!pokeupdateInstall(&image_config, title_writable, &image_remote, &image_progress, error, sizeof(error))) {
        say("image update: installed %s (%s)", image_remote.tag, image_remote.asset);
        if (title_writable) {
            // loader-52: a folder install. The executable was replaced in place, so there is nothing to remount, rescan
            // or restart - the next launch simply runs the new build.
            snprintf(update_warning, sizeof(update_warning), "Update installed. Close PokeMMO, then start it again to use it.");
        } else {
            char note[96] = "";
            if (!pokeupdateNudgeShadowMount(note, sizeof(note)))
                snprintf(update_warning, sizeof(update_warning), "Update installed. Close PokeMMO completely, then start it again - ShadowMount+ will switch to it automatically.");
            else
                snprintf(update_warning, sizeof(update_warning), "Update installed. Close PokeMMO, open ShadowMount+ once, then start the game again (%s).", note);
        }
        atomic_store(&update_warning_set, true);
        atomic_store(&image_available, false);
    } else {
        say("image update: failed: %s", error);
        // loader-58: the reason is technical (an asset of the wrong kind, a short download, a full disk) and the player can
        // do nothing with it, but "The update failed: <jargon>" leaves them unsure whether the game is even usable. Say
        // what matters - the installed build is untouched and still starts - and point at the log for the detail.
        snprintf(update_warning, sizeof(update_warning), "Update not installed. PokeMMO will start your current version (details in Settings > Log).");
        atomic_store(&update_warning_set, true);
    }
    atomic_store(&image_phase, IMAGE_IDLE);
    return NULL;
}
static void imageInstallStart(void) {
    atomic_store(&image_asked, true);  // Yes or No: do not ask again until the next start
    atomic_store(&image_phase, IMAGE_DOWNLOADING);
    pthread_t thread;
    if (pthread_create(&thread, NULL, imageInstallMain, NULL))
        atomic_store(&image_phase, IMAGE_IDLE);
    else
        pthread_detach(thread);
}
static void drawScreen(unsigned frame) {
    if (!overlayBegin((unsigned)width, (unsigned)height)) {
        drawTilesPlain(frame);
        return;
    }
    LoadingView view;
    LoadingStep step_view[STEP_COUNT];
    char detail[96];
    loadingView(&view, step_view, detail, sizeof(detail), frame);
    PlatformPad pad;
    bool read = platformPadRead(&pad);
    static uint32_t before = ~0u;  // what is held when a screen appears does not answer it
    uint32_t pressed = read ? pad.buttons & ~before : 0;
    before = read ? pad.buttons : 0;
    if (settings_open) {
        if (pressed & (PLATFORM_PAD_CIRCLE | PLATFORM_PAD_TRIANGLE)) {
            settings_open = false;
        } else {
            // A vertical list: up/down choose the entry, the content is on the right.
            if (pressed & PLATFORM_PAD_UP) settings_tab = (settings_tab + SETTINGS_ITEMS - 1) % SETTINGS_ITEMS;
            if (pressed & PLATFORM_PAD_DOWN) settings_tab = (settings_tab + 1) % SETTINGS_ITEMS;
            if (settings_tab == SETTINGS_LOG) {
                refreshLog(false);
                if (pressed & PLATFORM_PAD_L1) log_scroll += LOG_VISIBLE_LINES;
                if (pressed & PLATFORM_PAD_R1) log_scroll = log_scroll > LOG_VISIBLE_LINES ? log_scroll - LOG_VISIBLE_LINES : 0;
                if (pressed & PLATFORM_PAD_SQUARE) {  // this start, the start before, ps5-opengl's log
                    log_source = (log_source + 1) % LOG_SOURCE_COUNT;
                    log_scroll = 0;
                    refreshLog(true);
                }
                unsigned max_scroll = log_line_count > LOG_VISIBLE_LINES ? log_line_count - LOG_VISIBLE_LINES : 0;
                if (log_scroll > max_scroll) log_scroll = max_scroll;
            } else if (settings_tab == SETTINGS_UPDATES && (pressed & PLATFORM_PAD_CROSS)) {
                updateCheckStart();
                // loader-49: the GitHub release check above is about the published loader; when a private image update
                // was found, Cross also offers it again here (the player may have dismissed it at start, or an attempt
                // may have failed). The question itself is on the loading screen, so Settings closes.
                if (atomic_load(&image_available)) {
                    atomic_store(&image_asked, false);
                    atomic_store(&image_phase, IMAGE_PROMPT);
                    settings_open = false;
                    say("image update: offered again from Settings");
                }
            } else if (settings_tab == SETTINGS_INTERFACE) {
                if ((pressed & PLATFORM_PAD_LEFT) && interface_scale > INTERFACE_SCALE_MIN)
                    saveInterfaceScale(interface_scale - INTERFACE_SCALE_STEP);
                if ((pressed & PLATFORM_PAD_RIGHT) && interface_scale < INTERFACE_SCALE_MAX)
                    saveInterfaceScale(interface_scale + INTERFACE_SCALE_STEP);
            }
        }
    } else {
        if (pressed & PLATFORM_PAD_TRIANGLE) {  // Settings is always one press away, also while starting or on a failure
            settings_open = true;
            settings_tab = SETTINGS_DETAILS;
            log_scroll = 0;
            say("settings: opened");
        } else if (view.ready) {
            // loader-46: the interface size is reachable without opening Settings: L1/R1 are otherwise unused on this
            // menu, and the line under it says so.  The choice is saved now and applied when Start Game runs.
            if (pressed & PLATFORM_PAD_L1) {
                if (interface_scale > INTERFACE_SCALE_MIN) saveInterfaceScale(interface_scale - INTERFACE_SCALE_STEP);
            }
            if (pressed & PLATFORM_PAD_R1) {
                if (interface_scale < INTERFACE_SCALE_MAX) saveInterfaceScale(interface_scale + INTERFACE_SCALE_STEP);
            }
            if (pressed & (PLATFORM_PAD_LEFT | PLATFORM_PAD_UP)) menu_choice = (menu_choice + 2) % 3;
            if (pressed & (PLATFORM_PAD_RIGHT | PLATFORM_PAD_DOWN)) menu_choice = (menu_choice + 1) % 3;
            if (pressed & PLATFORM_PAD_CROSS) {
                if (menu_choice == 0) {
                    atomic_store(&start_requested, true);
                    say("menu: Start Game");
                } else if (menu_choice == 1) {
                    settings_open = true;
                    settings_tab = SETTINGS_DETAILS;
                    log_scroll = 0;
                    say("menu: Settings");
                } else {
                    say("menu: Exit");
                    backupSettings();
                    platformQuit();
                }
            }
        }
        if (!settings_open && atomic_load(&update_phase) == UPDATE_PROMPT) {
            if (pressed & PLATFORM_PAD_CROSS) atomic_store(&update_choice, CHOICE_DOWNLOAD);
            if (pressed & PLATFORM_PAD_CIRCLE) atomic_store(&update_choice, CHOICE_SKIP);
        }
        // loader-49: the loader image's own Yes/No, only while its question is actually on screen (view.question is set
        // by loadingView to image_question then). Cross installs, Circle dismisses for this start. Because the question
        // clears the ready menu, these cannot also drive Start Game / Settings / Exit.
        if (!settings_open && view.question == image_question && atomic_load(&image_phase) == IMAGE_PROMPT) {
            if (pressed & PLATFORM_PAD_CROSS) imageInstallStart();
            if (pressed & PLATFORM_PAD_CIRCLE) {
                atomic_store(&image_asked, true);
                atomic_store(&image_phase, IMAGE_IDLE);
                say("image update: skipped by the player");
            }
        }
    }
    view.settings = settings_open;
    view.settings_tab = settings_tab;
    view.interface_scale = interface_scale;
    view.update_status = update_status;
    view.menu = menu_choice;
    LogView log = {.title = logTitle(),
                   .path = logFilePath(),
                   .lines = log_lines,
                   .line_count = log_line_count,
                   .scroll = log_scroll,
                   .missing = log_missing};
    view.log = settings_open && settings_tab == SETTINGS_LOG ? &log : NULL;
    loadingScreenDraw(&view);
    overlayEnd();
}

// loader-39: a newer loader on GitHub: a banner on the loading screen (installing it stays the player's).
// loader-58: an "Updated to Rev X" confirmation from workMain wins over this banner: the player just updated, so telling
// them the next one exists as well would be noise.
static void *releaseCheckMain(void *argument) {
    (void)argument;
    char newest[64];
    if (!releaseNewest(RELEASES_URL, newest, sizeof(newest))) return NULL;
    bool newer = releaseCompare(newest, "v" POKEMMO_REVISION) > 0;  // loader-74: the tag is the loader revision, not the client release
    say("release check: newest published %s, this is v%s%s", newest, POKEMMO_REVISION, newer ? ": showing the update banner" : "");
    if (newer && !atomic_load(&release_notice_set)) {
        snprintf(release_notice, sizeof(release_notice), "A newer loader is out: %s", newest);
        atomic_store(&release_notice_set, true);
    }
    return NULL;
}

// The checks run behind the screen; the menu waits until the client is there, then Start Game runs it.
static void *workMain(void *argument) {
    (void)argument;
    listFolder("fs.list.app0", "/app0", NULL, 0);
    listFolder("fs.list.roms", rom_folder, NULL, 0);
    prepareSlots();
    // loader-58: if this is the first start of a build that is not the one last seen, say so plainly ("Updated to Rev
    // X"), whether the update came from the menu, an older loader or a manual deploy. Taken before the background checks
    // start, so their banners cannot replace it.
    char updated_notice[160];
    if (pokeupdateTakeUpdateNotice("v" POKEMMO_REVISION, updated_notice, sizeof(updated_notice))) {
        snprintf(release_notice, sizeof(release_notice), "%s", updated_notice);
        atomic_store(&release_notice_set, true);
        say("update: %s (this start runs it)", updated_notice);
    }
    pthread_t release_check;  // loader-39: in the background; the screen shows the banner whenever it has an answer
    if (!pthread_create(&release_check, NULL, releaseCheckMain, NULL)) pthread_detach(release_check);
    // loader-49: the loader image's own update check, also in the background. Without /data/pokeupdate.ini nothing is
    // sent and nothing is shown: the feature is entirely opt-in through that file on the console.
    pthread_t image_check;
    if (!pokeupdateLoadConfig(&image_config)) {
        say("image update: no source configured (%s)", POKEUPDATE_CONFIG_PATH);
    } else if (!pthread_create(&image_check, NULL, imageCheckMain, NULL)) {
        pthread_detach(image_check);
    } else {
        say("image update: could not start the check");
    }
    updateClient(false);
    if (slot_notice[0] && !atomic_load(&update_warning_set)) {
        snprintf(update_warning, sizeof(update_warning), "%s", slot_notice);
        atomic_store(&update_warning_set, true);
    }
    if (slot_state.active && active_revision[0]) {
        applyDefaults();
        loadInterfaceScale();
        applyUiScale(width, height);
        applyLoginDefaults();
        applyFrameCap();
        applyHudDefaults(height);
        applyPartyFrameAnchor();
        atomic_store(&game_ready, true);
        say("ready: the Start Game / Settings / Exit menu is up");
        while (!atomic_load(&start_requested)) sceKernelUsleep(20000);
        say("ready: the player chose Start Game");
        applyUiScale(width, height);  // Settings may have changed the interface size while the menu was up
        applyHudDefaults(height);
        applyPartyFrameAnchor();
        pthread_t game;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 1u << 20);
        if (!pthread_create(&game, &attributes, gameThread, NULL)) pthread_detach(game);
        pthread_attr_destroy(&attributes);
    } else if (!atomic_load(&start_problem)) {
        atomic_store(&start_advice, "Check the console's internet connection, then close the title and start it again.");
        atomic_store(&start_problem, "PokeMMO is not installed yet");
    }
    return NULL;
}

int main(void) {
    say("PokeMMO v%s, %s (Rev v%s, build %s, %s) starting; UDP log port 18194", POKEMMO_RELEASE, LOADER_MILESTONE, POKEMMO_REVISION, POKEMMO_VERSION, POKEMMO_TITLE_ID);
    guardInstall();
    signal(SIGPIPE, SIG_IGN);  // a write to a closed socket or pipe must fail with EPIPE, not end the title
    // loader-78 tried the driver's on-disk shader cache here; it crashed the title with a signal 11 before the menu drew
    // (FATAL signal 11, address=0), so it is off again until it can be isolated.
    bool screen = screenOpen();
    say("screen: %s %dx%d", screen ? "ready" : "unavailable", width, height);
    chooseStorage();
    chooseRomFolder();  // the title's own game files, or the folder the player fills over FTP
    copyDriverLog();  // the driver log of the start before this one, readable from the title folder
    pthread_t driver_log_thread;
    if (!pthread_create(&driver_log_thread, NULL, driverLogCopyMain, NULL)) pthread_detach(driver_log_thread);
    choosePlayer();
    pthread_t worker;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1u << 20);
    pthread_create(&worker, &attributes, workMain, NULL);
    pthread_attr_destroy(&attributes);
    bool reported = false;
    for (unsigned frame = 1;; ++frame) {
        if (atomic_load(&release_screen) && !atomic_load(&screen_released)) {
            if (screen) {
                overlayContextLost();  // the game's context may get this one's address
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroyContext(display, screen_context);
                eglDestroySurface(display, surface);  // the display stays initialized: the client's SDL initializes it again
                screen = false;
                say("screen: handed to the client");
            }
            atomic_store(&screen_released, true);
        }
        if (screen) {
            drawScreen(frame);
            eglSwapBuffers(display, surface);
        }
        if (frame == 1 || frame % 3750 == 0) backupSettings();  // about once a minute
        if (atomic_load(&fatal_signals) && atomic_load(&steps[STEP_COUNT - 1].state) != FAIL) mark("client.end", FAIL);
        bool install_failed = atomic_load(&start_problem) != NULL;
        if ((atomic_load(&game_finished) || install_failed) && !reported) {
            reported = true;
            say("DONE. Close the title with the PS button. The full log is above and in %s.", platformLogPath());
        }
        sceKernelUsleep(16000);
    }
}
