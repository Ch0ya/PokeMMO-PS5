// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// The loader's own update (see pokeupdate.h). Four steps, all on the console:
//   1. read /data/pokeupdate.ini (a private repository and a read-only token; absent means the feature is off);
//   2. GET .../releases/latest and parse the small JSON by hand, the way release_check.c does (this tree deliberately
//      has no JSON library), to find the tag and the update asset's API url, size and digest;
//   3. stream the asset to a .new file in 64 KiB chunks while hashing it, then rename it over the installed one;
//   4. (image install only) write the staging marker and rewrite ShadowMountPlus's manual.lst, then (loader-51) ask its
//      loopback API for a full scan with reset_attempts, so it drops the stale mount and picks up the new image once this
//      title closes - no console or ShadowMount+ restart.
// A folder install (loader-52) is step 3 alone: /app0 is writable, the asset is the built (FSELF) eboot.bin, and renaming
// it over /app0/eboot.bin is the whole update - no image, no marker, no ShadowMount+ rescan and nothing to restart.
// loader-56: the asset must be the FSELF the build produces, not a decrypted ELF. Only the FSELF is launchable from
// /app0, and installing an ELF breaks every later start (see installEboot).
// GitHub's asset download needs the asset's API url (not browser_download_url): with Accept: application/octet-stream
// it is a private, authenticated download.
#include "pokeupdate.h"
#include "diagnostics.h"
#include "platform.h"
#include "release_check.h"
#include "sha256.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK_BODY_LIMIT (256u * 1024u)  // the releases/latest body is a few KiB; cap it so a bad response cannot run away
#define DOWNLOAD_CHUNK (64u * 1024u)

static void fail(char *error, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void fail(char *error, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
    diagnosticsTrace("pokeupdate: %s", error);
}

// ---- the config file ----------------------------------------------------------------------------------------------------------------
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' || text[length - 1] == '\n')) text[--length] = 0;
    return text;
}
bool pokeupdateLoadConfig(PokeUpdateConfig *config) {
    memset(config, 0, sizeof(*config));
    int fd = open(POKEUPDATE_CONFIG_PATH, O_RDONLY);
    if (fd < 0) return false;
    char text[4096];
    ssize_t got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got <= 0) return false;
    text[got] = 0;
    for (char *line = text; *line;) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = 0;
        char *entry = trim(line);
        char *equals = *entry && *entry != '#' ? strchr(entry, '=') : NULL;
        if (equals) {
            *equals = 0;
            char *key = trim(entry), *value = trim(equals + 1);
            if (!strcasecmp(key, "repo"))
                snprintf(config->repo, sizeof(config->repo), "%s", value);
            else if (!strcasecmp(key, "asset"))
                snprintf(config->asset, sizeof(config->asset), "%s", value);
            else if (!strcasecmp(key, "token"))
                snprintf(config->token, sizeof(config->token), "%s", value);
        }
        if (!newline) break;
        line = newline + 1;
    }
    return config->repo[0] != 0 && config->token[0] != 0;
}

