// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// loader-49: updating the loader image itself, from a private GitHub release. The loader runs from a 265 MB .ffpfsc
// image that ShadowMountPlus mounts as the title, so until now replacing it meant staging the new image from a PC over
// FTP. This lets the console do it: a config file on the console names a private repository and a read-only token, the
// loader asks GitHub for its newest release, and the player can install it on the spot and restart.
//
// loader-52: the title can also be installed as a folder (the ShadowMountPlus-recommended layout), where /app0 is a
// writable nullfs of the source folder. Then the update is only the executable - the release's eboot.bin - and replacing
// it needs no image, no rescan and no restart at all. See POKEUPDATE_EBOOT_PATH and pokeupdateInstall.
//
// loader-56: that executable must be the built FSELF (the signed form), never the decrypted ELF the console's storage
// layer returns when it copies one out. The launcher only starts the FSELF form: with a bare ELF at /app0/eboot.bin every
// launch ends between ShadowMount+'s "launch mount ready" and the sandbox step, with no error anywhere.
//
// The source is OPTIONAL and INERT when unconfigured: without /data/pokeupdate.ini the loader makes no request and
// shows no prompt, exactly as it did before this feature. The token lives only in that file: it is never compiled in,
// never logged and never put in an error string.
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The config the player (or tools/ps5-publish.py) writes:
//   # Private loader update source. Delete this file to switch the check off.
//   repo=OWNER/REPO
//   asset=
//   token=github_pat_xxx
// Keys are case-insensitive, blank lines and lines starting with '#' are ignored. `asset` may be empty: the first
// release asset whose name ends in .ffpfsc (image install) or eboot.bin (folder install) is used then. A missing file,
// or one without repo and token, leaves the feature off. loader-55: `asset` is used only when it is already of the
// required kind, so a folder release's eboot.bin is never mistaken for an image (and vice versa).
#define POKEUPDATE_CONFIG_PATH "/data/pokeupdate.ini"
// Exactly one image may exist for the title (ShadowMountPlus mounts every image it finds), so the new one is written
// under a .new name and renamed over the old one only once it is complete and checked.
#define POKEUPDATE_IMAGE_PATH "/data/homebrew/PPSA98001.ffpfsc"
#define POKEUPDATE_IMAGE_NEW POKEUPDATE_IMAGE_PATH ".new"
// loader-52: a folder install. ShadowMountPlus mounts /data/homebrew/PPSA98001 as a nullfs over /app0, and that source
// folder is writable, so an update is just the executable: the release carries the built (FSELF) eboot.bin, it is
// written beside the old one and renamed over it. No .ffpfsc image, no staging marker, no ShadowMountPlus rescan and
// nothing to restart - the new build runs the next time the game is launched. loader-56: an FSELF is required; a
// decrypted ELF is refused, because the launcher cannot start one.
#define POKEUPDATE_EBOOT_PATH "/app0/eboot.bin"
#define POKEUPDATE_EBOOT_NEW POKEUPDATE_EBOOT_PATH ".new"
// loader-58: the revision of the last build that started, so the start after an update can say "Updated to Rev X". It is
// written next to the image marker, not in the title folder, so it survives the executable being replaced - and unlike a
// note written by the installer, it also covers an update staged any other way (a manual FTP deploy, an older loader).
#define POKEUPDATE_RUNNING_PATH "/data/homebrew/.pokemmo-running"
// The PC staging tool's marker (tools/ps5-autoupdate.py): the uppercase hex SHA-256 of the installed image, then a
// newline. The loader writes the same content so the two never disagree about what is installed.
#define POKEUPDATE_MARKER_PATH "/data/homebrew/.pokemmo-image.sha"
// ShadowMountPlus caches its mount of the image between launches and only rescans when this file *changes content*
// (see pokeupdate.c). It is rewritten after every successful install.
#define POKEUPDATE_SHADOW_LIST "/data/shadowmount/manual.lst"
// loader-51: ShadowMountPlus's loopback HTTP/JSON API. After a successful install the loader asks it to reset its mount
// retry counters and run a full scan, so the newly staged image is picked up without restarting ShadowMount+ or the
// console. While a game is running SMP queues the scan and runs it once the title has fully closed; when the API is
// unreachable the request is skipped and only the restart wording changes.
#define POKEUPDATE_SHADOW_API "http://127.0.0.1:10101/api/v1/scan"

