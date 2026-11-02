// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// What this project draws over a picture: the loading screen, the on-screen keyboard and the link box, batched and drawn
// with the overlay's own shader on whichever context is current; the game's state is saved and restored around it.
// Coordinates are a virtual 1920x1080 screen, colors are 0xRRGGBBAA.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define OVERLAY_WIDTH 1920.0f
#define OVERLAY_HEIGHT 1080.0f

bool overlayBegin(unsigned width, unsigned height);  // false (and nothing to end) when OpenGL cannot be used
void overlayEnd(void);
void overlayClear(uint32_t rgba);  // the whole picture, for screens of our own
void overlayRect(float x, float y, float w, float h, uint32_t rgba);
void overlayFrame(float x, float y, float w, float h, float thickness, uint32_t rgba);  // outline inside the rectangle
// Text in DejaVu Sans; `size` is the line height in pixels, y the top of the line. Codes 1-4 are the controller's face
// buttons (overlay_assets.h). Returns the width drawn.
float overlayText(float x, float y, const char *text, float size, uint32_t rgba);
float overlayTextWidth(const char *text, float size);
float overlayTextCentered(float centre_x, float y, const char *text, float size, uint32_t rgba);
void overlayLogo(float x, float y, float height);  // the project logo, `height` tall, width kept in proportion
// The context the overlay's objects were made in is gone (a new one may get its address): make them again next time.
void overlayContextLost(void);
// Whether the picture drawn so far shows anything but black (reads a few pixels back: call it seldom, outside Begin/End).
bool overlayPictureHasContent(unsigned width, unsigned height);
// A block of the picture on screen, for the pad's own probes: x, y, w and h are in the same virtual 1920x1080 space as
// the drawing (y from the top). `pixels` gets the block as RGBA quads, its first row at the top; `capacity` is the size
// of `pixels` in bytes. Returns false when the block does not fit or nothing could be read. Call it seldom.
bool overlayReadBack(unsigned width, unsigned height, float x, float y, float w, float h, uint8_t *pixels, unsigned capacity,
                     unsigned *out_width, unsigned *out_height);
// Totals for the read-backs since the last call, then the counters start again: `ns` is their whole time, `max_ms` the
// slowest one.
void overlayReadBackStats(unsigned *calls, unsigned long long *ns, unsigned *max_ms);
// loader-82: the same block read, split in two so the caller never waits for the frame the game is drawing. The driver
// copies the block into this slot's own pixel-pack buffer while it works; the CPU collects it on a later call, when the
// copy has finished. `overlayReadBackQueue` starts it (false while the previous block is still in flight, or when the
// block does not fit `capacity`), `overlayReadBackFetch` collects it (true when `pixels` holds a fresh block, first row
// at the top, with the same geometry as `overlayReadBack`). Both fall back to no work when the driver has no pixel-pack
// buffers, so a caller can keep `overlayReadBack` as its fallback.
typedef struct {
    unsigned buffer;    // the slot's own pixel-pack buffer (0: not made yet)
    void *fence;        // the fence of the copy in flight (NULL: nothing in flight)
    unsigned frame;     // the frame the copy was queued on
    unsigned generation; // the overlay context generation the objects belong to
    unsigned bytes, block_width, block_height;
} OverlayReadBack;
bool overlayReadBackQueue(OverlayReadBack *slot, unsigned width, unsigned height, float x, float y, float w, float h,
                          unsigned capacity, unsigned frame);
bool overlayReadBackFetch(OverlayReadBack *slot, uint8_t *pixels, unsigned capacity, unsigned *out_width,
                          unsigned *out_height);
// Text cut to fit `max_width`, with "..." where it was cut.
void overlayTextFit(float x, float y, const char *text, float size, float max_width, uint32_t rgba);
// loader-45: the client's picture when the loader upscales it (a render scale above 1). The window the client was given
// is the surface divided by the scale; its picture sits in the bottom-left `source_width` x `source_height` pixels of the
// framebuffer. Copy that block (glCopyTexSubImage2D) and draw it over the whole `width` x `height` surface, so the
// client's smaller picture fills the screen (the game's pixels are soft, the interface is the size the player asked for).
// Called outside Begin/End, before the loader's own overlay for the frame.
void overlayUpscale(unsigned width, unsigned height, unsigned source_width, unsigned source_height);
// loader-62: draw a texture the client itself rendered into (its whole frame) over the surface. Cheaper than
// overlayUpscale: nothing is copied out of the window framebuffer, so the picture never leaves the GPU.
void overlayDrawClientTexture(unsigned texture, unsigned width, unsigned height);
// loader-68: the same, when the client drew into a framebuffer: copy it to a texture first (a render-target blit, which
// also forces the client's queued draws to finish) and draw that.
bool overlayCaptureFramebuffer(unsigned framebuffer, unsigned width, unsigned height);
// loader-70: capture the current read framebuffer (the client's own surface) into a texture, then draw it over the surface.
// The capture is a render-target blit; the draw happens after the caller has made the window current.
bool overlayCaptureSurface(unsigned width, unsigned height);
void overlayDrawCapturedSurface(unsigned width, unsigned height);
