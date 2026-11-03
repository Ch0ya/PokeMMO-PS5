# v1.1.91 — PokeMMO for PS5

The loader that runs the **official PokeMMO Linux client** natively on a jailbroken PlayStation 5. The client is
downloaded by the client itself; this project ships no PokeMMO assets and no game ROMs.

This release is mostly about **frame time**. Everything below was measured on the console, not estimated.

## What changed since v1.1.72

- **The loader no longer stalls the frame.** The screen probes that detect the client's battle and hotbar read
  pixels back about four times a second, and each read waited for the GPU to finish the frame. They now queue the
  read and collect it a frame later: **84 ms → 0 ms** per ten-second window.
- **One frame pacer instead of two.** The client both limits itself to 60 fps and asks for vsync, so it slept to
  the deadline and then presented — a frame that finished slightly late waited for the *next* vblank and became a
  33 ms frame. The client's own cap is now out of the way and vsync paces the game.
- **The client's garbage collector was tuned.** Its bundled runtime is Serial-only and was running with a 512 MB
  young generation, which it copies on every collection: **40–140 ms pauses**. With a 32 MB young generation, a
  1 GB committed heap and the collector's own pause logging on, pauses are **25–43 ms** and collections are rare.
- Result in steady play: slow frames went from **17–48% of frames to 4–7%**, and there are no longer frames
  landing on 33 ms in pairs.

## The remaining stutter — what it is and what it is not

It is **not** the loader, and it is measurable. Over a full session the loader's present path costs under 1 ms
per frame, and the client's own thread spends **60–75% of each frame's wall time as CPU** — in crowded areas about
**12 ms of CPU per frame** against a 16.7 ms budget, which is why busy towns still drop frames.

The largest stalls (0.5–1.6 s when a zone loads) were broken down per frame: the worst three contain
**1.5 seconds of pure CPU with zero disk reads, zero texture uploads and zero time waiting on the network**. The
rest of a zone transition is split between reading the client's own files (one 188 MB read took 529 ms) and
texture uploads (38 uploads took 283 ms), both on the client's render thread.

In short: PokeMMO's own renderer, on this hardware, is the ceiling. The measurements and a written report with
proposed client-side fixes are in
[docs/frame-time-report.md](https://github.com/Ch0ya/PokeMMO-PS5/blob/ps5-working-v1/docs/frame-time-report.md),
and have been sent to the PokeMMO developers. If a client update improves it, the loader will pick the improvement
up without changes.

## Game files (you supply these)

No ROMs are distributed here. PokeMMO needs at least one Nintendo DS game, dumped from a cartridge you own:

- **Pokémon Black or White** — required, the client will not start without one.
- FireRed / Emerald — optional, adds Kanto / Hoenn.
- Platinum / HeartGold / SoulSilver — optional, adds Sinnoh / Johto.

Copy them into **`/data/PokeMMO-roms/`** (FTP works; the loader creates the folder and mounts it as the client's
ROM folder). This works for folder and image installs, and file names do not matter.

## Assets

| Asset | What it is |
|---|---|
| `PPSA98001.zip` | **The title**, unzip into `/data/homebrew/` (folder install). Ships no game ROMs. |
| `PPSA98001.ffpfsc` | **The same title as a compressed image**, for launchers that only install images. Ships no game ROMs. |
| `eboot.bin` | The loader executable alone, for an existing install and the title's own updater. |

## Requirements

- A jailbroken PS5. Tested on **firmware 12.40** with **kstuff-lite 1.07+** and **ShadowMountPlus 1.7beta3**.
- Internet and a free PokeMMO account for the client download.
- Your own ROMs, as above.

## Installing and updating

**Folder install:** unzip `PPSA98001.zip` into `/data/homebrew/`.
**Image install:** install `PPSA98001.ffpfsc`.
Then put your ROMs in `/data/PokeMMO-roms/` and start **PokeMMO**.

**Existing install:** the title can update itself — accept the prompt, then **close the title fully and start it
again**, because a running title holds its own executable open.

If an update ever fails to start, install `eboot.bin` from this release over FTP as `eboot.new` and rename it to
`eboot.bin` with the title closed; that is the same replace the updater performs.

## Settings

- The interface size is 100%–150% (left/right on the Settings screen); 125% is the default and is what this
  release is tuned for.
- The client renders at 1440p and the console scales to 4K, which keeps the interface size while giving the
  renderer a third fewer pixels each frame.
- At most 10 other players are drawn (`max_visible_players`), which is the single biggest lever on frame time in
  towns.

## Credits

- **Ch0ya** — this fork: controller pointer, interface-size rendering, framebuffer repairs, Gengar branding,
  packaging and the performance work above.
- **Shabbypenguin** — [PokeMMO-Prospero](https://github.com/Shabbypenguin/PokeMMO-Prospero), the original PS5
  port, and [pokemmo-ps5-buildenv](https://github.com/Shabbypenguin/pokemmo-ps5-buildenv), the pinned toolchain.
- **Petit_Prince** — [PokeMMO-NX](https://github.com/Petit-Prince-dev/PokeMMO-NX), whose design this port follows.
- Mesa, ps5-opengl, ps5-payload-sdk and the ps5-native-app-boilerplate, as listed in
  [CREDITS.md](https://github.com/Ch0ya/PokeMMO-PS5/blob/ps5-working-v1/CREDITS.md).

No game data, client binaries or artwork from PokeMMO are redistributed.
