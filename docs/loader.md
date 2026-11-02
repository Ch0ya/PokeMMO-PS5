# The loader

`loader/` runs the official, unmodified PokeMMO Linux x86-64 client on a PS5. The design and most of the
code come from [PokeMMO-NX](https://github.com/Petit-Prince-dev/PokeMMO-NX) (Petit_Prince, MIT): an ELF
loader maps the client, and a Linux ABI layer answers the C library calls the client and its native libraries
make. Every file taken from PokeMMO-NX keeps its notice in the header and says what changed (see also
[CREDITS.md](../CREDITS.md)).

## Layout

| Folder | What | Origin |
|--------|------|--------|
| `loader/src/elf_*.c` | ELF layout, staging, relocations, symbol versions, mapping | PokeMMO-NX, adapted to x86-64 and in-place staging |
| `loader/src/linux_*.c` | the Linux ABI: files, stdio, printf, threads, sync, memory, sockets, epoll/eventfd, zlib, dlopen, traps | PokeMMO-NX, adapted; `linux_sync`, `linux_semaphore`, `linux_tls` and `linux_vm`'s backend rewritten |
| `loader/src/linux_stubs_x86_64.c` | trap/virtual stubs and `__tls_get_addr` entry (assembly) | rewritten for x86-64 |
| `loader/src/game.c` | maps the client and runs its `main` on a 16 MiB thread | PokeMMO-NX, simplified |
| `loader/src/pokeupdate.c`, `loader/src/sha256.c` | loader-49: updating the loader image itself, with a SHA-256 to verify it | new |
| `loader/include/platform.h` | the only system interface the adapters use | new |
| `loader/platform/ps5/` | the PS5 title: platform layer, self-checks, screen | new |
| `loader/platform/host/` | the same loader for a Linux PC | new |

### What changed from the Switch

| Topic | Switch (PokeMMO-NX) | PS5 (here) |
|-------|---------------------|------------|
| CPU, client binary | ARM64, `bin/linux/arm64/PokeMMO` | x86-64, `bin/linux/x64/PokeMMO` |
| Struct layouts | glibc AArch64 | glibc x86-64 (`stat` 144 bytes, packed `epoll_event`, 40-byte mutex, 56-byte thread attr, 4-byte mutex/cond attrs) |
| Thread-local storage of loaded libraries | TLS descriptors on TPIDR_EL0 | general-dynamic `__tls_get_addr`, per-thread blocks created on first use (`%fs` belongs to the PS5's libc) |
| Executable memory | Horizon code memory | written, then `mprotect` to read+execute (RWX pages fault on the PS5) |
| Large memory | Horizon heap | address space from `sceKernelReserveVirtualRange`, direct memory committed in 64 KiB blocks |
| Directory listing | newlib `opendir` | `open` + `getdents` (titles get EPERM from `opendir`), an index file as fallback (the retired installer wrote one) |
| DNS | Horizon `getaddrinfo` | `sceNetResolver` (`getaddrinfo` crashes PS5 titles) |
| Page size | 4 KiB | 16 KiB system pages; the guest still sees 4 KiB pages |

## Running it on a PC

```bash
make fetch-client      # once: the official client into private/ (never committed)
make host-run          # builds build/host/pokemmo-host and runs the client in it
```

Expected today: the client starts (logback, config, LWJGL, SDL3, libgdx), then stops at
"Error attempting to set up hardware accelerated graphics", because no display exists yet.

## Running it on the console

```bash
../pokemmo-ps5-buildenv/ps5env make image-loader LOG_HOST=<your PC's IP>
```

Install `dist/PPSA98001.ffpfsc` (or unzip `dist/pokemmo-ps5-loader-PPSA98001.zip` into `/data/homebrew/` for a
folder install); the console downloads the client itself. Put the game files in `private/roms` before building: both
targets copy every `.nds` / `.gba` there to `assets/roms`, and the build stops if there are none. A UDP log listener on
your PC receives the start-up trace while **PokeMMO** starts.

Since loader-8 the screen shows the project logo, one progress bar and a line of status ("Checking files",
"Installing PokeMMO", "Starting PokeMMO"). It stays up until the client is ready, then shows the horizontal
**Start Game / Settings / Exit** menu; the menu waits for a controller choice and hands the display over when
Start Game is chosen. When starting cannot go on, the bar turns red with a plain-language reason. **Triangle** opens
Settings at any time, including before the menu appears and on an error. Settings has two tabs:

| Tab | What it shows |
|-----|---------------|
| Startup details | Each start-up step (green OK, red Failed, blue Note, yellow Running, grey Not run), the version, the playing profile and the log file path |
| Log | The current log, the previous start's log, or ps5-opengl's driver log (Square switches), with line numbers and automatic refresh |

The steps are:

| Step | What it answers |
|------|-----------------|
| fs.list.app0, fs.list.roms | Does `getdents` list folders where `opendir` was refused, and are the preloaded ROMs readable? |
| client.update, client.map, client.start, client.end | The client updated/downloaded, mapped, started, and how it ended |

The former storage, module, HTTPS and ROM-reading probes answered their questions and no longer run. The log viewer
reads the last 64 KiB of the file, keeps up to 4096 lines, shows 24 at a time and refreshes once a second while open.

### In the game

| Input | Does |
|-------|------|
| Left stick | Moves like the d-pad: the direction it leans most, from halfway out (loader-28). While R2 is held and the stick leaves the dead zone, it steers the pointer instead (loader-60), and the d-pad stays quiet |
| L1 / R1 | Move the hotbar selection; held, page the hotbar (Page Up / Page Down), one page then repeats (loader-41) |
| D-pad up / down / left / right | The client's own controller d-pad, left in the gamepad state: its controller map binds KEY_UP/DOWN/LEFT/RIGHT to those buttons, so the d-pad navigates every one of the client's screens the same way the left stick does - windows, the battle command, the move grid, the shop and the PC (loader-48). Earlier releases sent arrow keys instead, which only the client's table widgets (the PC) listened to. The left stick uses the same buttons |
| Square | Use the selected hotbar slot: the loader presses its 1-9 (the client has no pad binding for it) |
| Circle | The client's own B: cancel, close the topmost window, step back in the battle UI. **Held for 700 ms it also sends Escape once** (loader-60), the keyboard's close, which reaches the panels B ignores - the catch screen draws only a corner X. On a panel that does answer B, a long hold therefore cancels twice (B closes it, Escape closes the window behind); tap there. A tap or an ordinary press-release sends only B |
| L2 | The client's own KEY_RING_FOCUS: brings up the party hotbar on the right (loader-59) |
| R2 | One button by how it is held (loader-60): a tap is Return; a still hold of 600 ms is Tab (step to the next field), one per repeat; a hold that moves the **left stick** brings up the pointer, steers it with the stick and clicks with **Cross**. While the pointer is up the game sees an idle controller (sticks and Cross cleared), and no Tab or Return is sent. loader-63: the pointer works in the client's window pixels, so a click lands under the arrow at every interface size (the touchpad no longer moves it; it has no right click) |
| Touchpad press | The client's own KEY_INVENTORY: the bag (loader-59). loader-63: sliding on the touchpad no longer moves the pointer |
| R3 | On-screen keyboard on/off (select the text field first) |
| Keyboard up: d-pad / left stick, Cross | Move, type the highlighted key |
| Keyboard up: Square, Triangle | Delete, space (held: repeat) |
| Keyboard up: L1 / R1 | Move the text cursor |
| Keyboard up: L2 / R2 | Symbols page / shift (once, caps lock, off) |
| Keyboard up: touchpad, Options | Tab (next field), Enter |
| Keyboard up: L3, Circle | Move the keyboard to the top or bottom, close it |
| Exit in the game's menu | Closes the title and goes back to the home screen (loader-21) |
| A link in the game | Opens in the PS5 web browser (the PS button brings the game back); if the browser cannot start, a box shows the address to type |

The loader's own drawing in the game is only the hotbar selection and the battle hints (loader-41/42). The client draws
the hotbar itself, so the loader paints a thin light-grey edge and faint wash over the selected slot in the client's own
design units (linux_sdl.h) and nothing else. Client 34702 put the bar at the **bottom centre** and the party hotbar on the
**right, vertically centred** (loader-59): those places are the client's own HUD anchors, not free coordinates, so the
loader writes `client.gui.hud.hotkeybar.anchor=centre:0,bottom:0` into `main.properties` at every start and
`client.gui.hud.party_frame_anchor=right:0,centre:0` into each character's `<account id>.properties`. The hotbar is
horizontal (`hotkeybar.vertical=false`) and the loader's marker uses the measured 430x53 frame: 39-unit slots, a 44-unit
pitch and a 32-unit pager border on the left (linux_sdl.h). The marker stays hidden until the client's own bar is on screen and the
player has worked it: the loader cannot know when the player is logged in (the client's window title is always
"PokeMMO"), so it reads a band of the picture back (`sdl.hotbarcells`) and needs the client's neutral dark slot greys
there. In a battle it reads the client's "B GO BACK" key-cap (`sdl.battle`) and draws a Circle over the B; it also reads
the bright-yellow frame around the selected move (loader-46) and keeps the pointer on that cell, so the client's own
Power and Accuracy summary appears automatically, with no L3 and no click. The probes read the picture back every 15th
frame and need two readings in a row before they change what is drawn. loader-61 puts each probe on its own frame (never
two in the same one) and makes all three stand aside while the left stick is deflected: a read-back waits for the frame the
game is still drawing, and that wait is most visible while the world scrolls. `sdl.probe.pace` logs the read-backs'
count, total time and slowest one every 600 frames.

The client places its own battle window: the loader cannot move it (it only draws on top of the game's frames). The
window is a fixed 758-unit-high panel in the client's design units, and its bottom edge sits at
`viewport height - client.gui.hud.chatframe.height - 127` — so the **chat box height is what places the battle scene**.
`applyHudDefaults` writes a 34-unit chat box at a 1080-high design viewport, which puts the panel at y 161..919: exactly
centred, with the panel's top and bottom margins equal. The same fraction of the viewport is written at every interface
size (17 at 2x), so it stays centred (loader-43/46).

### Interface size

The client's own UI Scaling slider stops at 200% (`client.gui.scale.ui_zoom2`), which is small on a 4K screen. The loader
stores 2.1 (loader-59), a slight enlargement of the HUD that the client accepts from its file, and for a larger interface
it gives the client a smaller window: **L1/R1 on the ready menu**, or Settings >
**Interface size** (100%-150%, a slight 5%-step enlargement kept in `/download0/root/interface-scale`), tells the SDL
layer to give the client a window that is the surface divided by the chosen scale (`linuxSdlSetRenderScale`,
`clientScreenSize` in `linux_sdl.c`). loader-62: the client's binds of framebuffer 0 are redirected to an RGBA8 texture of
that window size (depth Z32F), and `GL_SwapWindow` draws that texture over the whole surface
(`overlayDrawClientTexture`). Nothing is copied out of the window framebuffer, which ps5-opengl treats as a display target
and copies on the CPU - at 125% that copy measured ~330 ms a frame, a 3 fps game. `client.target=ON`/`=FAIL` in the log says
which path is in use; if the target cannot be made, the old copy path (`overlayUpscale`) is the fallback. The design
viewport is therefore `surface / (ui_zoom2 x scale)` design units. The hotbar's place no longer needs that number: its
bottom-centre anchor centres it in whatever viewport the client works in, and `applyUiScale` only records the zoom and the
window size. The marker and the battle probes follow the client's own `uiViewportScale` log line, so moving the client's UI
Scaling slider (or the client clamping 2.1 back to 2.0) does not drift them. The world is rendered at the smaller size and
scaled up with the interface, so the picture is softer the larger the size; 1x is the old behaviour on this 4K screen.
loader-63: mouse positions are the client's window pixels (`linuxSdlEventsSetRenderScale`), not the surface's, so the R2
pointer and the battle move parking click the element under the arrow; the drawn arrow is scaled to the surface.
loader-64: the display target starts each frame with a cleared depth buffer (the swap does not carry it over), while a
texture keeps the last frame's. The loader clears the client target's depth after present (`clientTargetClearDepth`,
logged as `client.depth` with the client's own clears), because the client clears colour every frame but not depth, and
its depth-tested sprites (the player, NPCs, some objects) disappeared without it. The client asked for no stencil, and
ps5-opengl cannot give a texture target one, so `client.stencil=on` in the log would mean a stencil effect is lost.

loader-65: `/app0/upscale-copy` in the title folder forces the old window-copy path above 100% (`client.target=OFF copy
path`), so the texture path and the copy path can be compared at the same interface size while the visual fault is being
tracked. The driver log copy in the title folder is refreshed once a minute while the title runs, because the driver
truncates its log at start: the startup copy only has its banner.

loader-66: a texture target is made indistinguishable from the window in the queries an engine asks: the driver answers 0
for `GL_DEPTH_BITS`/`GL_STENCIL_BITS` there while the window answers 32/8, and `GL_DOUBLEBUFFER`/`GL_DRAW_BUFFER`/
`GL_READ_BUFFER` differ too. The loader reports the window's answers while its target is bound (an engine that saw 0 took
its "no depth buffer" path and stopped drawing the player, NPCs and some objects). The client's window is also rounded to
an even width and height, because its interface is drawn at 2x: at 105% that turns 3657x2057 into 3656x2056, so the design
viewport stays whole. Every interface size from 105% to 150% goes through the same path.

loader-67: the texture path's only GL error was `GL_INVALID_OPERATION` from the client selecting the window's `GL_BACK`
buffer (the window path logs none). `glDrawBuffer`/`glReadBuffer`/`glDrawBuffers` are mapped to the target's colour
attachment, so a pass that selects its buffer and then checks for errors is not abandoned. The touchpad click's edges are
logged as `sdl.touchpad=press/release`, because the client stopped opening the bag from it.

loader-68: the client's picture is copied into a texture with `glCopyTexSubImage2D` before it is drawn
(`overlayDrawClientFramebuffer`) instead of sampling the render target directly. The copy goes through the driver's blit
path (a render target, not the display target) and makes the driver finish the client's queued draws first - with deferred
draw batching, sampling the target directly lost the frame's last draws, the character models. GL errors are also drained
at the start of present and reported as `when=client`, so a load line says whether the client's own calls fail.

loader-69: the real window's back buffer is cleared to magenta after present and a small block of it is read before the
upscale (`client.window probes=.. drawn=..`). If the client drew there itself, some bind of framebuffer 0 bypasses the
redirect (an ARB/OES alias), and the upscale then covers that picture - which is what the character models did. The ARB and
OES bind aliases are redirected like the core call.

### Preloaded ROMs

The ROMs are part of the title. `make loader` copies every `.nds` / `.gba` in `private/roms` (default `ROMS_DIR`;
git-ignored) into `assets/roms`, and the client is started with that folder mounted as `/game/roms`. Nothing is
scanned, identified or uploaded at runtime, and there is no ROM screen or upload server: the client reads the files
directly. `fs.list.roms` only checks that the folder can be listed. The build fails with a clear message when
`private/roms` has no game files.

### Ready menu, Settings and logs

Once the client is installed and the update question is answered, the loading screen shows a horizontal
**Start Game / Settings / Exit** menu (left/right or up/down, Cross to choose). **L1/R1** change the interface size
without opening Settings; the line under the menu shows the current size. Start Game creates the client thread;
Settings opens the Settings screen (Startup details, Log, Check for updates, Interface size); Exit backs up the settings
copy and calls `platformQuit()`. Triangle opens the
same Settings screen at any other time. Circle or Triangle closes it. While Settings > Log is up, up/down scroll one
line, L1/R1 one page, and Square switches between this start's log, the previous start's log and ps5-opengl's driver
log. The screen no longer depends on holding a button, so the log stays readable at all times. **Settings > Check for
updates** asks GitHub about newer releases and, when a loader update source is configured, offers the loader image
update again there (Cross).

The footer shows **R3 (on-screen keyboard)** at the bottom right and the loader's own revision (**Rev v1.0**) in the
centre. Screens that have their own controller hint (Settings, or the loading screen's "Settings" prompt before the
menu) show it just above the footer. The ready menu shows its own line under the menu: the current interface size and
that L1/R1 change it.

The ready screen is laid out on one vertical rhythm: the logo, the **PokeMMO** wordmark, the progress bar and the menu
have equal gaps and the group is centred above the footer. The art is Gengar: the badge is Gengar's
official-artwork silhouette in near-black on a deep purple disc with a pale lavender rim, and the screen, the home icon
and the home background all use that purple/black palette (the name stays **PokeMMO**). `icon0.png`, `background.dds`
(pic0/pic1) and the logo inside `overlay_assets.c` all derive from `assets/branding/logo-source.png`, so replacing that
one image and the palette constants changes the whole look. The loader does not print the PS5 profile
name; PokeMMO asks for the login itself once it has loaded. Settings > Startup details shows the PokeMMO client version
and revision, the loader revision and the build identity.

The loader revision comes from `REVISION` at the repository root: a base version and the commit its count starts from
(`1.0 <commit>`). Every commit after that commit adds one (`v1.0`, `v1.0.1`, ...); uncommitted builds keep the same
revision and are told apart by the build identity in Settings. Bump the base line to start a new series.

### Rendering diagnostics

ps5-opengl's context is created with the size the client asked for (8-bit RGBA), a 24-bit depth buffer by default,
and the client's stencil and multisampling requests when it sets them; the log records the requested and the chosen
EGL config, and `SDL_GL_GetAttribute` then answers what the window really has. On the first context the loader also
logs `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, the GLSL version and the real attachment bits, texture limits and
vertex attribute count.

While the client runs, `sdl.GL_Error` reports every GL error code it leaves behind (the first three occurrences and
then every 600th), including `GL_OUT_OF_MEMORY`, which is what a failed sprite/texture upload looks like. A GL entry
point the driver does not provide is logged as `gl.proc.missing`; a model or texture file the client cannot open is
logged as `files.open.failed` with its path; a shader that fails to compile or a program that
fails to link is logged with the driver's own message as `gl.shader.compile=FAIL` / `gl.program.link=FAIL` (the first
four successful compiles and links are noted too, so their absence means the client never compiled a shader). Frames
whose interval passed 500 ms (or whose `eglSwapBuffers` took over 250 ms) are logged as `sdl.frame.stall`, with a
`sdl.frame.summary` every 600 frames. `sdl.frame.pace` counts the intervals over 17/25/33/50/100 ms in each 600-frame
window, and `sdl.probe.pace` the pad probes' read-backs (count, total and slowest), so judder can be told apart from a
stalled frame and from a probe that costs a vblank. `game.state` now also reports ps5-opengl's live native heap. Together
they show whether black sprites, shadow-only NPCs and zone-change freezes come from driver errors, missing entry points,
failed shaders, memory pressure or a stalled frame. ps5-opengl's own log is the third source in Settings > Log, and it also
receives the client's stdout/stderr, where libgdx prints asset and shader errors. loader-64 also copies the last 4 MiB of it
to `/app0/ps5-opengl.log` at every start (it lives on `/download0`, which FTP does not expose), so the driver's view of a
rendering fault can be fetched next to `pokemmo.log`.

The shipped ps5-opengl stores depth only as 32-bit float; its packed depth/stencil and depth-texture paths are compiled
out. A framebuffer that asks for `GL_DEPTH_COMPONENT16` (libGDX's default) therefore fails completeness with
`GL_FRAMEBUFFER_UNSUPPORTED`, which PokeMMO turns into "Frame buffer couldn't be constructed" and a fatal render error.
The loader stores 16/24/32-bit depth as 32-bit float instead. `gl.fbo.renderbuffer` / `gl.fbo.texture2d` (and the storage
and multisample variants) record what was requested, what was stored and the sample count; `gl.fbo.status` records a failed
completeness check, and the first failures also dump every attachment (`gl.fbo.attach`, with format, size and samples) and
`gl.fbo.recent`, the last sixteen storage requests in front of the check.

A pack of repairs runs before the failure reaches the game, and each one is logged as `gl.fbo.repair` /
`gl.fbo.repaired`. Packed depth/stencil requests (`GL_DEPTH24_STENCIL8` and friends) are stored as depth-only 32-bit float:
the driver rejects the packed format outright, so the attachment would otherwise hold no storage at all, and a usable depth
buffer with no stencil is closer to what the game asked for than a fatal error. When the failure is depth and stencil on
two different objects, the separate stencil is detached (Gallium has no separate stencil) and the depth object is kept.
A target with one colour attachment and no depth at all is given a private depth renderbuffer of the same size and checked
again - the crash that prompted this (loader-60) was exactly that shape, a 320x256 RGBA8 colour-only target, and the dump
named no invalid attachment. Those private renderbuffers are kept per size, never freed, and counted in the log. Every 600
frames `gl.objects` reports live texture, renderbuffer, framebuffer and buffer counts plus the cumulative texture upload
size, so a leaked object or an over-eager render target is visible next to ps5-opengl's `resource-map-failed` (ENOMEM)
lines.

### Client slots (loader-18)

`/download0/root/slots/a` and `slots/b` each hold a complete client (a `.pokemmo-complete` marker names its revision);
`slots/state` says which is active. A new client (download or developer copy) goes into the other slot, which becomes
active "on trial". The trial ends when the game shows its first picture; the previous client is then deleted (loader-21), so
the second copy only exists while an update is on trial. If the game ends before that, or a start finds
a trial still open (the previous start crashed), the loader goes back to the previous slot, says so on the loading
screen, and does not offer that revision again until a newer one is published. The settings live in
`/download0/root/users/<console profile id>/config` (loader-34; `shared/config` before, removed on the first start), mounted over the
game's `/game/config`: each PS5 profile has its own, used by either slot. The single client
folder of loader-17 and earlier (`/download0/root/game`) becomes slot a on the first start.

### Title ID (loader-24)

The title is `PPSA98001`. Retail PS5 games use `PPSA` + five digits and are numbered far lower; PS5 homebrew settles in
`PPSA99xxx` (the [homebrew.page](https://homebrew.page/) catalog lists none in `PPSA98xxx`). A non-`PPSA` id
(`POKEMMOP5`) did not work on a console. Earlier builds used `PPSA27166`, which a retail game could also get, so a
`PPSA98001` install is a new title: its own storage, client download and preloaded ROMs.

### Folder or image install (loader-23)

At start the loader checks whether the title folder (`/app0`) is writable. In a folder install it is, and nothing changes.
In an image install (`.ffpfsc`, mounted read-only) the log is `/download0/root/pokemmo.log`; a folder install keeps it
at `/app0/pokemmo.log`. Either way the previous start's log stays next to it as `pokemmo-previous.log` and both are
readable in **Settings > Log**. The ROMs live read-only in `assets/roms` inside the title, so no upload step is needed.
The settings backup copy in the title folder only exists in a folder install. A folder install is the layout the loader
prefers for updates: the title can replace its own executable in place (loader-52, below), where an image install has to
stage a whole new image and have ShadowMountPlus pick it up. To keep one install and one only, a folder install also
deletes any leftover image for the title at every start (loader-54): an `.ffpfsc` beside the folder is a second
candidate ShadowMountPlus has to recognise and ignore as a "duplicate" at every scan.

### Client updates (loader-15)

At every start the loader asks PokeMMO's download server whether the published client changed (one HEAD request; the
ETag of the installed revision is kept in `/download0/root/update-etag`). When it did, the zip's index and its
`revision.txt` are read with range requests; a newer revision is offered on the loading screen (Cross: download,
Circle: skip; download after six seconds). With no client installed it is downloaded straight away. Only what a PS5
runs is fetched (about 93 MB of the 271 MB zip, in two requests), unpacked as it arrives, every file checked against its
CRC-32, into the client slot that is not in use (below). A dropped connection resumes where it stopped. HTTPS is the system's own (`libSceHttp`, certificates checked).
The same updater code also builds for a PC (`make host-loader`), which is how the resume path is exercised.

### Loader updates (loader-49; folder installs loader-52)

The title runs either from the `.ffpfsc` image ShadowMountPlus mounts as the title (an **image install**) or from a
folder, `/data/homebrew/PPSA98001` (a **folder install**), so replacing the build used to mean staging it from a PC over
FTP. It can now update **itself** from a private GitHub release. A small config file on the console turns the source on:

```
# /data/pokeupdate.ini
repo=OWNER/REPO
asset=
token=github_pat_xxx
```

`repo` and `token` (a fine-grained, read-only token) are required; `asset` may be empty, and then the first release
asset whose name ends in `.ffpfsc` (image install) or `eboot.bin` (folder install) is used. Keys are case-insensitive and
`#` comments and blank lines are ignored. The token lives only in that file: it is never compiled in, never logged and
never added to an error string. **Delete the file to switch the check off**: without it the loader sends nothing and
shows no prompt, exactly as before.

At start (in the background, like the GitHub release banner) `pokeupdateCheck` asks
`GET https://api.github.com/repos/<repo>/releases/latest` with the token and compares the release's `tag_name` against
the running revision with `releaseCompare`. The body is read with a 256 KiB cap and parsed by hand, the same `strstr`
style as `release_check.c` (this tree has no JSON library); from the chosen asset it reads the API `url`, `size` and
`digest`. When the release is newer, the ready menu shows the same question style as the client update:
**"An update is available: Rev 1.1.33 (265 MB). Would you like to update?"** with **[Yes]** (Cross) and **[No]**
(Circle). The menu is suppressed while the question is up, so the prompt's buttons cannot also choose Start Game. No
dismisses it for this start.

**Yes** runs `pokeupdateInstall` on its own thread. The asset's API url is fetched with
`Accept: application/octet-stream` (a private, authenticated download; GitHub's 302 to a signed url is followed) and
streamed to `/data/homebrew/PPSA98001.ffpfsc.new` in 64 KiB chunks while `sha256.c` hashes it, so the 265 MB image is
never held in memory. The size and, when GitHub publishes one, the SHA-256 are checked; on any failure the `.new` file
is deleted, so a second image never survives for ShadowMountPlus to mount. On success the file is `rename()`d over
`/data/homebrew/PPSA98001.ffpfsc` and the same uppercase-hex marker the PC tool writes goes to
`/data/homebrew/.pokemmo-image.sha`.

Finally the loader writes `/data/shadowmount/manual.lst` (the image's path, then a `# staged <digest>` comment).
ShadowMountPlus caches its mount of the image between launches and only rescans when that file **changes content**; an
empty write over an empty file is invisible and the console would keep running the previous build. The installed
digest always differs from the last one, so the file visibly changes and the rescan fires (loader-51). The image has
changed under a running title, so the player is told to close the title and start it again; a failure says that the
update was not installed and that the current build will start, with the technical reason in **Settings > Log**, and can
be retried from **Settings > Check for updates**.

A **folder install** (loader-52) skips all of that. `/app0` is a writable nullfs of the source folder, so the release
asset is just the executable - `eboot.bin` - and the update is a checked download renamed over `/app0/eboot.bin`: no
image, no staging marker, no ShadowMountPlus rescan and nothing to restart. The running process keeps its own copy and
the new build runs the next time the game is launched, so the prompt says only "Close PokeMMO, then start it again".
Before the rename the file is checked to be a signed executable (an FSELF, `4f 15 3d 1d`; loader-56), so a release that
carried an image, or the decrypted ELF the console's storage layer hands out, cannot become the executable.

That check matters because only the FSELF form starts from `/app0/eboot.bin`. A bare ELF there - the old loader-52/53
asset - mounts, logs ShadowMount+'s `launch mount ready`, and then never reaches the sandbox: the title simply does not
start, with no error anywhere. Publishing and installing the FSELF is what keeps the folder install launchable.

loader-58 makes the outcome of an update visible to the player, in both directions. The revision that last started is
recorded in `/data/homebrew/.pokemmo-running`, so the first start of a different build shows a small **"Updated to Rev
1.1.45"** banner on the loading screen - which is the start that actually proves the update took - and the record is
rewritten on every start so the banner appears once. Comparing revisions rather than trusting a note the installer wrote
also covers an update that arrived another way (an older loader, a manual deploy), and it is what makes the end of the
prompt honest: the previous build said "update failed" while the next start silently ran the new one. A failed install
now reads **"Update not installed. PokeMMO will start your current version (details in Settings > Log)."**
instead of putting the raw download error on screen, since the installed executable is untouched and still starts.

The publishing half uploads the built `eboot.bin` from the title folder exactly as the build produced it - i.e. the
FSELF - and refuses to publish anything that does not start with the FSELF magic. For an image install it publishes the
`.ffpfsc` named by `dist/latest.txt`. It tags a release `v<loader revision>` (from the generated `pokemmo_version.h`) and
writes `/data/pokeupdate.ini` on the console over FTP. The token comes from `GH_TOKEN` / `GITHUB_TOKEN` and the repo from
`--repo` / `POKEMMO_REPO`; the token is never printed, and the tool is a no-op with a clear message when either is
missing. It is maintainer tooling and is not part of this repository.

Note for any manual FTP deploy: ftpsrv runs with **SELF transfer mode on** by default, which decrypts an FSELF while it
is stored, so an upload of `eboot.bin` lands as an unlaunchable ELF. Send the `SITE SELF` command on the FTP control
connection first to transfer the file verbatim (that is what `--quote SELF` does in the other console scripts).

The game's settings live in `config/main.properties` in the title storage, which FTP cannot see. The loader copies the
file to `/data/homebrew/PPSA98001/settings/main.properties` at start and within a minute of any change (loader-11).
Before the game starts, the loader applies `assets/settings/defaults.properties` (fullscreen 1920x1080, 60 fps, UI scale
2.1, the controller layout): in full on a new install, and once per `DEFAULTS_VERSION` on an existing one, so later changes in the game stay
(loader-12). Raise `DEFAULTS_VERSION` to push changed defaults to everyone once. `applyUiScale` then writes the
interface's own values at every start (`ui_zoom2=2.1` and the window size the interface size gives the client,
loader-45/59), `applyHudDefaults` keeps the hotbar anchored bottom centre and the chat box height, and
`applyPartyFrameAnchor` centres the party hotbar on the right in each character file.

The keyboard (`loader/src/osk.c`) and the link box (`loader/src/link_box.c`) are the loader's own, drawn over the
game with `loader/src/overlay.c`; nothing typed is kept. `make overlay-preview` draws them (and the loading screen,
ready menu, Settings and log) on a PC with Mesa into `build/overlay-preview/`, and checks what the keyboard types.

Nothing in the checks can end the title: faults are caught, and slow calls get a time limit. A fault in the
client itself is logged with its address (`FATAL signal ...`) and that thread is parked, so the screen and
the log stay up.