// ---- a little JSON, by hand ---------------------------------------------------------------------------------------------------------
// The releases/latest body is small and its shape is fixed (release_check.c relies on the same): find a key, step over
// the colon and any spaces, and read the value. Strings are copied out unescaped, which is enough for tags, asset names
// and API urls.
static const char *jsonValue(const char *from, const char *key) {
    const char *found = strstr(from, key);
    if (!found) return NULL;
    found = strchr(found + strlen(key), ':');
    if (!found) return NULL;
    ++found;
    while (*found == ' ' || *found == '\t') ++found;
    return found;
}
static bool jsonString(const char *value, char *out, size_t size) {
    if (!value || *value != '"') return false;
    size_t length = strcspn(value + 1, "\"");
    if (!length || length >= size) return false;
    memcpy(out, value + 1, length);
    out[length] = 0;
    return true;
}
// The last "url" key before `end`: from an asset's "name" the asset's own "url" is the nearest one before it (GitHub
// orders the object url, id, node_id, name, label, uploader, ...), and every other url on the way is an *_url.
static const char *urlBefore(const char *from, const char *end) {
    const char *last = NULL;
    while (from < end) {
        const char *found = strstr(from, "\"url\"");
        if (!found || found >= end) break;
        last = found;
        from = found + 5;
    }
    return last;
}
// Finds an asset by exact name, or the first asset whose name ends in `suffix` (".ffpfsc" for an image install,
// "eboot.bin" for a folder install) when `wanted` is empty, each within the "assets" array. Returns the start of the
// name's value (inside the quotes), or NULL.
//
// loader-55: `wanted` is honoured only when it is itself of the required kind. A folder install publishes the raw ELF
// as "eboot.bin"; an image install must never accept that name, or the ELF would be written out as the .ffpfsc and
// ShadowMountPlus could not mount the result (the console then has a corrupt image and no title). When `wanted` is the
// wrong kind the first asset of the right kind is used instead, and when there is none the check fails cleanly - it
// never stages the wrong file.
static const char *findAsset(const char *assets, const char *wanted, const char *suffix) {
    const size_t suffix_length = strlen(suffix);
    const size_t wanted_length = wanted ? strlen(wanted) : 0;
    const bool wanted_matches_kind = wanted_length >= suffix_length &&
        !strncasecmp(wanted + wanted_length - suffix_length, suffix, suffix_length);
    for (const char *p = assets; (p = strstr(p, "\"name\"")) != NULL; p += 6) {
        const char *value = jsonValue(p, "\"name\"");
        if (!value || *value != '"') continue;
        const char *name = value + 1;
        size_t length = strcspn(name, "\"");
        if (wanted_matches_kind) {
            if (length == wanted_length && !strncmp(name, wanted, length)) return name;
        } else if (length >= suffix_length && !strncasecmp(name + length - suffix_length, suffix, suffix_length)) {
            return name;
        }
    }
    return NULL;
}

int pokeupdateCheck(const PokeUpdateConfig *config, bool folder_install, const char *current_revision, PokeUpdateRemote *remote, char *error, size_t error_size) {
    memset(remote, 0, sizeof(*remote));
    char url[400];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", config->repo);
    // The token only ever appears in this header: no log line and no `error` string is built from it.
    char authorization[600];
    snprintf(authorization, sizeof(authorization), "Bearer %s", config->token);
    const PlatformHttpHeader headers[] = {{"Accept", "application/vnd.github+json"},
                                          {"X-GitHub-Api-Version", "2022-11-28"},
                                          {"Authorization", authorization}};
    int status = 0;
    char reason[160] = "";
    PlatformHttp *http = platformHttpSend("GET", url, headers, 3, NULL, 0, &status, reason, sizeof(reason));
    if (!http) {
        fail(error, error_size, "cannot reach GitHub: %s", reason);
        return -1;
    }
    static char text[CHECK_BODY_LIMIT + 1];
    size_t have = 0;
    for (int64_t got; have < CHECK_BODY_LIMIT && (got = platformHttpRead(http, text + have, CHECK_BODY_LIMIT - have)) > 0;) have += (size_t)got;
    text[have] = 0;
    platformHttpClose(http);
    if (status != 200) {
        fail(error, error_size, "GitHub answered HTTP %d for the release", status);
        return -1;
    }
    char tag[64];
    if (!jsonString(jsonValue(text, "\"tag_name\""), tag, sizeof(tag))) {
        fail(error, error_size, "the release has no tag");
        return -1;
    }
    const char *assets = strstr(text, "\"assets\"");
    if (!assets) {
        fail(error, error_size, "the release has no assets");
        return -1;
    }
    const char *asset = findAsset(assets, config->asset, folder_install ? "eboot.bin" : ".ffpfsc");
    if (!asset) {
        // loader-55: a release with only the other kind (usually a folder release with no .ffpfsc) is "nothing to
        // install for this layout", so the message names the kind that is missing rather than echoing `config->asset`.
        fail(error, error_size, folder_install ? "the release has no eboot.bin to install" : "the release has no .ffpfsc image to install");
        return -1;
    }
    const char *url_key = urlBefore(assets, asset);
    if (!url_key || !jsonString(jsonValue(url_key, "\"url\""), remote->api_url, sizeof(remote->api_url))) {
        fail(error, error_size, "the asset has no API url");
        return -1;
    }
    size_t name_length = strcspn(asset, "\"");
    snprintf(remote->asset, sizeof(remote->asset), "%.*s", (int)name_length, asset);
    const char *size_value = jsonValue(asset, "\"size\"");
    remote->size = size_value && isdigit((unsigned char)*size_value) ? strtoull(size_value, NULL, 10) : 0;
    const char *digest_value = jsonValue(asset, "\"digest\"");
    char raw_digest[128];
    if (digest_value && *digest_value == '"' && jsonString(digest_value, raw_digest, sizeof(raw_digest)) && !strncasecmp(raw_digest, "sha256:", 7)) {
        snprintf(remote->digest, sizeof(remote->digest), "%s", raw_digest + 7);
        remote->has_digest = remote->digest[0] != 0;
    }
    snprintf(remote->tag, sizeof(remote->tag), "%s", tag);
    int newer = releaseCompare(remote->tag, current_revision) > 0;
    diagnosticsTrace("pokeupdate: %s offers %s (%s, %llu bytes, digest %s); this loader is %s", config->repo, remote->tag, remote->asset,
                     (unsigned long long)remote->size, remote->has_digest ? remote->digest : "(none)", newer ? "older" : "up to date");
    return newer ? 1 : 0;
}

