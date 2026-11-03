# Frame-time report from a PS5 port of the Linux client

## What this is

I maintain a homebrew PS5 loader for PokeMMO ([github.com/Ch0ya/PokeMMO-PS5](https://github.com/Ch0ya/PokeMMO-PS5)).
It runs the official Linux x64 client unmodified on a PS5: the loader provides the Linux ABI the client's
libraries expect, plus SDL3 / EGL / OpenGL 4.6 on the console's GPU. Game files are the player's own; the project
distributes no PokeMMO assets and no ROMs.

This is not a support request for the port. It is a frame-time report from a platform with a fixed 60 Hz target,
with measurements, because the same issues show up on low-end PCs and the fixes are in the client.

## Setup

- PS5, 8-core Zen 2, RDNA2. Client renders at 2560x1440, console scales the output to 4K. Interface at 125%.
- Client: the official Linux build (Java 25 / GraalVM runtime bundled with it), OpenGL backend.
- Everything below is measured **inside the loader's present path**, so it is the client's own time:
  - wall time between presents,
  - CPU time of the presenting thread (`getrusage(RUSAGE_THREAD)`),
  - time and bytes inside the client's `read()` calls,
  - time inside its socket `recv()` calls,
  - time inside `glTexImage*` / `glTexSubImage*` uploads,
  - plus the client's own `-XX:+PrintGC` output.

## Measurements

Steady play, per 600 frames (~10-19 s). "CPU" is the render thread's own CPU time:

| window | wall | CPU | wait |
|---|---|---|---|
| 1 | 19391 ms | 10992 ms | 8731 ms |
| 2 | 12795 ms | 7230 ms | 5564 ms |
| 3 | 11118 ms | 7156 ms | 3962 ms |
| 4 | 12281 ms | 7212 ms | 5068 ms |
| 5 | 17544 ms | 13093 ms | 4465 ms |

So in busy scenes the render thread spends **~12 ms of CPU per frame** (wall 18–30 ms), against a 16.7 ms budget.

Single-frame stalls, with the work inside them broken down:

| interval | CPU | disk read | texture uploads | socket recv |
|---|---|---|---|---|
| 1103 ms | 515 ms | 210 ms (57 MB) | 79 ms | 0 |
| 1239 ms | 737 ms | 529 ms (188 MB) | 0 | 0 |
| 1536 ms | **1534 ms** | 0 | 0 | 0 |
| 1584 ms | **1580 ms** | 0 | 282 ms | 0 |
| 836 ms | **828 ms** | 0 | 0 | 0 |

The three largest freezes are 0.8–1.6 s of **pure render-thread CPU** with no I/O of any kind.

Frame interval histogram in a busy area (1 ms buckets, 600 frames): 16:303, 17:123, then a second cluster at
31:10, 32:29, 33:44, 34:19 — i.e. frames land on 16.7 ms or 33.3 ms. The client also caps its own rate
(`client.graphics.max_fps`) and sleeps to that deadline before presenting; on a platform where the present call
does not block, that sleep is what turns a 17 ms frame into a 33 ms one.

Garbage collection (the runtime bundled with the client): Serial collector only — its own option table says
"Serial GC only", `InitialCollectionPolicy=Adaptive2`, and it rejects `-XX:MaxGCPauseMillis`, `-Xlog` and any
option name longer than 16 characters. With a 512 MB young generation it paused **40–140 ms**; with 32 MB it
pauses **25–43 ms**, roughly every 20 s in steady play.

## What would fix it on the client side

Ordered by measured impact.

1. **Move zone/state loading off the render thread.** The 1.5 s stalls are pure CPU on the presenting thread.
   A background loader plus a bounded work queue drained at a per-frame budget (say 2 ms) would turn them into
   a slightly longer transition instead of a visible freeze. Prefetching connected maps while the player is a
   screen away would remove most of them entirely.

2. **Give texture uploads their own budget.** During one transition: 27 uploads costing 79 ms, and 38 uploads
   costing 283 ms, all on the render thread. A PBO-backed uploader that moves at most N MB per frame (or until
   a loading screen is shown) removes that too.

3. **Don't pace the frame twice.** If `client.graphics.max_fps` is set and vsync is also requested, sleep until
   `frame_start + 1/fps` and then present — which misses the vblank whenever the frame ends a fraction late.
   Either skip the sleep when vsync is active, or make the deadline account for the present call, so the frame
   is submitted early rather than exactly on the boundary.

4. **Read the zone data in fewer, larger, or asynchronous reads.** One 188 MB read on the render thread cost
   529 ms. Reading on a loader thread and handing parsed data to the render thread would overlap it with
   rendering.

5. **GC**: the bundled runtime offers only a Serial collector. Exposing a low-pause option (or allowing the
   standard `-XX:+UseG1GC` / `-Xlog:gc` path through) would remove a 25–43 ms pause every ~20 s. Our workaround
   was to shrink the young generation to 32 MB, which trades pause length for frequency.

6. **A frame-time log** (`--frame-log` or similar: per-frame wall/CPU, stalls with what they were doing) would
   let anyone on any platform file reports like this one with numbers instead of descriptions.

## Contact

Happy to test any build or patch on the console and return measurements — including whether a fix actually
moves the numbers above. The loader's instrumentation is small and can be pointed at any build.
