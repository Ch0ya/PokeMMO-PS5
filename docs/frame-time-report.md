# Frame-time report from a PS5 port of the Linux client

## What this is

I maintain a homebrew PS5 loader for PokeMMO ([github.com/Ch0ya/PokeMMO-PS5](https://github.com/Ch0ya/PokeMMO-PS5)).
It runs the official Linux x64 client unmodified on a PS5: the loader provides the Linux ABI the client's
libraries expect, plus SDL3 / EGL / OpenGL 4.6 on the console's GPU. Game files are the player's own; the project
distributes no PokeMMO assets and no ROMs.

This is not a support request for the port. It is a frame-time report from a platform with a fixed 60 Hz target,
with measurements and patch sketches, because the same issues show up on low-end PCs and the fixes are in the
client.

The code below is written from the outside: the client is obfuscated, so names and call sites will not match your
tree. They are the *shape* of each change, not a diff. I am happy to test anything you build and report the same
numbers back.

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

So in busy scenes the render thread spends **~12 ms of CPU per frame** (wall 18-30 ms), against a 16.7 ms budget.

Single-frame stalls, with the work inside them broken down:

| interval | CPU | disk read | texture uploads | socket recv |
|---|---|---|---|---|
| 1103 ms | 515 ms | 210 ms (57 MB) | 79 ms | 0 |
| 1239 ms | 737 ms | 529 ms (188 MB) | 0 | 0 |
| 1536 ms | **1534 ms** | 0 | 0 | 0 |
| 1584 ms | **1580 ms** | 0 | 282 ms | 0 |
| 836 ms | **828 ms** | 0 | 0 | 0 |

The three largest freezes are 0.8-1.6 s of **pure render-thread CPU** with no I/O of any kind.

Frame interval histogram in a busy area (1 ms buckets, 600 frames): 16:303, 17:123, then a second cluster at
31:10, 32:29, 33:44, 34:19 - i.e. frames land on 16.7 ms or 33.3 ms. The client also caps its own rate
(`client.graphics.max_fps`) and sleeps to that deadline before presenting; on a platform where the present call
does not block, that sleep is what turns a 17 ms frame into a 33 ms one.

Garbage collection (the runtime bundled with the client): Serial collector only - its own option table says
"Serial GC only", `InitialCollectionPolicy=Adaptive2`, and it rejects `-XX:MaxGCPauseMillis`, `-Xlog` and any
option name longer than 16 characters. With a 512 MB young generation it paused **40-140 ms**; with 32 MB it
pauses **25-43 ms**, roughly every 20 s in steady play.

---

# Patch sketches

Ordered by measured impact. Each one has an acceptance test: the number that should move, and where it comes from.

## 1. Budget the frame's non-render work (turns 1.5 s stalls into a slower-looking transition)

The stalls with 1534 ms / 1580 ms / 828 ms of CPU and no I/O happen on the render thread. Whatever they are
(decode, model build, map assembly), they are executed synchronously inside the frame.

The general fix is a work queue that the render loop drains with a time budget, and a "loading" state that keeps
presenting the previous frame while the queue runs. In its simplest form:

```java
// One queue of small, resumable steps; nothing in it may block.
final class FrameBudget {
    private final ArrayDeque<Runnable> pending = new ArrayDeque<>();
    private static final long BUDGET_NS = 2_000_000L;   // 2 ms per frame, tune to 1-3

    void submit(Runnable step) { pending.add(step); }

    /** Call once per frame, after input, before drawing. */
    void drain() {
        long deadline = System.nanoTime() + BUDGET_NS;
        while (!pending.isEmpty()) {
            pending.peek().run();          // steps must be short: build one mesh, decode one page
            pending.poll();
            if (System.nanoTime() >= deadline) break;
        }
    }

    boolean busy() { return !pending.isEmpty(); }
}
```

If a step is too coarse to fit in a budget (one 188 MB read, one 4096x4096 atlas), it has to become several
steps: `open -> read header -> read chunk -> decode chunk -> build mesh -> upload`. The queue then interleaves
them with rendering, and the transition takes a few hundred ms of *slow frames* instead of one freeze.

Two things make this much easier and are worth doing at the same time:

- **Prefetch connected maps.** When the player is one screen from a map edge, enqueue that map's steps. By the
  time the border is crossed the work is done, and the freeze disappears rather than shrinking.
- **Show something during the transition.** With the old frame still presented, a 300 ms transition reads as a
  pause, not a hang.

**Acceptance test:** `sdl.frame.stall` lines above 500 ms disappear; per-window `wall_ms` grows slightly while
`cpu_ms` per frame stays under ~14 ms.

## 2. Give texture uploads a budget, and use a PBO

Measured on one transition: 27 uploads costing 79 ms, and 38 uploads costing 283 ms. All of them are
`glTexImage2D`/`glTexSubImage2D` calls made between two presents, so they are part of the frame.

Two changes, both standard OpenGL:

1. Upload through a **pixel unpack buffer** so the driver can DMA instead of copying from client memory:
   orphaning the buffer each frame (`glBufferData(..., null, GL_STREAM_DRAW)`) avoids the implicit sync that
   makes `glTexSubImage2D` from client memory wait for the previous upload.
2. Upload at most **N MB per frame** (2-4 MB is a reasonable start), leaving the rest queued for the next frame.
   Those bytes then cost a few hundred microseconds per frame instead of a 283 ms spike.

```java
final class UploadQueue {
    private static final long BYTES_PER_FRAME = 4L << 20;   // 4 MB
    private final ArrayDeque<Upload> pending = new ArrayDeque<>();
    private int pbo;

    void init() {
        int[] id = new int[1];
        glGenBuffers(id); pbo = id[0];
    }

    void submit(Upload u) { pending.add(u); }

    /** Call once per frame while a texture is current. */
    void drain() {
        long sent = 0;
        while (!pending.isEmpty() && sent < BYTES_PER_FRAME) {
            Upload u = pending.poll();
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, u.bytes.capacity(), null, GL_STREAM_DRAW); // orphan: no sync
            glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, u.bytes);
            // offset 0 in the buffer, not a client pointer: this is the DMA path
            glTexSubImage2D(u.target, u.level, u.x, u.y, u.w, u.h, u.format, u.type, 0L);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            sent += u.bytes.capacity();
        }
    }
}
```

If a transition must be complete before the world is drawn, the alternative is to show a loading screen and
upload everything behind it - still better than 283 ms inside the world, because nothing is animating.

**Acceptance test:** `upload_ms` per stall drops below ~20 ms; uploads spread over frames instead of one burst.

## 3. Do not pace the frame twice

The client caps itself to `client.graphics.max_fps` **and** asks for vsync. The limiter sleeps until
`frame_start + 1/fps` and only then presents, so a frame whose work ends at 16.8 ms misses the vblank and becomes
33.3 ms. The histogram above is exactly that shape: 16-17 ms or 31-36 ms, nothing in between.

```java
// Shape of the fix: one pacer, and the deadline measured from the last present, not from the frame start.
long now = System.nanoTime();
if (vsyncEnabled) {
    // the display paces us; the FPS cap can only make a good frame worse
} else if (fpsLimit > 0) {
    long due = lastPresentNanos + 1_000_000_000L / fpsLimit;
    long wait = due - now;
    if (wait > 0) LockSupport.parkNanos(wait);
}
lastPresentNanos = present();   // present first, then start the next frame's clock
```

On the PS5 build I set the client's cap out of the way (`max_fps=120`) and let vsync pace; that alone removed the
33 ms pairs. A platform-neutral version is the branch above, plus measuring the deadline from the last present.

**Acceptance test:** the histogram stops having a 31-36 ms cluster while the scene is otherwise unchanged.

## 4. Move zone reads off the render thread

One 188 MB read cost 529 ms on the presenting thread. Even with (1) in place, a single blocking `read()` of that
size cannot be split into 2 ms steps.

- Read on a loader thread into a pool buffer, and hand the *parsed* result to the render thread (or enqueue the
  parse as steps from (1)).
- If the data comes from an archive, read it sequentially in 1-4 MB chunks; that is also what lets the OS keep it
  in the page cache for the next visit.
- Avoid `mmap` of the whole archive on a platform with a small page cache; explicit chunked reads behave better.

**Acceptance test:** `read_ms` inside a stall stays under ~20 ms; total zone-load wall time may stay the same.

## 5. The bundled runtime's collector

The runtime is Serial-GC-only and rejects the standard HotSpot GC flags, so the only lever we have from outside is
the young generation (32 MB now, pauses 25-43 ms, one every ~20 s). Two things would help:

- expose a low-pause collector, or at least accept `-XX:+UseG1GC` / `-XX:MaxGCPauseMillis` / `-Xlog:gc` and HPROF;
- or document the recommended `MaxHeapSize` / `MaxNewSize` pair for a large-heap client.

A 25-43 ms pause every 20 s is a visible hitch at 60 fps; with G1 or ZGC it would be under 2 ms.

**Acceptance test:** the `GC(n) Pause` lines drop below 5 ms, or disappear from the frame histogram.

## 6. Ship a frame-time log

Everything in this report came from instrumentation the loader added from the outside. A `--frame-log=<file>`
option in the client, writing one line per slow frame, would make reports like this unnecessary:

```
frame=12345 interval_ms=33.4 cpu_ms=12.1 reads=0 read_ms=0 uploads=3 upload_ms=1.2 zone=Route 3
```

Even at a threshold of 20 ms, that is a handful of lines per minute, and it turns "it stutters" into a number
anyone can act on - on every platform, not just this one.

---

## Contact

Happy to test any build or patch on the console and return measurements - including whether a fix actually
moves the numbers above. The loader's instrumentation is small and can be pointed at any build.