typedef struct {
    char repo[200];   // "OWNER/REPO"
    char asset[200];  // optional exact asset name; "" takes the first .ffpfsc asset
    char token[520];  // the fine-grained read-only token; only ever read here
} PokeUpdateConfig;

typedef struct {
    char tag[64];         // the release's tag_name, e.g. "v1.1.33" (the loader revision)
    char asset[200];      // the chosen asset's name
    char api_url[520];    // the asset's API url (downloaded with Accept: application/octet-stream)
    uint64_t size;        // its size in bytes, 0 when GitHub did not say
    char digest[80];      // the published SHA-256, lowercase hex, when there is one
    bool has_digest;
} PokeUpdateRemote;

typedef struct {
    _Atomic uint64_t done, total;  // bytes, for the progress bar
} PokeUpdateProgress;

// Reads POKEUPDATE_CONFIG_PATH. False when the file is absent or names no repo or token: the caller then does nothing
// at all. The token is not logged here or anywhere else.
bool pokeupdateLoadConfig(PokeUpdateConfig *config);
// Asks the repository for its newest release and compares its tag against `current_revision` ("v1.1.32", releaseCompare
// rules). 1 when it is newer (fills `remote`), 0 when this loader is up to date, -1 on failure (reason in `error`).
// `folder_install` (the title runs from a writable folder) makes it look for an eboot.bin asset instead of a .ffpfsc.
int pokeupdateCheck(const PokeUpdateConfig *config, bool folder_install, const char *current_revision, PokeUpdateRemote *remote, char *error, size_t error_size);
// In an image install: downloads `remote`'s asset to POKEUPDATE_IMAGE_NEW, checks its size and (when published) its
// SHA-256, renames it over POKEUPDATE_IMAGE_PATH, writes the staging marker and asks ShadowMountPlus to rescan. 0 or -1
// (reason in `error`). When the marker already names this exact image (an earlier session staged it but the console kept
// the previous mount), the download is skipped and the image is re-applied in place.
// In a folder install (`folder_install`): downloads the eboot.bin asset, checks that it is a signed (FSELF) executable,
// and renames it over /app0/eboot.bin; the new build runs at the next launch, with no marker, no rescan and no restart.
int pokeupdateInstall(const PokeUpdateConfig *config, bool folder_install, const PokeUpdateRemote *remote, PokeUpdateProgress *progress, char *error, size_t error_size);
// loader-51: asks ShadowMountPlus (loopback API) to reset its mount retry counters and run a full scan, so a staged
// image is taken up without a console or ShadowMount+ restart. 0 when the request was accepted, -1 otherwise with a
// short reason in `note` (never the token). Best-effort: failure only changes the player-facing restart wording.
int pokeupdateNudgeShadowMount(char *note, size_t note_size);
// loader-54: a folder install. Any .ffpfsc left behind for this title (an earlier image install, or an interrupted
// .new) is a second candidate ShadowMountPlus keeps ignoring as a "duplicate" at every scan, so the loader deletes it
// once it is running from the folder - automatically, on every console and at every start. Idempotent: nothing to do
// (and nothing logged) when there is no image. Must not be called in an image install.
// loader-57: in a folder install it also removes /app0/eboot.bin.new, the half-written executable an interrupted (or
// refused) update leaves behind, so the folder only ever holds the executable that runs.
void pokeupdateRemoveLeftoverImage(void);
// loader-58: the confirmation for the first start of a new build. The revision this console last started is kept in
// POKEUPDATE_RUNNING_PATH; when it differs from `current_revision` ("v1.1.45", the build running now) this returns true and
// fills `notice` with "Updated to Rev 1.1.45". The record is updated either way, so the notice is shown exactly once. A
// first start ever (no record: a fresh install) says nothing, and neither does an unchanged revision.
bool pokeupdateTakeUpdateNotice(const char *current_revision, char *notice, size_t notice_size);