// ---- installing ---------------------------------------------------------------------------------------------------------------------
static void upperHex(char *text) {
    for (; *text; ++text)
        if (*text >= 'a' && *text <= 'f') *text -= 'a' - 'A';
}
static bool writeText(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}
// The first line of a small text file, trimmed. False when it is missing or empty.
static bool readText(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t got = read(fd, out, size - 1);
    close(fd);
    if (got <= 0) return false;
    out[got] = 0;
    char *start = trim(out);
    memmove(out, start, strlen(start) + 1);
    return out[0] != 0;
}
// The asset, opened for reading: the token header is added only here, and the caller never sees it.
static PlatformHttp *openDownload(const PokeUpdateConfig *config, const char *url, int *status, char *error, size_t error_size) {
    char authorization[600];
    snprintf(authorization, sizeof(authorization), "Bearer %s", config->token);
    const PlatformHttpHeader headers[] = {{"Accept", "application/octet-stream"}, {"Authorization", authorization}};
    PlatformHttp *http = platformHttpSend("GET", url, headers, 2, NULL, 0, status, error, error_size);
    if (!http) return NULL;
    // GitHub answers this endpoint with a 302 to a signed storage url. The system usually follows it; when it does not,
    // follow it here without the Authorization header (the signed url carries its own, and two mechanisms at once are
    // rejected). The error string is never built from the url's query.
    if (*status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308) {
        char location[1200];
        bool have = platformHttpHeader(http, "Location", location, sizeof(location));
        platformHttpClose(http);
        if (!have) {
            snprintf(error, error_size, "GitHub redirected the download without a location");
            return NULL;
        }
        static const PlatformHttpHeader redirect[] = {{"Accept", "application/octet-stream"}};
        return platformHttpSend("GET", location, redirect, 1, NULL, 0, status, error, error_size);
    }
    return http;
}
// Streams `remote`'s asset into `path_new` (created 0755), hashing as it goes, and checks that its size and (when
// published) its SHA-256 match. 0 on success (with the lowercase hash copied to `digest_hex` when it is not NULL), -1
// with `error` set; on failure no file is left behind. Shared by the image install (.ffpfsc) and folder install (eboot).
static int downloadAsset(const PokeUpdateConfig *config, const PokeUpdateRemote *remote, const char *path_new, PokeUpdateProgress *progress, char *error, size_t error_size,
                         char *digest_hex, size_t digest_size) {
    unlink(path_new);  // an earlier attempt that was interrupted must not be taken for the finished file
    int out = open(path_new, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        fail(error, error_size, "cannot write %s (errno %d)", path_new, errno);
        return -1;
    }
    int status = 0;
    PlatformHttp *http = openDownload(config, remote->api_url, &status, error, error_size);
    if (!http) {
        char reason[200];
        snprintf(reason, sizeof(reason), "%s", error);
        close(out);
        unlink(path_new);
        fail(error, error_size, "the download failed: %s", reason);
        return -1;
    }
    if (status != 200) {
        platformHttpClose(http);
        close(out);
        unlink(path_new);
        fail(error, error_size, "GitHub answered HTTP %d for the update", status);
        return -1;
    }
    uint64_t total = remote->size;
    if (!total) {
        int64_t length = platformHttpLength(http);
        if (length > 0) total = (uint64_t)length;
    }
    atomic_store(&progress->done, 0);
    atomic_store(&progress->total, total);
    Sha256 hash;
    sha256Init(&hash);
    static unsigned char chunk[DOWNLOAD_CHUNK];
    uint64_t written = 0;
    bool ok = true;
    for (;;) {
        int64_t got = platformHttpRead(http, chunk, sizeof(chunk));
        if (got < 0) {
            fail(error, error_size, "the connection dropped while downloading the update");
            ok = false;
            break;
        }
        if (!got) break;
        sha256Update(&hash, chunk, (size_t)got);
        for (size_t done = 0; done < (size_t)got;) {
            ssize_t wrote = write(out, chunk + done, (size_t)got - done);
            if (wrote <= 0) {
                fail(error, error_size, "writing the update failed (errno %d; storage full?)", errno);
                ok = false;
                break;
            }
            done += (size_t)wrote;
        }
        written += (uint64_t)got;
        atomic_store(&progress->done, written);
        if (!ok) break;
    }
    platformHttpClose(http);
    unsigned char digest[32];
    char hex[65];
    sha256Final(&hash, digest);
    sha256Hex(digest, hex);
    if (ok && written == 0) {
        fail(error, error_size, "the download was empty");
        ok = false;
    }
    if (ok && total && written != total) {
        fail(error, error_size, "the update is %llu bytes, expected %llu", (unsigned long long)written, (unsigned long long)total);
        ok = false;
    }
    if (ok && remote->has_digest && strcasecmp(hex, remote->digest)) {
        fail(error, error_size, "the update hash does not match the published one");
        ok = false;
    }
    close(out);
    if (!ok) {
        unlink(path_new);  // never leave a second candidate behind for ShadowMountPlus to mount
        return -1;
    }
    if (digest_hex && digest_size) snprintf(digest_hex, digest_size, "%s", hex);
    atomic_store(&progress->done, written);
    atomic_store(&progress->total, written);
    return 0;
}

