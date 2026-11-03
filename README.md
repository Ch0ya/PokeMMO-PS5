# PokeMMO on PS5

**The un-official PokeMMO Linux client, running natively on a jailbroken PlayStation 5 — no streaming, no PC, no emulator layer in between.**

This is a loader. It maps the real, unmodified PokeMMO client (a GraalVM native image built for Linux x86-64),
gives it the Linux, SDL3, OpenGL, OpenAL and GTK pieces it asks for, and puts the client's own interface on
the television with the DualSense in your hands. The client is never bundled: the console downloads it from
PokeMMO itself, and it updates the way the desktop client does.

> **Status:** playable. Login, character select, the overworld, battles, chat, the bag, the party and the
> on-screen keyboard all work, with sound. Tested on **firmware 12.40** with **kstuff-lite** and
> **ShadowMountPlus**. The 4K interface can be scaled from **100% to 150%** and stays smooth.

This project is a fork of **[PokeMMO-Prospero](https://github.com/Shabbypenguin/PokeMMO-Prospero) by
Shabbypenguin**, who built the original PS5 port — the loader, the title packaging and the first branding. The
fork adds the controller pointer, the interface-size rendering, the framebuffer repairs and the Gengar art.

---

## What you need

| | |
|---|---|
| **Console** | A jailbroken PS5. Tested on firmware 12.40; other firmware only if your exploit supports fake-signed titles. |
| **Homebrew runtime** | [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) to run fake-signed titles, and **ShadowMountPlus** to mount the title (folder or `.ffpfsc` image). |
| **Network** | The first start downloads the PokeMMO client (about 93 MB) from PokeMMO's own servers. |
| **PokeMMO account** | Free, from the title's login screen or pokemmo.com. |
| **Your own ROMs** | This release ships **no** game files. PokeMMO needs a Nintendo DS Pokémon ROM (Black or White) that you dump from a cartridge you own; see [Game files](#game-files). |

---

## Installing

1. Put the title in place with ShadowMountPlus:
   - **Folder install (recommended):** unzip the title package into `/data/homebrew/PPSA98001/`.
   - **Image install:** install `PPSA98001.ffpfsc` if your launcher only handles images.
2. Put your ROMs in `/data/PokeMMO-roms/` (see [Game files](#game-files)); the package contains none.
3. Start **PokeMMO** from the home screen. The loading screen downloads the client; leave it running.
4. Choose **Start Game** and log in.

A folder install keeps the client, settings and logs inside the title's storage and can replace its own
executable, so updates need nothing but a restart. An image install is read-only; updates stage a new image.

**The title updates itself.** With an update source configured it asks before installing and verifies the
download against the published SHA-256; a folder install replaces its own executable, so an update is "install,
close, start again". See [Updating](#updating).

**Each PS5 profile gets its own PokeMMO login and settings.** The client and game files are shared. To switch
player, close the title, change profile, start it again.

### Game files

The release ships **no** ROMs (they are copyrighted; this project does not distribute them). PokeMMO needs at
least one Nintendo DS game:

| Game | Needed? |
|---|---|
| Pokémon Black **or** White (`.nds`) | **Required — the client will not start without one** |
| FireRed / Emerald (`.gba`) | Optional: Kanto / Hoenn |
| Platinum, HeartGold or SoulSilver (`.nds`) | Optional: Sinnoh / Johto |

Dump them from cartridges you own. Copy the files into **`/data/PokeMMO-roms/`** (FTP works; the loader creates the
folder and mounts it as the client's ROM folder). This works for a **folder install and an image install** alike, and
file names do not matter. If you build the title yourself, files in `private/roms/` are embedded in the title and
take precedence over that folder.

---

## Playing

The pad drives **PokeMMO's own interface**; the loader only adds what the client's controller map cannot do.

| Button | Action |
|---|---|
| Left stick / d-pad | The client's own movement and menu navigation |
| **Cross** | A — confirm, talk, interact |
| **Circle** | B — cancel, back. **Hold 700 ms** to also send Escape, for panels that only show a corner X |
| **Square** | Use the highlighted hotbar slot |
| **Triangle** | Pokédex |
| **L1 / R1** | Move along the hotbar; hold to page it |
| **L2** | Party hotbar on the right (the client's ring focus) |
| **Options** | Game menu |
| **Touchpad press** | Bag |
| **R3** | On-screen keyboard on/off |
| **R2 + left stick** | Pointer: hold R2, steer with the stick, **Cross** clicks. Releases R2 to put it away |
| **R2 tap / R2 still hold** | Return / Tab, for the client's keyboard-only login and dialog fields |

The battle move summary needs no button: while the move grid is up, the loader keeps the pointer on the
selected move and the client shows its Power and Accuracy by itself.

### Interface size

The client's own UI scaling stops at 200%, which is small on a 4K panel. The ready menu's **L1/R1**, or
**Settings → Interface size**, pick **100%–150%** in 5% steps. Above 100% the client draws into an offscreen
surface and the loader presents it scaled to the screen, so the whole picture — world and interface — grows
without a per-frame copy. The setting is kept between starts.

### Logs

**Triangle** opens Settings at any time. **Settings → Log** shows this start's log, the previous start's log
and ps5-opengl's own driver log (Square switches between them), with line numbers and live refresh.
`/app0/pokemmo.log` and `/app0/ps5-opengl.log` can also be fetched over FTP.

### Updating

The loader updates itself, the client updates itself, and nothing is re-installed by hand:

- **Loader:** `/data/pokeupdate.ini` on the console names the release channel (`repo`, optional `asset`, and a
  read-only `token`). At start the title asks GitHub for the newest release, offers it, and checks the download
  against the published SHA-256 before replacing its own executable. Close the title and start it again to use
  the new build. Without that file the check is inert. **Settings → Check for updates** repeats it on demand.
- **Client:** downloaded and updated by the title from PokeMMO itself, into the slot that is not in use; a
  client that fails to start is rolled back to the previous one.

---

## Building

The toolchain is pinned in [pokemmo-ps5-buildenv](https://github.com/Shabbypenguin/pokemmo-ps5-buildenv).

```bash
git clone https://github.com/Shabbypenguin/pokemmo-ps5-buildenv.git
git clone https://github.com/Ch0ya/PokeMMO-PS5.git
cd PokeMMO-PS5

../pokemmo-ps5-buildenv/ps5env build                # once: build the toolchain image
../pokemmo-ps5-buildenv/ps5env make loader          # folder title  -> dist/pokemmo-ps5-loader-PPSA98001.zip
../pokemmo-ps5-buildenv/ps5env make image-loader    # image title   -> dist/PPSA98001.ffpfsc
```

Put the game files in `private/roms/` first (git-ignored); the build copies them into the title. The build
stops with a clear message if the folder is empty. `make loader` produces the folder title; `make image-loader`
the `.ffpfsc`.

Releases are built with `make loader` / `make image-loader` and attached to a GitHub release by hand. The title also
carries its own on-console updater, configured by `/data/pokeupdate.ini` on the console (`repo`, optional `asset`,
optional read-only `token`).

Repository layout:

| Path | What |
|---|---|
| `loader/` | the Linux runtime, SDL/GL/audio adapters, pad layer, loading screen, ready menu, Settings and logs |
| `assets/` | branding, default game settings, preloaded ROMs (build output) |
| `scripts/build-title.sh` | assembles the native PS5 title (FSELF, `libc.prx`, `sce_sys`) against ps5-opengl |
| `docs/` | [plan](docs/plan.md), [loader internals](docs/loader.md), [to do](docs/todo.md) |
| `CREDITS.md` | every third-party package, its author and its license |

---

## Compatibility, honestly

- **Not a one-click install for the general public.** It needs a jailbroken console, a homebrew mounter, and
  ROMs you dumped yourself. On stock firmware it will not run at all.
- **Tested on one setup:** firmware 12.40, kstuff-lite 1.07+, ShadowMountPlus 1.7beta3. Other firmware
  versions depend entirely on whether your exploit can run fake-signed titles.
- **The release ships no game files**; add your own ROMs as described above.
- **Not implemented:** USB/Bluetooth keyboards and mice, HDR.
- **Image installs** work without embedded ROMs: the loader mounts `/data/PokeMMO-roms/` as the client's ROM folder.
- The PokeMMO client is proprietary and downloaded from PokeMMO; this project ships none of it, and none of
  Sony's or Nintendo's files.

---

## Credits

This port stands on other people's work. The full, per-file list is in [CREDITS.md](CREDITS.md); the short
version:

| Project | Author | Used for |
|---|---|---|
| [PokeMMO-Prospero](https://github.com/Shabbypenguin/PokeMMO-Prospero) | **Shabbypenguin** | the original PS5 build this project is forked from: the first loader, the title packaging and the original branding |
| [pokemmo-ps5-buildenv](https://github.com/Shabbypenguin/pokemmo-ps5-buildenv) | **Shabbypenguin** | the pinned toolchain this project builds with |
| [PokeMMO-NX](https://github.com/Petit-Prince-dev/PokeMMO-NX) | **Petit_Prince** (MIT) | the approach and much of the loader's Linux runtime |
| [ps5-opengl](https://github.com/darkxex/ps5-opengl) | **BlackBearReloaded** (GPL-3.0+; original repo gone — preserved mirror by darkxex) | OpenGL/EGL for the titles (Mesa-based) |
| [ps5-native-app-boilerplate](https://github.com/darkxex/ps5-native-app-boilerplate) | **BlackBearReloaded** (GPL-3.0+; preserved source snapshot by darkxex) | title packaging, FSELF writer, `libc.prx` |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | **John Törnblom** and contributors | compiler target, headers, system stubs |
| [Mesa](https://mesa3d.org/) | Mesa contributors | the GL implementation ps5-opengl builds on |
| [zlib](https://zlib.net/) | **Jean-loup Gailly, Mark Adler** | compression used by the loader and its assets |
| [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) | **EchoStretch** | running fake-signed titles (console side) |
| ShadowMountPlus | **EchoStretch** | mounting the title |
| [DejaVu fonts](https://dejavu-fonts.github.io/) | DejaVu project (Bitstream Vera license) | the loader's overlay and file-chooser fonts |
| [PokeAPI](https://pokeapi.co/) | PokeAPI contributors | the Gengar artwork the Gengar branding is cut from |

PokeMMO is © the PokeMMO team; Pokémon is © Nintendo / Creatures / GAME FREAK. This is an unofficial fan
project, not affiliated with or endorsed by any of them.

## License

GPL-3.0-or-later ([LICENSE](LICENSE)): the title links ps5-opengl and is built with the GPL native-app
boilerplate. Files adapted from PokeMMO-NX keep their MIT notice
([LICENSES/PokeMMO-NX-MIT.txt](LICENSES/PokeMMO-NX-MIT.txt)).