// A folder install (loader-52): the update is the executable alone, so a checked download is renamed straight over
// /app0/eboot.bin. The running process keeps its own copy, and the new build starts at the next launch - no image, no
// staging marker, no ShadowMount+ rescan and nothing to restart.
static int installEboot(const PokeUpdateConfig *config, const PokeUpdateRemote *remote, PokeUpdateProgress *progress, char *error, size_t error_size) {
    if (downloadAsset(config, remote, POKEUPDATE_EBOOT_NEW, progress, error, error_size, NULL, 0)) return -1;
    // loader-56: the file must be a signed executable (an FSELF, magic 4F 15 3D 1D), because that is the only form the
    // console's own launcher starts from /app0/eboot.bin. A bare ELF (0x7f 'E' 'L' 'F') is what the console's storage
    // layer hands out when it decrypts an FSELF - it is not launchable, and replacing the executable with it makes every
    // later launch die between the mount and the sandbox (SMP: "launch mount ready" and then nothing). loader-53 checked
    // for that ELF magic; loader-56 checks for the SELF one instead, so a mis-published asset is refused here and the
    // working executable stays in place.
    unsigned char magic[4] = {0};
    int fd = open(POKEUPDATE_EBOOT_NEW, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, magic, sizeof(magic)) : -1;
    if (fd >= 0) close(fd);
    if (got != (ssize_t)sizeof(magic) || magic[0] != 0x4f || magic[1] != 0x15 || magic[2] != 0x3d || magic[3] != 0x1d) {
        unlink(POKEUPDATE_EBOOT_NEW);
        fail(error, error_size, "the downloaded file is not a signed (FSELF) executable (is the release's eboot.bin the decrypted ELF?)");
        return -1;
    }
    if (rename(POKEUPDATE_EBOOT_NEW, POKEUPDATE_EBOOT_PATH)) {
        fail(error, error_size, "cannot replace the executable (errno %d)", errno);
        unlink(POKEUPDATE_EBOOT_NEW);
        return -1;
    }
    diagnosticsTrace("pokeupdate: installed %s (%s) into %s", remote->tag, remote->asset, POKEUPDATE_EBOOT_PATH);
    return 0;
}

int pokeupdateInstall(const PokeUpdateConfig *config, bool folder_install, const PokeUpdateRemote *remote, PokeUpdateProgress *progress, char *error, size_t error_size) {
    error[0] = 0;
    if (!remote->api_url[0]) {
        fail(error, error_size, "there is nothing to download");
        return -1;
    }
    if (folder_install) return installEboot(config, remote, progress, error, error_size);
    mkdir("/data/homebrew", 0755);  // the image lives here; the other files below live in folders that already exist
    // loader-51: an earlier session may have downloaded and staged this exact image while the console kept running the
    // previous mount (ShadowMount+ held the old one busy, so the change never took). The marker names the staged digest,
    // so skip the ~260 MB download: just re-apply it in place and let the caller ask ShadowMount+ to rescan.
    if (remote->has_digest) {
        char marker[128];
        struct stat info;
        if (readText(POKEUPDATE_MARKER_PATH, marker, sizeof(marker)) && !strcasecmp(marker, remote->digest) && !stat(POKEUPDATE_IMAGE_PATH, &info) &&
            (!remote->size || (uint64_t)info.st_size == remote->size)) {
            char line[160];
            snprintf(line, sizeof(line), "%s\n# staged %s\n", POKEUPDATE_IMAGE_PATH, marker);
            if (!writeText(POKEUPDATE_SHADOW_LIST, line)) {
                fail(error, error_size, "the image is already staged but %s could not be written (errno %d)", POKEUPDATE_SHADOW_LIST, errno);
                return -1;
            }
            atomic_store(&progress->done, remote->size);
            atomic_store(&progress->total, remote->size);
            diagnosticsTrace("pokeupdate: image %s is already staged (sha256 %s, %llu bytes); re-applying", remote->asset, marker, (unsigned long long)info.st_size);
            return 0;
        }
    }
    char hex[65] = "";
    if (downloadAsset(config, remote, POKEUPDATE_IMAGE_NEW, progress, error, error_size, hex, sizeof(hex))) return -1;
    // Replace the image in place. A .old to rename back to would be one more candidate for the same title ID, so the
    // rename is the only step (as tools/ps5-autoupdate.py does).
    if (rename(POKEUPDATE_IMAGE_NEW, POKEUPDATE_IMAGE_PATH)) {
        fail(error, error_size, "cannot replace the image (errno %d)", errno);
        unlink(POKEUPDATE_IMAGE_NEW);
        return -1;
    }
    char uppercase[65];
    snprintf(uppercase, sizeof(uppercase), "%s", hex);
    upperHex(uppercase);
    char line[128];
    snprintf(line, sizeof(line), "%s\n", uppercase);
    if (!writeText(POKEUPDATE_MARKER_PATH, line)) diagnosticsTrace("pokeupdate: could not write %s", POKEUPDATE_MARKER_PATH);
    // ShadowMountPlus keeps its mount of the image cached between launches and only rescans when manual.lst changes
    // content; an empty write over an empty file is invisible, so the console keeps running the previous build. The
    // installed digest differs from the last one, so writing it here makes the file change and the rescan fire.
    snprintf(line, sizeof(line), "%s\n# staged %s\n", POKEUPDATE_IMAGE_PATH, uppercase);
    if (!writeText(POKEUPDATE_SHADOW_LIST, line)) {
        fail(error, error_size, "the image was installed but %s could not be written (errno %d); the console may keep the previous build until it is staged from the PC",
             POKEUPDATE_SHADOW_LIST, errno);
        return -1;
    }
    atomic_store(&progress->done, remote->size);
    atomic_store(&progress->total, remote->size);
    diagnosticsTrace("pokeupdate: installed %s (%s, sha256 %s)", remote->tag, remote->asset, uppercase);
    return 0;
}

// ---- loader-51: making ShadowMountPlus take up the staged image without a restart -----------------------------------------------
// ShadowMountPlus mounts the image on demand at each launch and releases it when a title exits. Its release can get stuck
// after this title's same-app handoff ("runtime release awaiting sandbox removal"), and then every later launch reuses the
// stale mount and boots the previous build. Rewriting manual.lst is not enough: SMP only watches it while no title runs,
// and its scanner is not the thing that clears a stuck release. Its loopback HTTP/JSON API is: a full scan with
// reset_attempts clears the mount retry counters and re-reads the sources. While a game is running SMP queues that scan
// and runs it once the title has fully closed, so "close and start again" becomes enough - no console or SMP restart.
int pokeupdateNudgeShadowMount(char *note, size_t note_size) {
    if (note && note_size) note[0] = 0;
    static const char body[] = "{\"reset_attempts\":true}";
    static const PlatformHttpHeader headers[] = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    int status = 0;
    char reason[160] = "";
    PlatformHttp *http = platformHttpSend("POST", POKEUPDATE_SHADOW_API, headers, 2, body, sizeof(body) - 1, &status, reason, sizeof(reason));
    if (!http) {
        if (note && note_size) snprintf(note, note_size, "ShadowMount+ API unreachable");
        diagnosticsTrace("pokeupdate: shadowmount rescan request failed: %s", reason);
        return -1;
    }
    char text[512];
    size_t have = 0;
    for (int64_t got; have < sizeof(text) - 1 && (got = platformHttpRead(http, text + have, sizeof(text) - 1 - have)) > 0;) have += (size_t)got;
    text[have] = 0;
    platformHttpClose(http);
    if (status != 200) {
        if (note && note_size) snprintf(note, note_size, "ShadowMount+ API answered HTTP %d", status);
        diagnosticsTrace("pokeupdate: shadowmount rescan answered HTTP %d: %s", status, text);
        return -1;
    }
    bool deferred = strstr(text, "\"scan_deferred\":true") != NULL;
    diagnosticsTrace("pokeupdate: shadowmount rescan requested (deferred until the game closes=%d): %s", deferred, text);
    if (note && note_size) snprintf(note, note_size, "%s", deferred ? "queued until the game closes" : "queued");
    return 0;
}

// ---- loader-54: a folder install removes any leftover image ------------------------------------------------------------------------
// ShadowMountPlus scans for images and mounts every title it finds, so a .ffpfsc left beside the folder is a second
// candidate it has to recognise and ignore as a "duplicate" at every scan (its log says so). Once the title runs from
// the folder there is no reason for the image to exist, so the loader deletes it - and an interrupted staging file -
// by itself, on every console and at every start. Nothing to delete is normal and silent; the caller only invokes this
// in a folder install (a writable /app0), never in an image install where the image is the title itself.
void pokeupdateRemoveLeftoverImage(void) {
    if (!unlink(POKEUPDATE_IMAGE_PATH)) diagnosticsTrace("pokeupdate: removed the leftover image %s (the folder install is used)", POKEUPDATE_IMAGE_PATH);
    if (!unlink(POKEUPDATE_IMAGE_NEW)) diagnosticsTrace("pokeupdate: removed the interrupted staging file %s", POKEUPDATE_IMAGE_NEW);
    // The marker only describes the image; with the image gone its content is stale. Removing it keeps a later
    // image install (should one ever be staged again) from being mistaken for "already applied".
    if (!unlink(POKEUPDATE_MARKER_PATH)) diagnosticsTrace("pokeupdate: removed the stale image marker %s", POKEUPDATE_MARKER_PATH);
    // loader-57: the same holds for a folder install's half-written executable. An update that was interrupted (or whose
    // download was refused) leaves /app0/eboot.bin.new behind; nothing would ever use it, and it is a second executable
    // in the title folder. Remove it at every start so the folder only ever holds the executable that runs.
    if (!unlink(POKEUPDATE_EBOOT_NEW)) diagnosticsTrace("pokeupdate: removed the interrupted download %s", POKEUPDATE_EBOOT_NEW);
}

// loader-58: the confirmation for the first start of a new build. The revision that last started is kept in
// POKEUPDATE_RUNNING_PATH; if this start is a different one, the player is told "Updated to Rev 1.1.45" on the ready menu.
// Comparing revisions (rather than trusting a note the installer wrote) also covers an update that arrived some other way -
// a manual FTP deploy, or an older loader that predates this - which is exactly the case that confused the last test: the
// executable had been updated, the old loader said "failed", and the next start silently ran the new build.
//
// The record is written on every start, so the notice appears once. A console that has never run this build has no record
// (a fresh install), and says nothing: there is no update to confirm. Failing to read or write the record only costs the
// notice, never the start.
bool pokeupdateTakeUpdateNotice(const char *current_revision, char *notice, size_t notice_size) {
    char previous[80];
    bool have_previous = readText(POKEUPDATE_RUNNING_PATH, previous, sizeof(previous));
    char line[96];
    snprintf(line, sizeof(line), "%s\n", current_revision);
    if (!writeText(POKEUPDATE_RUNNING_PATH, line)) diagnosticsTrace("pokeupdate: could not record the running revision in %s", POKEUPDATE_RUNNING_PATH);
    if (!have_previous || !strcmp(previous, current_revision)) return false;
    const char *revision = current_revision[0] == 'v' || current_revision[0] == 'V' ? current_revision + 1 : current_revision;
    if (notice && notice_size) snprintf(notice, notice_size, "Updated to Rev %s", revision);
    diagnosticsTrace("pokeupdate: this is the first start of %s (the previous build was %s)", current_revision, previous);
    return true;
}
