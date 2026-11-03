// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_sdl.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO contributors.
// Changes: the window is ps5-opengl's fixed EGL surface (native window 0); no docking resize; platform time; the loading screen hands
// over the display (linuxSdlSetDisplayAcquire); links open the link box; the keyboard is the loader's own (osk.c).
#include "link_box.h"
#include "osk.h"
#include "overlay.h"
#include "linux_sdl.h"
#include "diagnostics.h"
#include "linux_abi.h"
#include "linux_audio.h"
#include "linux_file_picker.h"
#include "linux_format.h"
#include "linux_files.h"
#include "linux_net_native.h"
#include "linux_threads.h"
#include "linux_sdl_cursor.h"
#include "linux_sdl_events.h"
#include "linux_sdl_io.h"
#include "linux_sdl_keys.h"
#include "linux_sdl_input.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform.h"

static atomic_uint call_counter, unknown_counter, frame_counter;
static atomic_bool stop_requested;
static unsigned logged_unknown;
static void trace(const char *format, ...) {
    static atomic_uint lines;
    if (atomic_fetch_add(&lines, 1) > 600) return;
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}
void linuxSdlStopRequest(void) { atomic_store(&stop_requested, true); }

// ---- tiny helpers for the many functions that only need a fixed answer -------------------------------------------------
static bool sdlTrue(void) { return true; }
static bool sdlFalse(void) { return false; }
static uintptr_t sdlNull(void) { return 0; }
static void sdlVoid(void) {}
static float sdlOneFloat(void) { return 1.0f; }

// ---- init, errors, hints, version -----------------------------------------------------------------------------------------
static char error_text[256];
static bool sdlInit(uint32_t flags) {
    trace("sdl.Init flags=0x%x", flags);
    return true;
}
static bool sdlInitSubSystem(uint32_t flags) {
    trace("sdl.InitSubSystem flags=0x%x", flags);
    return true;
}
static uint32_t sdlWasInit(uint32_t flags) { return flags; }
static const char *sdlGetError(void) { return error_text; }
static bool sdlClearError(void) {
    error_text[0] = 0;
    return true;
}
static bool sdlSetError(const char *format, ...) {
    va_list args;
    va_start(args, format);
    linuxFormatVsnprintf(error_text, sizeof(error_text), format, args);
    va_end(args);
    trace("sdl.SetError %s", error_text);
    return false;
}
static bool sdlSetHint(const char *name, const char *value) {
    trace("sdl.SetHint %s=%s", name ? name : "?", value ? value : "?");
    return true;
}
static bool sdlSetHintWithPriority(const char *name, const char *value, int priority) {
    (void)priority;
    return sdlSetHint(name, value);
}
static int sdlGetVersion(void) { return 3004000; }
static const char *sdlGetRevision(void) { return "pokemmo-virtual"; }
static const char *sdlGetPlatform(void) { return "Linux"; }
static void *sdlMalloc(size_t bytes) { return linuxAbiMalloc(bytes); }
static void *sdlCalloc(size_t count, size_t bytes) { return linuxAbiCalloc(count, bytes); }
static void *sdlRealloc(void *memory, size_t bytes) { return linuxAbiRealloc(memory, bytes); }
static void sdlFree(void *memory) { linuxAbiFree(memory); }

// ---- properties (name -> number/pointer/boolean/string, identified by small integers) -------------------------------
#define MAX_PROPERTY_SETS 16u
#define MAX_PROPERTIES 24u
typedef struct {
    char name[64];
    uint64_t value;
} Property;
typedef struct {
    bool used;
    Property items[MAX_PROPERTIES];
    unsigned count;
} PropertySet;
static PropertySet property_sets[MAX_PROPERTY_SETS];
static uint32_t sdlCreateProperties(void) {
    for (unsigned i = 1; i < MAX_PROPERTY_SETS; ++i)
        if (!property_sets[i].used) {
            memset(&property_sets[i], 0, sizeof(property_sets[i]));
            property_sets[i].used = true;
            return i;
        }
    return 0;
}
static void sdlDestroyProperties(uint32_t id) {
    if (id && id < MAX_PROPERTY_SETS) property_sets[id].used = false;
}
static Property *findProperty(uint32_t id, const char *name, bool create) {
    if (!id || id >= MAX_PROPERTY_SETS || !property_sets[id].used || !name) return NULL;
    PropertySet *set = &property_sets[id];
    for (unsigned i = 0; i < set->count; ++i)
        if (!strcmp(set->items[i].name, name)) return &set->items[i];
    if (!create || set->count == MAX_PROPERTIES || strlen(name) >= sizeof(set->items[0].name)) return NULL;
    Property *item = &set->items[set->count++];
    strcpy(item->name, name);
    return item;
}
static bool sdlSetNumberProperty(uint32_t id, const char *name, int64_t value) {
    Property *p = findProperty(id, name, true);
    if (p) p->value = (uint64_t)value;
    return p != NULL;
}
static bool sdlSetPointerProperty(uint32_t id, const char *name, void *value) {
    Property *p = findProperty(id, name, true);
    if (p) p->value = (uintptr_t)value;
    return p != NULL;
}
static bool sdlSetBooleanProperty(uint32_t id, const char *name, bool value) {
    Property *p = findProperty(id, name, true);
    if (p) p->value = value;
    return p != NULL;
}
static bool sdlSetStringProperty(uint32_t id, const char *name, const char *value) {
    Property *p = findProperty(id, name, true);
    if (p) p->value = (uintptr_t)value;
    return p != NULL;
}
static int64_t sdlGetNumberProperty(uint32_t id, const char *name, int64_t fallback) {
    Property *p = findProperty(id, name, false);
    return p ? (int64_t)p->value : fallback;
}
static void *sdlGetPointerProperty(uint32_t id, const char *name, void *fallback) {
    Property *p = findProperty(id, name, false);
    return p ? (void *)(uintptr_t)p->value : fallback;
}
static bool sdlGetBooleanProperty(uint32_t id, const char *name, bool fallback) {
    Property *p = findProperty(id, name, false);
    return p ? p->value != 0 : fallback;
}

// ---- displays and windows -----------------------------------------------------------------------------------------------------
typedef struct {
    int x, y, w, h;
} Rect;
typedef struct {
    uint32_t display, format;
    int w, h;
    float density, refresh;
    int numerator, denominator;
    void *internal;
} DisplayMode;
_Static_assert(sizeof(DisplayMode) == 40, "SDL_DisplayMode");
#define PIXELFORMAT_XRGB8888 0x16161804u
static DisplayMode display_mode;
// The PS5 window is ps5-opengl's fixed full-screen surface (1080p unless the title's display metadata says otherwise); its size is
// known once the surface exists.
static unsigned surface_width = 1920, surface_height = 1080;
// loader-45: how much larger the interface is drawn than the client's own 200% cap allows. The client is given a window
// (clientScreenSize) that is the surface divided by this, and the picture it draws there is scaled over the whole surface
// on present (overlayUpscale). 1.0 is off: the client draws into the whole surface, exactly as before.  loader-47: the
// factor is continuous (100%..150% in 5% steps), so a slight enlargement stays cheap: the smaller the factor, the larger
// the block the upscale copies each frame.
#define RENDER_SCALE_MIN 1.0f
#define RENDER_SCALE_MAX 1.5f
static float render_scale = 1.0f;
void linuxSdlSetRenderScale(float scale) {
    render_scale = scale < RENDER_SCALE_MIN ? RENDER_SCALE_MIN : scale > RENDER_SCALE_MAX ? RENDER_SCALE_MAX : scale;
    linuxSdlEventsSetRenderScale(render_scale);  // loader-63: SDL mouse positions are the client's window, not the surface
}
static bool upscale_copy;  // loader-65: the old window-copy path, forced by /app0/upscale-copy for an A/B comparison
void linuxSdlSetUpscaleCopy(bool on) { upscale_copy = on; }
float linuxSdlRenderScale(void) { return render_scale; }
// loader-46: the client's own UI Scaling slider can still be moved while it runs.  It logs the resulting UI scale
// ("Initializing ui viewport ... uiViewportScale: 0.5"), so the loader follows it instead of assuming 2.0: the marker and
// the battle hints then stay on the client's own layout if the slider is moved.  The game.c console sink passes every
// client line here before it is logged.
static _Atomic float client_ui_scale = 2.0f;
static float padGuiScale(void) {
    float scale = atomic_load(&client_ui_scale);
    return scale > 0.5f && scale < 6.0f ? scale : 2.0f;
}
void linuxSdlClientLine(const char *line) {
    if (!line) return;
    const char *key = strstr(line, "uiViewportScale:");
    if (!key) return;
    double value = strtod(key + 16, NULL);  // 1 / UI scale (0.5 at 200%)
    if (value <= 0.05 || value > 2.0) return;
    float scale = (float)(1.0 / value);
    if (scale < 0.5f || scale > 6.0f) return;
    float before = atomic_load(&client_ui_scale);
    atomic_store(&client_ui_scale, scale);
    if (fabsf(scale - before) > 0.01f) diagnosticsTrace("sdl.clientuiscale=%.3f", scale);
}
static void screenSize(unsigned *width, unsigned *height) {
    *width = surface_width;
    *height = surface_height;
}
// The window the client is told it has (SDL_GetWindowSize and SDL_GetWindowSizeInPixels), and the size it therefore
// renders at. With a render scale of 1 this is the surface; above 1 it is the surface the client is handed, which the
// loader then scales back up over the surface.
static void clientScreenSize(unsigned *width, unsigned *height) {
    unsigned w = (unsigned)((float)surface_width / render_scale + 0.5f);
    unsigned h = (unsigned)((float)surface_height / render_scale + 0.5f);
    // loader-66: the client's interface is drawn at 2x (its zoom clamps to 2.0), so an even window makes the design
    // viewport whole. 105% would otherwise be 3657x2057 and its 1828.5x1028.5 viewport lands text and effects on half pixels.
    *width = w & ~1u;
    *height = h & ~1u;
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;
}
static void refreshMode(void) {
    unsigned w, h;
    clientScreenSize(&w, &h);
    display_mode = (DisplayMode){1, PIXELFORMAT_XRGB8888, (int)w, (int)h, 1.0f, 60.0f, 60, 1, NULL};
}
static int sdlGetNumVideoDrivers(void) { return 1; }
static const char *sdlGetVideoDriver(int index) { return index == 0 ? "x11" : NULL; }  // the game only accepts the Linux compositors
static const char *sdlGetCurrentVideoDriver(void) { return "x11"; }
static uint32_t *sdlGetDisplays(int *count) {
    uint32_t *list = linuxAbiCalloc(2, sizeof(uint32_t));
    if (list) list[0] = 1;
    if (count) *count = list ? 1 : 0;
    return list;
}
static uint32_t sdlGetPrimaryDisplay(void) { return 1; }
static const char *sdlGetDisplayName(uint32_t display) {
    (void)display;
    return "PlayStation 5";
}
static bool sdlGetDisplayBounds(uint32_t display, Rect *rect) {
    (void)display;
    unsigned w, h;
    screenSize(&w, &h);
    if (rect) *rect = (Rect){0, 0, (int)w, (int)h};
    return true;
}
static const DisplayMode *sdlGetDisplayMode(uint32_t display) {
    (void)display;
    refreshMode();
    return &display_mode;
}
static DisplayMode **sdlGetFullscreenDisplayModes(uint32_t display, int *count) {
    (void)display;
    refreshMode();
    DisplayMode **list = linuxAbiCalloc(2, sizeof(DisplayMode *));
    if (list) list[0] = &display_mode;
    if (count) *count = list ? 1 : 0;
    return list;
}
static uint32_t sdlGetDisplayForWindow(void *window) {
    (void)window;
    return 1;
}

typedef struct {
    bool used;
    uint32_t id;
    int w, h;
    uint64_t flags;
    char title[64];
} Window;
#define MAX_WINDOWS 4u
static Window windows[MAX_WINDOWS];
static unsigned windows_created, next_window_id = 1;
static void *newWindow(const char *title, int w, int h, uint64_t flags) {
    (void)w;
    (void)h;
    for (unsigned i = 0; i < MAX_WINDOWS; ++i)
        if (!windows[i].used) {
            unsigned width, height;
            clientScreenSize(&width, &height);
            windows[i] = (Window){true, next_window_id++, (int)width, (int)height, flags, {0}};
            snprintf(windows[i].title, sizeof(windows[i].title), "%s", title ? title : "");
            ++windows_created;
            trace("sdl.window id=%u requested=%dx%d actual=%ux%u flags=0x%llx title=%s", windows[i].id, w, h, width, height, (unsigned long long)flags,
                  windows[i].title);
            return &windows[i];
        }
    return NULL;
}
static void *sdlCreateWindow(const char *title, int w, int h, uint64_t flags) { return newWindow(title, w, h, flags); }
static void *sdlCreateWindowWithProperties(uint32_t properties) {
    const char *title = sdlGetPointerProperty(properties, "SDL_PROP_WINDOW_CREATE_TITLE_STRING", NULL);
    return newWindow(title, (int)sdlGetNumberProperty(properties, "SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER", 1280),
                     (int)sdlGetNumberProperty(properties, "SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER", 720), 0x2);
}
static Window *checked(void *handle) {
    Window *w = handle;
    return (w >= windows && w < windows + MAX_WINDOWS && w->used) ? w : NULL;
}
static void sdlDestroyWindow(void *handle) {
    Window *w = checked(handle);
    if (w) w->used = false;
}
static bool sdlGetWindowSize(void *handle, int *width, int *height) {
    Window *w = checked(handle);
    unsigned sw, sh;
    clientScreenSize(&sw, &sh);
    if (w) {
        w->w = (int)sw;
        w->h = (int)sh;
    }
    if (width) *width = (int)sw;
    if (height) *height = (int)sh;
    return true;
}
static uint32_t sdlGetWindowID(void *handle) {
    Window *w = checked(handle);
    return w ? w->id : 0;
}
static void *sdlGetWindowFromID(uint32_t id) {
    for (unsigned i = 0; i < MAX_WINDOWS; ++i)
        if (windows[i].used && windows[i].id == id) return &windows[i];
    return NULL;
}
static uint64_t sdlGetWindowFlags(void *handle) {
    Window *w = checked(handle);
    return (w ? w->flags : 0) | 0x2 /* OPENGL */ | 0x8000000 /* INPUT_FOCUS-like */;
}
static bool sdlGetWindowPosition(void *handle, int *x, int *y) {
    (void)handle;
    if (x) *x = 0;
    if (y) *y = 0;
    return true;
}
static bool sdlGetWindowSafeArea(void *handle, Rect *rect) {
    int w, h;
    sdlGetWindowSize(handle, &w, &h);
    if (rect) *rect = (Rect){0, 0, w, h};
    return true;
}
static bool sdlGetWindowBordersSize(void *handle, int *top, int *left, int *bottom, int *right) {
    (void)handle;
    if (top) *top = 0;
    if (left) *left = 0;
    if (bottom) *bottom = 0;
    if (right) *right = 0;
    return true;
}
// The window is the console's screen: the game's display settings are accepted and have no effect. They are written to the log (a few
// of each) so that a diagnostics file shows which of them the game really asks for.
static unsigned window_requests;
static void windowRequest(const char *name, long a, long b) {
    if (window_requests++ < 40) trace("sdl.%s %ld %ld (accepted, no effect: the window is the screen)", name, a, b);
}
static bool sdlSetWindowFullscreen(void *handle, bool fullscreen) {
    (void)handle;
    windowRequest("SetWindowFullscreen", fullscreen, 0);
    return true;
}
static bool sdlSetWindowSize(void *handle, int w, int h) {
    (void)handle;
    windowRequest("SetWindowSize", w, h);
    return true;
}
static bool sdlSetWindowBordered(void *handle, bool bordered) {
    (void)handle;
    windowRequest("SetWindowBordered", bordered, 0);
    return true;
}
static bool sdlSetWindowResizable(void *handle, bool resizable) {
    (void)handle;
    windowRequest("SetWindowResizable", resizable, 0);
    return true;
}
static bool sdlSetWindowFullscreenMode(void *handle, const DisplayMode *mode) {
    (void)handle;
    windowRequest("SetWindowFullscreenMode", mode ? mode->w : 0, mode ? mode->h : 0);
    return true;
}
static bool sdlShowSimpleMessageBox(uint32_t flags, const char *title, const char *message, void *window) {
    (void)flags;
    (void)window;
    trace("sdl.MESSAGEBOX title=%s message=%s", title ? title : "", message ? message : "");
    return true;
}

// ---- surfaces and cursors (the icon and the mouse cursors are accepted and dropped) -------------------------------------
typedef struct {
    uint32_t flags, format;
    int w, h, pitch;
    void *pixels;
    int refcount;
    void *reserved;
} Surface;
_Static_assert(sizeof(Surface) == 48, "SDL_Surface");
static int pitchFor(uint32_t format, int width) {
    (void)format;
    return width * 4;
}
static Surface *sdlCreateSurface(int w, int h, uint32_t format) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    Surface *surface = linuxAbiCalloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->format = format;
    surface->w = w;
    surface->h = h;
    surface->pitch = pitchFor(format, w);
    surface->refcount = 1;
    surface->pixels = linuxAbiCalloc(1, (size_t)surface->pitch * (size_t)h);
    if (!surface->pixels) {
        linuxAbiFree(surface);
        return NULL;
    }
    return surface;
}
static Surface *sdlCreateSurfaceFrom(int w, int h, uint32_t format, void *pixels, int pitch) {
    Surface *surface = linuxAbiCalloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->format = format;
    surface->w = w;
    surface->h = h;
    surface->pitch = pitch;
    surface->pixels = pixels;
    surface->refcount = 1;
    surface->flags = 0x2;  // PREALLOCATED
    return surface;
}
static void sdlDestroySurface(Surface *surface) {
    if (!surface) return;
    if (!(surface->flags & 0x2)) linuxAbiFree(surface->pixels);
    linuxAbiFree(surface);
}
static void *sdlCreateSystemCursor(int id) {
    static char cursors[32][8];
    return id >= 0 && id < 32 ? cursors[id] : NULL;
}

// ---- OpenGL through EGL (the context and surface of the console's window) -----------------------------------------------
// SDL_GL_* attribute numbers (0 red, 1 green, 2 blue, 3 alpha, 6 depth, 7 stencil, 13 multisample buffers, 14 samples,
// 17/18 context major/minor). The loader used to ask for a 16-bit depth buffer and ignored the stencil and multisampling
// requests: honour all of them, and say in the log what the driver answered.
enum {
    GL_ATTRIBUTE_RED = 0,
    GL_ATTRIBUTE_GREEN = 1,
    GL_ATTRIBUTE_BLUE = 2,
    GL_ATTRIBUTE_ALPHA = 3,
    GL_ATTRIBUTE_DEPTH = 6,
    GL_ATTRIBUTE_STENCIL = 7,
    GL_ATTRIBUTE_MULTISAMPLE_BUFFERS = 13,
    GL_ATTRIBUTE_MULTISAMPLE_SAMPLES = 14,
    GL_ATTRIBUTE_CONTEXT_MAJOR = 17,
    GL_ATTRIBUTE_CONTEXT_MINOR = 18,
    GL_ATTRIBUTE_COUNT = 48
};
static int gl_attributes[GL_ATTRIBUTE_COUNT];
static bool gl_attribute_set[GL_ATTRIBUTE_COUNT];  // 0 is a value too (no depth, no stencil)
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLConfig egl_config;
static _Atomic unsigned requested_size;  // width << 16 | height, 0: nothing asked
static unsigned contexts_created;
static int swap_interval = 1;  // vblanks to wait on present (loader-50): what the client asked for, but never below one
static bool sdlGlSetAttribute(int attribute, int value) {
    if (attribute >= 0 && attribute < GL_ATTRIBUTE_COUNT) {
        gl_attributes[attribute] = value;
        gl_attribute_set[attribute] = true;
    }
    trace("sdl.GL_SetAttribute %d=%d", attribute, value);
    return true;
}
static bool sdlGlGetAttribute(int attribute, int *value) {
    if (value) *value = attribute >= 0 && attribute < GL_ATTRIBUTE_COUNT ? gl_attributes[attribute] : 0;
    return true;
}
static void sdlGlResetAttributes(void) {
    memset(gl_attributes, 0, sizeof(gl_attributes));
    memset(gl_attribute_set, 0, sizeof(gl_attribute_set));
}
static int glAttribute(int attribute, int fallback) { return gl_attribute_set[attribute] ? gl_attributes[attribute] : fallback; }
static void (*display_acquire)(void);
void linuxSdlSetDisplayAcquire(void (*acquire)(void)) { display_acquire = acquire; }

// ---- what the driver reports (diagnostics) ----------------------------------------------------------------------------
// Missing or black sprites, NPCs that only leave their shadow and stalls when a zone loads are driver or resource problems,
// not input/UI problems. Log the context's real capabilities, every GL error the client leaves behind (once per code/period)
// and frames that took far longer than normal, so the console log says which of those it is.
typedef unsigned GlEnum;
typedef int GlInt;
typedef const unsigned char *(*GlGetStringFn)(GlEnum);
typedef void (*GlGetIntegervFn)(GlEnum, GlInt *);
typedef GlEnum (*GlGetErrorFn)(void);
enum {
    GL_VENDOR_ = 0x1F00,
    GL_RENDERER_ = 0x1F01,
    GL_VERSION_ = 0x1F02,
    GL_SHADING_LANGUAGE_VERSION_ = 0x8B8C,
    GL_RED_BITS_ = 0x0D52,
    GL_GREEN_BITS_ = 0x0D53,
    GL_BLUE_BITS_ = 0x0D54,
    GL_ALPHA_BITS_ = 0x0D55,
    GL_DEPTH_BITS_ = 0x0D56,
    GL_STENCIL_BITS_ = 0x0D57,
    GL_SAMPLE_BUFFERS_ = 0x80A8,
    GL_SAMPLES_ = 0x80A9,
    GL_MAX_TEXTURE_SIZE_ = 0x0D33,
    GL_MAX_TEXTURE_IMAGE_UNITS_ = 0x8872,
    GL_MAX_VERTEX_ATTRIBS_ = 0x8869,
    GL_INVALID_ENUM_ = 0x0500,
    GL_INVALID_VALUE_ = 0x0501,
    GL_INVALID_OPERATION_ = 0x0502,
    GL_STACK_OVERFLOW_ = 0x0503,
    GL_STACK_UNDERFLOW_ = 0x0504,
    GL_OUT_OF_MEMORY_ = 0x0505,
    GL_INVALID_FRAMEBUFFER_OPERATION_ = 0x0506
};
static GlGetStringFn glGetStringDiagnostic;
static GlGetIntegervFn glGetIntegervDiagnostic;
static GlGetErrorFn glGetErrorDiagnostic;
static bool loadDiagnosticFunction(void *slot, const char *name) {
    void *address = (void *)eglGetProcAddress(name);
    if (address) memcpy(slot, &address, sizeof(address));
    return address != NULL;
}
static void loadGlDiagnostics(void) {
    if (glGetStringDiagnostic) return;
    bool strings = loadDiagnosticFunction(&glGetStringDiagnostic, "glGetString");
    bool integers = loadDiagnosticFunction(&glGetIntegervDiagnostic, "glGetIntegerv");
    bool errors = loadDiagnosticFunction(&glGetErrorDiagnostic, "glGetError");
    if (!strings || !integers || !errors) diagnosticsTrace("sdl.GL_Diagnostics=UNAVAILABLE strings=%d integers=%d errors=%d", strings, integers, errors);
}
static const char *glText(GlEnum name) {
    const unsigned char *text = glGetStringDiagnostic ? glGetStringDiagnostic(name) : NULL;
    return text ? (const char *)text : "?";
}
// A missing entry point is silent otherwise (LWJGL keeps a null pointer and the feature it draws with disappears), so
// name the first 64 that the driver does not know. Capped: probing for optional extensions is normal.
static _Atomic unsigned missing_gl_reports;
static void *resolveGlFunction(const char *name) {
    void *address = (void *)eglGetProcAddress(name);
    if (!address && name && !strncmp(name, "gl", 2) && atomic_fetch_add(&missing_gl_reports, 1) < 64)
        diagnosticsTrace("gl.proc.missing=%s", name);
    return address;
}
// A shader that fails to compile or link draws nothing and sets no GL error; that is what a "missing" or black model
// looks like from the outside. Wrap the two calls and report the driver's log. The first few successes are recorded too,
// so a log with no shader lines at all says the client never compiled one (and the fault is elsewhere).
typedef unsigned GlName;
typedef void (*GlCompileShaderFn)(GlName);
typedef void (*GlLinkProgramFn)(GlName);
typedef void (*GlGetShaderivFn)(GlName, GlEnum, GlInt *);
typedef void (*GlGetProgramivFn)(GlName, GlEnum, GlInt *);
typedef void (*GlGetShaderInfoLogFn)(GlName, GlInt, GlInt *, char *);
typedef void (*GlGetProgramInfoLogFn)(GlName, GlInt, GlInt *, char *);
enum { GL_COMPILE_STATUS_ = 0x8B81, GL_LINK_STATUS_ = 0x8B82 };
static GlCompileShaderFn glCompileShaderReal;
static GlLinkProgramFn glLinkProgramReal;
static GlGetShaderivFn glGetShaderivReal;
static GlGetProgramivFn glGetProgramivReal;
static GlGetShaderInfoLogFn glGetShaderInfoLogReal;
static GlGetProgramInfoLogFn glGetProgramInfoLogReal;
static void loadShaderDiagnostics(void) {
    if (glCompileShaderReal) return;
    loadDiagnosticFunction(&glCompileShaderReal, "glCompileShader");
    loadDiagnosticFunction(&glLinkProgramReal, "glLinkProgram");
    loadDiagnosticFunction(&glGetShaderivReal, "glGetShaderiv");
    loadDiagnosticFunction(&glGetProgramivReal, "glGetProgramiv");
    loadDiagnosticFunction(&glGetShaderInfoLogReal, "glGetShaderInfoLog");
    loadDiagnosticFunction(&glGetProgramInfoLogReal, "glGetProgramInfoLog");
}
static const char *oneLine(char *text) {
    for (char *p = text; *p; ++p)
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    return text;
}
static void glCompileShaderLogging(GlName shader) {
    loadShaderDiagnostics();
    if (!glCompileShaderReal) return;
    glCompileShaderReal(shader);
    static _Atomic unsigned passed, failed;
    GlInt status = 1;  // without the query, do not claim a failure
    if (glGetShaderivReal) glGetShaderivReal(shader, GL_COMPILE_STATUS_, &status);
    if (status) {
        unsigned count = atomic_fetch_add(&passed, 1) + 1;
        if (count <= 4) diagnosticsTrace("gl.shader.compile=OK shader=%u compiled=%u", shader, count);
        return;
    }
    char log[302] = "(no message)";
    if (glGetShaderInfoLogReal) {
        GlInt length = 0;
        glGetShaderInfoLogReal(shader, (GlInt)sizeof(log) - 1, &length, log);
        log[sizeof(log) - 1] = 0;
    }
    unsigned count = atomic_fetch_add(&failed, 1) + 1;
    if (count <= 16 || count % 50 == 0) diagnosticsTrace("gl.shader.compile=FAIL shader=%u failures=%u log=\"%s\"", shader, count, oneLine(log));
}
static void glLinkProgramLogging(GlName program) {
    loadShaderDiagnostics();
    if (!glLinkProgramReal) return;
    glLinkProgramReal(program);
    static _Atomic unsigned passed, failed;
    GlInt status = 1;  // without the query, do not claim a failure
    if (glGetProgramivReal) glGetProgramivReal(program, GL_LINK_STATUS_, &status);
    if (status) {
        unsigned count = atomic_fetch_add(&passed, 1) + 1;
        if (count <= 4) diagnosticsTrace("gl.program.link=OK program=%u linked=%u", program, count);
        return;
    }
    char log[302] = "(no message)";
    if (glGetProgramInfoLogReal) {
        GlInt length = 0;
        glGetProgramInfoLogReal(program, (GlInt)sizeof(log) - 1, &length, log);
        log[sizeof(log) - 1] = 0;
    }
    unsigned count = atomic_fetch_add(&failed, 1) + 1;
    if (count <= 16 || count % 50 == 0) diagnosticsTrace("gl.program.link=FAIL program=%u failures=%u log=\"%s\"", program, count, oneLine(log));
}
// ---- framebuffer depth compatibility and object accounting ---------------------------------------------------------------
// The shipped ps5-opengl accepts only PIPE_FORMAT_Z32_FLOAT for a depth attachment: packed depth/stencil and depth
// textures are compiled out. A libGDX-style framebuffer asks for GL_DEPTH_COMPONENT16, the renderbuffer is rejected and
// glCheckFramebufferStatus answers GL_FRAMEBUFFER_UNSUPPORTED, which PokeMMO reports as "Frame buffer couldn't be
// constructed: unsupported combination of formats" and turns into a fatal render error. Store the same depth test in the
// format the hardware has (32-bit float) instead. Combined depth/stencil is left alone and logged: the driver has no packed
// format, and dropping the stencil silently would change what the game draws.
typedef unsigned FboEnum;
typedef int FboInt;
typedef int FboSize;
typedef unsigned FboUint;
typedef unsigned char FboBool;
enum {
    GL_DEPTH_COMPONENT16_ = 0x81A5,
    GL_DEPTH_COMPONENT24_ = 0x81A6,
    GL_DEPTH_COMPONENT32_ = 0x81A7,
    GL_DEPTH_COMPONENT32F_ = 0x8CAC,
    GL_DEPTH_STENCIL_ = 0x84F9,
    GL_DEPTH24_STENCIL8_ = 0x88F0,
    GL_DEPTH32F_STENCIL8_ = 0x8CAD,
    GL_FRAMEBUFFER_ = 0x8D40,
    GL_FRAMEBUFFER_BINDING_ = 0x8CA6,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5,
    GL_FRAMEBUFFER_UNSUPPORTED_ = 0x8CDD,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_ = 0x8CD0,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_ = 0x8CD1,
    GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL_ = 0x8CD2,
    GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_DEPTH_ATTACHMENT_ = 0x8D00,
    GL_STENCIL_ATTACHMENT_ = 0x8D20,
    GL_RENDERBUFFER_ = 0x8D41,
    GL_RENDERBUFFER_BINDING_ = 0x8CA7,
    GL_TEXTURE_MIN_FILTER_ = 0x2801,
    GL_TEXTURE_MAG_FILTER_ = 0x2800,
    GL_TEXTURE_WRAP_S_ = 0x2802,
    GL_TEXTURE_WRAP_T_ = 0x2803,
    GL_LINEAR_ = 0x2601,
    GL_CLAMP_TO_EDGE_ = 0x812F,
    GL_RGBA_ = 0x1908,
    GL_UNSIGNED_BYTE_ = 0x1401,
    GL_DRAW_FRAMEBUFFER_ = 0x8CA9,
    GL_READ_FRAMEBUFFER_ = 0x8CA8,
    GL_READ_FRAMEBUFFER_BINDING_ = 0x8CAA,
    GL_DEPTH_BUFFER_BIT_ = 0x00000100,
    GL_SCISSOR_TEST_ = 0x0C11,
    GL_STENCIL_TEST_ = 0x0B90,
    GL_ALL_ATTRIB_BITS_ = 0x000FFFFF,
    GL_DOUBLEBUFFER_ = 0x0C32,
    GL_READ_BUFFER_ = 0x0C02,
    GL_DRAW_BUFFER_ = 0x0C01,
    GL_BACK_ = 0x0405,
    GL_FRONT_ = 0x0404,
    GL_TEXTURE_BINDING_2D_ = 0x8069,
    GL_TEXTURE_2D_ = 0x0DE1,
    GL_TEXTURE_BINDING_3D_ = 0x806A,
    GL_TEXTURE_BINDING_2D_ARRAY_ = 0x8C1D,
    GL_TEXTURE_BINDING_2D_MULTISAMPLE_ = 0x9104,
    GL_TEXTURE_BINDING_RECTANGLE_ = 0x84F6,
    GL_TEXTURE_BINDING_CUBE_MAP_ = 0x8514
};
static bool isDepthFormat(FboEnum format) {
    return format == GL_DEPTH_COMPONENT16_ || format == GL_DEPTH_COMPONENT24_ || format == GL_DEPTH_COMPONENT32_ ||
           format == GL_DEPTH_COMPONENT32F_ || format == GL_DEPTH_STENCIL_ || format == GL_DEPTH24_STENCIL8_ ||
           format == GL_DEPTH32F_STENCIL8_;
}
static FboEnum compatibleDepthFormat(FboEnum format) {
    switch (format) {
        case GL_DEPTH_COMPONENT16_:
        case GL_DEPTH_COMPONENT24_:
        case GL_DEPTH_COMPONENT32_:
        // loader-60: the driver compiles packed depth/stencil out completely (PS5_ENABLE_PACKED_DEPTH_STENCIL is 0), so a
        // GL_DEPTH24_STENCIL8/GL_DEPTH32F_STENCIL8 request leaves the renderbuffer (or texture) with no storage at all and
        // the framebuffer fails as "invalid depth attachment". A depth-only Z32F target keeps the depth test and the FBO
        // usable; the client loses stencil writes in that one target only, which is far better than a fatal render error.
        case GL_DEPTH_STENCIL_:
        case GL_DEPTH24_STENCIL8_:
        case GL_DEPTH32F_STENCIL8_: return GL_DEPTH_COMPONENT32F_;
        default: return format;
    }
}
static _Atomic unsigned depth_storage_logs, fbo_status_logs, last_depth_format;
static _Atomic unsigned long long texture_upload_kib;
// Upload cost matters for hitching: a zone load uploads hundreds of tilesets, and the driver copies each one through its
// staging surface on the CPU.  Track both the bytes and the time spent inside the real upload call per 600-frame window.
static _Atomic unsigned long long texture_upload_ns;
static _Atomic unsigned texture_upload_calls;
static void countTextureUpload(FboSize w, FboSize h, FboSize d) {
    if (w <= 0 || h <= 0 || d <= 0) return;
    unsigned long long kib = (unsigned long long)w * (unsigned long long)h * (unsigned long long)d * 4ull / 1024ull;
    atomic_fetch_add(&texture_upload_kib, kib);
}
static void noteUploadCost(uint64_t began) {
    atomic_fetch_add(&texture_upload_ns, platformMonotonicNs() - began);
    atomic_fetch_add(&texture_upload_calls, 1);
}
// The last few storage requests in front of a failed completeness check: an unsupported framebuffer can be a color
// format, a depth format or a combined depth/stencil, and the status alone does not say which.
typedef struct {
    const char *kind;
    FboEnum requested, used;
    FboInt w, h;
} FboStorageEvent;
static FboStorageEvent fbo_storage_events[16];
static _Atomic unsigned fbo_storage_events_count;
static void rememberStorage(const char *kind, FboEnum requested, FboEnum used, FboInt w, FboInt h) {
    unsigned slot = atomic_fetch_add(&fbo_storage_events_count, 1) & 15u;
    fbo_storage_events[slot] = (FboStorageEvent){kind, requested, used, w, h};
}
static void logRecentStorage(void) {
    unsigned count = atomic_load(&fbo_storage_events_count);
    unsigned shown = count < 16 ? count : 16;
    for (unsigned i = 0; i < shown; ++i) {
        FboStorageEvent event = fbo_storage_events[(count - 1 - i) & 15u];
        diagnosticsTrace("gl.fbo.recent kind=%s requested=0x%x stored=0x%x size=%dx%d", event.kind, event.requested, event.used, event.w,
                         event.h);
    }
}
// Storage calls only name their target object through the binding, and a failed framebuffer is diagnosed later. Keep
// the last format and size seen for every texture/renderbuffer name so the attachment dump can print the truth.
typedef struct {
    FboUint name;
    FboEnum format;
    FboInt w, h;
    FboInt samples;
    unsigned char kind; // 0 = texture, 1 = renderbuffer
    unsigned char used;
    unsigned char attached;
} TextureMeta;
#define TEXTURE_META_SLOTS 4096u
static TextureMeta texture_meta[TEXTURE_META_SLOTS];
static void rememberTextureMeta(FboUint name, FboEnum format, FboInt w, FboInt h, FboInt samples, unsigned char kind) {
    if (!name) return;
    unsigned slot = (name * 2654435761u + kind * 97u) & (TEXTURE_META_SLOTS - 1u);
    for (unsigned probe = 0; probe < 32; ++probe) {
        TextureMeta *entry = &texture_meta[(slot + probe) & (TEXTURE_META_SLOTS - 1u)];
        if (!entry->used || (entry->name == name && entry->kind == kind)) {
            unsigned char attached = entry->used && entry->name == name ? entry->attached : 0;
            *entry = (TextureMeta){name, format, w, h, samples, kind, 1, attached};
            return;
        }
    }
}
static TextureMeta *findTextureMeta(FboUint name, unsigned char kind) {
    if (!name) return NULL;
    unsigned slot = (name * 2654435761u + kind * 97u) & (TEXTURE_META_SLOTS - 1u);
    for (unsigned probe = 0; probe < 32; ++probe) {
        TextureMeta *entry = &texture_meta[(slot + probe) & (TEXTURE_META_SLOTS - 1u)];
        if (!entry->used) return NULL;
        if (entry->name == name && entry->kind == kind) return entry;
    }
    return NULL;
}
static FboEnum bindingForKind(const char *kind) {
    if (!strncmp(kind, "renderbuffer", 12)) return GL_RENDERBUFFER_BINDING_;
    if (!strncmp(kind, "texture3d", 9) || !strncmp(kind, "storage3d", 9)) return GL_TEXTURE_BINDING_3D_;
    if (!strncmp(kind, "texture2d_multisample", 21) || !strncmp(kind, "storage2d_multisample", 21))
        return GL_TEXTURE_BINDING_2D_MULTISAMPLE_;
    return GL_TEXTURE_BINDING_2D_;
}
// A depth target is padded by the driver to its tiling alignment, so a small attachment still costs megabytes. Counting
// both the raw and padded size says whether the game is exhausting direct memory with many small targets.
typedef struct {
    FboUint name;
    FboEnum format;
    FboInt w, h;
} DepthTarget;
static DepthTarget depth_targets[64];
static _Atomic unsigned depth_target_count;
static _Atomic unsigned long long depth_target_bytes, depth_target_padded;
static void logDepthStorage(const char *kind, FboEnum requested, FboEnum used, FboInt w, FboInt h, FboInt samples) {
    rememberStorage(kind, requested, used, w, h);
    bool renderbuffer = !strncmp(kind, "renderbuffer", 12);
    FboInt binding = 0;
    if (glGetIntegervDiagnostic) glGetIntegervDiagnostic(bindingForKind(kind), &binding);
    rememberTextureMeta((FboUint)binding, used, w, h, samples, renderbuffer ? 1 : 0);
    if (!isDepthFormat(requested)) return;
    atomic_store(&last_depth_format, requested);
    unsigned count = atomic_fetch_add(&depth_storage_logs, 1) + 1;
    if (count <= 32) diagnosticsTrace("gl.fbo.%s requested=0x%x stored=0x%x size=%dx%d samples=%d", kind, requested, used, w, h, samples);
    unsigned slot = atomic_fetch_add(&depth_target_count, 1) & 63u;
    depth_targets[slot] = (DepthTarget){(FboUint)binding, used, w, h};
    if (w > 0 && h > 0) {
        unsigned long long bytes = (unsigned long long)w * (unsigned long long)h * 4ull;
        unsigned long long padded = (bytes + 0x1FFFFFull) & ~0x1FFFFFull;
        if (padded < 0x200000ull) padded = 0x200000ull;
        atomic_fetch_add(&depth_target_bytes, bytes);
        atomic_fetch_add(&depth_target_padded, padded);
    }
}
static void loadFboFunction(void *slot, const char *name) {
    void *address = (void *)eglGetProcAddress(name);
    if (address) memcpy(slot, &address, sizeof(address));
}
typedef void (*FboStorageFn)(FboEnum, FboEnum, FboSize, FboSize);
typedef void (*FboStorageMultisampleFn)(FboEnum, FboSize, FboEnum, FboSize, FboSize);
typedef void (*FboTexImage2DFn)(FboEnum, FboInt, FboInt, FboSize, FboSize, FboInt, FboEnum, FboEnum, const void *);
typedef void (*FboTexImage3DFn)(FboEnum, FboInt, FboInt, FboSize, FboSize, FboSize, FboInt, FboEnum, FboEnum, const void *);
typedef void (*FboTexStorage2DFn)(FboEnum, FboSize, FboEnum, FboSize, FboSize);
typedef void (*FboTexStorage3DFn)(FboEnum, FboSize, FboEnum, FboSize, FboSize, FboSize);
typedef void (*FboTexMultisampleFn)(FboEnum, FboSize, FboEnum, FboSize, FboSize, FboBool);
typedef FboEnum (*FboStatusFn)(FboEnum);
typedef void (*FboGetAttachmentFn)(FboEnum, FboEnum, FboEnum, FboInt *);
static FboGetAttachmentFn glGetFramebufferAttachmentParameterivReal;
static FboStorageFn glRenderbufferStorageReal, glRenderbufferStorageExtReal;
static FboStorageMultisampleFn glRenderbufferStorageMultisampleReal, glRenderbufferStorageMultisampleExtReal;
static FboTexImage2DFn glTexImage2DReal;
static FboTexImage3DFn glTexImage3DReal;
static FboTexStorage2DFn glTexStorage2DReal;
static FboTexStorage3DFn glTexStorage3DReal;
static FboTexMultisampleFn glTexImage2DMultisampleReal, glTexStorage2DMultisampleReal;
static FboStatusFn glCheckFramebufferStatusReal, glCheckFramebufferStatusExtReal;
static void glRenderbufferStorageCompat(FboEnum target, FboEnum format, FboSize w, FboSize h) {
    loadFboFunction(&glRenderbufferStorageReal, "glRenderbufferStorage");
    if (!glRenderbufferStorageReal) return;
    FboEnum used = compatibleDepthFormat(format);
    logDepthStorage("renderbuffer", format, used, w, h, 0);
    glRenderbufferStorageReal(target, used, w, h);
}
static void glRenderbufferStorageExtCompat(FboEnum target, FboEnum format, FboSize w, FboSize h) {
    loadFboFunction(&glRenderbufferStorageExtReal, "glRenderbufferStorageEXT");
    if (!glRenderbufferStorageExtReal) return;
    FboEnum used = compatibleDepthFormat(format);
    logDepthStorage("renderbuffer_ext", format, used, w, h, 0);
    glRenderbufferStorageExtReal(target, used, w, h);
}
static void glRenderbufferStorageMultisampleCompat(FboEnum target, FboSize samples, FboEnum format, FboSize w, FboSize h) {
    loadFboFunction(&glRenderbufferStorageMultisampleReal, "glRenderbufferStorageMultisample");
    if (!glRenderbufferStorageMultisampleReal) return;
    FboEnum used = compatibleDepthFormat(format);
    logDepthStorage("renderbuffer_multisample", format, used, w, h, samples);
    glRenderbufferStorageMultisampleReal(target, samples, used, w, h);
}
static void glRenderbufferStorageMultisampleExtCompat(FboEnum target, FboSize samples, FboEnum format, FboSize w, FboSize h) {
    loadFboFunction(&glRenderbufferStorageMultisampleExtReal, "glRenderbufferStorageMultisampleEXT");
    if (!glRenderbufferStorageMultisampleExtReal) return;
    FboEnum used = compatibleDepthFormat(format);
    logDepthStorage("renderbuffer_multisample_ext", format, used, w, h, samples);
    glRenderbufferStorageMultisampleExtReal(target, samples, used, w, h);
}
static void glTexImage2DCompat(FboEnum target, FboInt level, FboInt internal, FboSize w, FboSize h, FboInt border, FboEnum format,
                               FboEnum type, const void *pixels) {
    loadFboFunction(&glTexImage2DReal, "glTexImage2D");
    if (!glTexImage2DReal) return;
    // Substitute only when there is no pixel data: a packed depth/stencil image with data must keep its internal format, or the
    // upload's format/type pair no longer matches it. An empty image is a storage request, and those are the ones that fail.
    FboEnum used = pixels ? (FboEnum)internal : compatibleDepthFormat((FboEnum)internal);
    logDepthStorage("texture2d", (FboEnum)internal, used, w, h, 0);
    countTextureUpload(w, h, 1);
    uint64_t began = platformMonotonicNs();
    glTexImage2DReal(target, level, (FboInt)used, w, h, border, format, type, pixels);
    noteUploadCost(began);
}
static void glTexImage3DCompat(FboEnum target, FboInt level, FboInt internal, FboSize w, FboSize h, FboSize d, FboInt border, FboEnum format,
                               FboEnum type, const void *pixels) {
    loadFboFunction(&glTexImage3DReal, "glTexImage3D");
    if (!glTexImage3DReal) return;
    FboEnum used = pixels ? (FboEnum)internal : compatibleDepthFormat((FboEnum)internal);
    logDepthStorage("texture3d", (FboEnum)internal, used, w, h, 0);
    countTextureUpload(w, h, d);
    uint64_t began = platformMonotonicNs();
    glTexImage3DReal(target, level, (FboInt)used, w, h, d, border, format, type, pixels);
    noteUploadCost(began);
}
static void glTexStorage2DCompat(FboEnum target, FboSize levels, FboEnum internal, FboSize w, FboSize h) {
    loadFboFunction(&glTexStorage2DReal, "glTexStorage2D");
    if (!glTexStorage2DReal) return;
    FboEnum used = compatibleDepthFormat(internal);
    logDepthStorage("storage2d", internal, used, w, h, 0);
    countTextureUpload(w, h, 1);
    uint64_t began = platformMonotonicNs();
    glTexStorage2DReal(target, levels, used, w, h);
    noteUploadCost(began);
}
static void glTexStorage3DCompat(FboEnum target, FboSize levels, FboEnum internal, FboSize w, FboSize h, FboSize d) {
    loadFboFunction(&glTexStorage3DReal, "glTexStorage3D");
    if (!glTexStorage3DReal) return;
    FboEnum used = compatibleDepthFormat(internal);
    logDepthStorage("storage3d", internal, used, w, h, 0);
    countTextureUpload(w, h, d);
    glTexStorage3DReal(target, levels, used, w, h, d);
}
static void glTexImage2DMultisampleCompat(FboEnum target, FboSize samples, FboEnum internal, FboSize w, FboSize h, FboBool fixed) {
    loadFboFunction(&glTexImage2DMultisampleReal, "glTexImage2DMultisample");
    if (!glTexImage2DMultisampleReal) return;
    FboEnum used = compatibleDepthFormat(internal);
    logDepthStorage("texture2d_multisample", internal, used, w, h, samples);
    glTexImage2DMultisampleReal(target, samples, used, w, h, fixed);
}
static void glTexStorage2DMultisampleCompat(FboEnum target, FboSize samples, FboEnum internal, FboSize w, FboSize h, FboBool fixed) {
    loadFboFunction(&glTexStorage2DMultisampleReal, "glTexStorage2DMultisample");
    if (!glTexStorage2DMultisampleReal) return;
    FboEnum used = compatibleDepthFormat(internal);
    logDepthStorage("storage2d_multisample", internal, used, w, h, samples);
    glTexStorage2DMultisampleReal(target, samples, used, w, h, fixed);
}
static void logFboAttachment(FboEnum target, FboEnum attachment, const char *slot) {
    loadFboFunction(&glGetFramebufferAttachmentParameterivReal, "glGetFramebufferAttachmentParameteriv");
    if (!glGetFramebufferAttachmentParameterivReal) return;
    FboInt type = 0, name = 0, level = 0;
    glGetFramebufferAttachmentParameterivReal(target, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_, &type);
    if (type == 0) {
        diagnosticsTrace("gl.fbo.attach slot=%s none", slot);
        return;
    }
    glGetFramebufferAttachmentParameterivReal(target, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_, &name);
    if (type == GL_RENDERBUFFER_) {
        const TextureMeta *meta = findTextureMeta((FboUint)name, 1);
        if (meta)
            diagnosticsTrace("gl.fbo.attach slot=%s type=renderbuffer name=%d format=0x%x size=%dx%d samples=%d", slot, name, meta->format,
                             meta->w, meta->h, meta->samples);
        else
            diagnosticsTrace("gl.fbo.attach slot=%s type=renderbuffer name=%d format=unseen", slot, name);
        return;
    }
    glGetFramebufferAttachmentParameterivReal(target, attachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL_, &level);
    const TextureMeta *meta = findTextureMeta((FboUint)name, 0);
    if (meta)
        diagnosticsTrace("gl.fbo.attach slot=%s type=texture name=%d level=%d format=0x%x size=%dx%d samples=%d", slot, name, level,
                         meta->format, meta->w, meta->h, meta->samples);
    else
        diagnosticsTrace("gl.fbo.attach slot=%s type=texture name=%d level=%d format=unseen", slot, name, level);
}
static void logFboAttachments(FboEnum target) {
    static const char *const color_slots[8] = {"color0", "color1", "color2", "color3",
                                               "color4", "color5", "color6", "color7"};
    logFboAttachment(target, GL_DEPTH_ATTACHMENT_, "depth");
    logFboAttachment(target, GL_STENCIL_ATTACHMENT_, "stencil");
    for (unsigned i = 0; i < 8; ++i) logFboAttachment(target, GL_COLOR_ATTACHMENT0_ + i, color_slots[i]);
}
static void logFboStatus(FboEnum target, FboEnum status) {
    unsigned count = atomic_fetch_add(&fbo_status_logs, 1) + 1;
    if (count > 16) return;
    loadGlDiagnostics();
    FboInt binding = 0;
    if (glGetIntegervDiagnostic) glGetIntegervDiagnostic(GL_FRAMEBUFFER_BINDING_, &binding);
    diagnosticsTrace("gl.fbo.status=0x%x target=0x%x fbo=%d last_depth=0x%x failures=%u", status, target, binding,
                     atomic_load(&last_depth_format), count);
    if (count <= 2) {
        logFboAttachments(target);
        logRecentStorage();
    }
}
// The repair of a rejected framebuffer lives below, after the attachment calls it needs (see "framebuffer repair").
// A live object count says whether the direct-memory failures are a leak (textures/framebuffers that are made and never
// released) or the game simply asking for more than the console maps. Texture bytes are allocation churn, not residency.
typedef void (*FboGenFn)(FboSize, FboUint *);
typedef void (*FboDeleteFn)(FboSize, const FboUint *);
static FboGenFn glGenTexturesReal, glGenRenderbuffersReal, glGenRenderbuffersExtReal, glGenFramebuffersReal, glGenFramebuffersExtReal, glGenBuffersReal;
static FboDeleteFn glDeleteTexturesReal, glDeleteRenderbuffersReal, glDeleteRenderbuffersExtReal, glDeleteFramebuffersReal, glDeleteFramebuffersExtReal, glDeleteBuffersReal;
static _Atomic int live_textures, live_renderbuffers, live_framebuffers, live_buffers;
static _Atomic int total_textures_generated, total_textures_deleted;
static void setAttachmentState(FboUint name, bool attached, unsigned char kind);
static void countGenerated(FboSize n, _Atomic int *live) {
    if (n > 0) atomic_fetch_add(live, (int)n);
}
static void countDeleted(FboSize n, _Atomic int *live) {
    if (n > 0) atomic_fetch_sub(live, (int)n);
}
static void glGenTexturesCompat(FboSize n, FboUint *names) {
    loadFboFunction(&glGenTexturesReal, "glGenTextures");
    if (glGenTexturesReal) glGenTexturesReal(n, names);
    countGenerated(n, &live_textures);
    countGenerated(n, &total_textures_generated);
}
static void glDeleteTexturesCompat(FboSize n, const FboUint *names) {
    loadFboFunction(&glDeleteTexturesReal, "glDeleteTextures");
    for (FboSize i = 0; i < n; ++i) setAttachmentState(names[i], false, 0);
    if (glDeleteTexturesReal) glDeleteTexturesReal(n, names);
    countDeleted(n, &live_textures);
    if (n > 0) atomic_fetch_add(&total_textures_deleted, (int)n);
}
static void glGenRenderbuffersCompat(FboSize n, FboUint *names) {
    loadFboFunction(&glGenRenderbuffersReal, "glGenRenderbuffers");
    if (glGenRenderbuffersReal) glGenRenderbuffersReal(n, names);
    countGenerated(n, &live_renderbuffers);
}
static void glDeleteRenderbuffersCompat(FboSize n, const FboUint *names) {
    loadFboFunction(&glDeleteRenderbuffersReal, "glDeleteRenderbuffers");
    if (glDeleteRenderbuffersReal) glDeleteRenderbuffersReal(n, names);
    countDeleted(n, &live_renderbuffers);
}
static void glGenRenderbuffersExtCompat(FboSize n, FboUint *names) {
    loadFboFunction(&glGenRenderbuffersExtReal, "glGenRenderbuffersEXT");
    if (glGenRenderbuffersExtReal) glGenRenderbuffersExtReal(n, names);
    countGenerated(n, &live_renderbuffers);
}
static void glDeleteRenderbuffersExtCompat(FboSize n, const FboUint *names) {
    loadFboFunction(&glDeleteRenderbuffersExtReal, "glDeleteRenderbuffersEXT");
    if (glDeleteRenderbuffersExtReal) glDeleteRenderbuffersExtReal(n, names);
    countDeleted(n, &live_renderbuffers);
}
static void glGenFramebuffersCompat(FboSize n, FboUint *names) {
    loadFboFunction(&glGenFramebuffersReal, "glGenFramebuffers");
    if (glGenFramebuffersReal) glGenFramebuffersReal(n, names);
    countGenerated(n, &live_framebuffers);
}
static void glDeleteFramebuffersCompat(FboSize n, const FboUint *names) {
    loadFboFunction(&glDeleteFramebuffersReal, "glDeleteFramebuffers");
    if (glDeleteFramebuffersReal) glDeleteFramebuffersReal(n, names);
    countDeleted(n, &live_framebuffers);
}
static void glGenFramebuffersExtCompat(FboSize n, FboUint *names) {
    loadFboFunction(&glGenFramebuffersExtReal, "glGenFramebuffersEXT");
    if (glGenFramebuffersExtReal) glGenFramebuffersExtReal(n, names);
    countGenerated(n, &live_framebuffers);
}
static void glDeleteFramebuffersExtCompat(FboSize n, const FboUint *names) {
    loadFboFunction(&glDeleteFramebuffersExtReal, "glDeleteFramebuffersEXT");
    if (glDeleteFramebuffersExtReal) glDeleteFramebuffersExtReal(n, names);
    countDeleted(n, &live_framebuffers);
}
static void glGenBuffersCompat(FboSize n, FboUint *names) {
    loadFboFunction(&glGenBuffersReal, "glGenBuffers");
    if (glGenBuffersReal) glGenBuffersReal(n, names);
    countGenerated(n, &live_buffers);
}
static void glDeleteBuffersCompat(FboSize n, const FboUint *names) {
    loadFboFunction(&glDeleteBuffersReal, "glDeleteBuffers");
    if (glDeleteBuffersReal) glDeleteBuffersReal(n, names);
    countDeleted(n, &live_buffers);
}
// A texture that is used as a framebuffer attachment costs a 2 MiB-aligned tiled surface in the driver, whether it is
// 80x80 or 640x512. Tracking attachments separates real residency from the much larger pool of sampled-only textures.
static _Atomic int attached_targets;
static _Atomic unsigned long long attached_target_bytes;
static unsigned long long paddedTargetBytes(FboInt w, FboInt h) {
    if (w <= 0 || h <= 0) return 0;
    unsigned long long bytes = (unsigned long long)w * (unsigned long long)h * 4ull;
    unsigned long long padded = (bytes + 0x1FFFFFull) & ~0x1FFFFFull;
    return padded < 0x200000ull ? 0x200000ull : padded;
}
static void setAttachmentState(FboUint name, bool attached, unsigned char kind) {
    TextureMeta *entry = findTextureMeta(name, kind);
    if (!entry || entry->attached == (unsigned char)attached) return;
    entry->attached = (unsigned char)attached;
    unsigned long long bytes = paddedTargetBytes(entry->w, entry->h);
    if (attached) {
        atomic_fetch_add(&attached_targets, 1);
        atomic_fetch_add(&attached_target_bytes, bytes);
    } else {
        atomic_fetch_sub(&attached_targets, 1);
        atomic_fetch_sub(&attached_target_bytes, bytes);
    }
}
typedef void (*FboFramebufferTexture2DFn)(FboEnum, FboEnum, FboEnum, FboUint, FboInt);
typedef void (*FboFramebufferTextureFn)(FboEnum, FboEnum, FboUint, FboInt);
typedef void (*FboFramebufferRenderbufferFn)(FboEnum, FboEnum, FboEnum, FboUint);
typedef void (*FboNamedFramebufferTextureFn)(FboUint, FboEnum, FboUint, FboInt);
typedef void (*FboNamedFramebufferRenderbufferFn)(FboUint, FboEnum, FboEnum, FboUint);
static FboFramebufferTexture2DFn glFramebufferTexture2DReal;
static FboFramebufferTextureFn glFramebufferTextureReal;
static FboFramebufferRenderbufferFn glFramebufferRenderbufferReal;
static FboNamedFramebufferTextureFn glNamedFramebufferTextureReal;
static FboNamedFramebufferRenderbufferFn glNamedFramebufferRenderbufferReal;
static void glFramebufferTexture2DCompat(FboEnum target, FboEnum attachment, FboEnum textarget, FboUint texture, FboInt level) {
    loadFboFunction(&glFramebufferTexture2DReal, "glFramebufferTexture2D");
    if (glFramebufferTexture2DReal) glFramebufferTexture2DReal(target, attachment, textarget, texture, level);
    setAttachmentState(texture, texture != 0, 0);
}
static void glFramebufferTextureCompat(FboEnum target, FboEnum attachment, FboUint texture, FboInt level) {
    loadFboFunction(&glFramebufferTextureReal, "glFramebufferTexture");
    if (glFramebufferTextureReal) glFramebufferTextureReal(target, attachment, texture, level);
    setAttachmentState(texture, texture != 0, 0);
}
static void glFramebufferRenderbufferCompat(FboEnum target, FboEnum attachment, FboEnum renderbuffer_target, FboUint renderbuffer) {
    loadFboFunction(&glFramebufferRenderbufferReal, "glFramebufferRenderbuffer");
    if (glFramebufferRenderbufferReal) glFramebufferRenderbufferReal(target, attachment, renderbuffer_target, renderbuffer);
    setAttachmentState(renderbuffer, renderbuffer != 0, 1);
}
static void glNamedFramebufferTextureCompat(FboUint framebuffer, FboEnum attachment, FboUint texture, FboInt level) {
    loadFboFunction(&glNamedFramebufferTextureReal, "glNamedFramebufferTexture");
    if (glNamedFramebufferTextureReal) glNamedFramebufferTextureReal(framebuffer, attachment, texture, level);
    setAttachmentState(texture, texture != 0, 0);
}
static void glNamedFramebufferRenderbufferCompat(FboUint framebuffer, FboEnum attachment, FboEnum renderbuffer_target,
                                                 FboUint renderbuffer) {
    loadFboFunction(&glNamedFramebufferRenderbufferReal, "glNamedFramebufferRenderbuffer");
    if (glNamedFramebufferRenderbufferReal)
        glNamedFramebufferRenderbufferReal(framebuffer, attachment, renderbuffer_target, renderbuffer);
    setAttachmentState(renderbuffer, renderbuffer != 0, 1);
}
// ---- framebuffer repair -----------------------------------------------------------------------------------------------------
// Mesa answers GL_FRAMEBUFFER_UNSUPPORTED for three shapes Gallium cannot express: depth and stencil on separate objects, a
// packed depth/stencil object (the driver compiles that format out), and a color attachment it does not expose as a render
// target. The first two can be repaired without changing what the game draws - keep the depth object, drop the separate
// stencil or use the depth-only format - and a color-only target is also given a depth buffer of its own, because a target
// that declares no depth at all has no way to say why the driver rejected it and libGDX-style code always expected one.
// The original failure is logged first, then the outcome, so a log always names the truth.
typedef void (*FboBindRenderbufferFn)(FboEnum, FboUint);
static FboBindRenderbufferFn glBindRenderbufferReal;
static FboUint repair_depth_names[4];  // one private depth renderbuffer per size, made on demand and never freed
static FboInt repair_depth_w[4], repair_depth_h[4];
static _Atomic unsigned repair_attempts, repair_successes, repair_depth_made;
static FboInt attachmentObjectName(FboEnum target, FboEnum attachment) {
    loadFboFunction(&glGetFramebufferAttachmentParameterivReal, "glGetFramebufferAttachmentParameteriv");
    if (!glGetFramebufferAttachmentParameterivReal) return 0;
    FboInt name = 0;
    glGetFramebufferAttachmentParameterivReal(target, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_, &name);
    return name;
}
static bool attachSharedDepth(FboEnum target) {
    loadFboFunction(&glGetFramebufferAttachmentParameterivReal, "glGetFramebufferAttachmentParameteriv");
    loadFboFunction(&glGenRenderbuffersReal, "glGenRenderbuffers");
    loadFboFunction(&glBindRenderbufferReal, "glBindRenderbuffer");
    loadFboFunction(&glRenderbufferStorageReal, "glRenderbufferStorage");
    loadFboFunction(&glFramebufferRenderbufferReal, "glFramebufferRenderbuffer");
    if (!glGetFramebufferAttachmentParameterivReal || !glGenRenderbuffersReal || !glBindRenderbufferReal ||
        !glRenderbufferStorageReal || !glFramebufferRenderbufferReal)
        return false;
    FboInt color_type = 0, other_type = 0, depth_type = 0, stencil_type = 0;
    glGetFramebufferAttachmentParameterivReal(target, GL_COLOR_ATTACHMENT0_, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_, &color_type);
    if (!color_type) return false;
    for (unsigned slot = 1; slot < 8; ++slot) {  // one color attachment only: that is the shape the crash showed
        glGetFramebufferAttachmentParameterivReal(target, GL_COLOR_ATTACHMENT0_ + slot, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_,
                                                  &other_type);
        if (other_type) return false;
    }
    glGetFramebufferAttachmentParameterivReal(target, GL_DEPTH_ATTACHMENT_, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_, &depth_type);
    glGetFramebufferAttachmentParameterivReal(target, GL_STENCIL_ATTACHMENT_, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_, &stencil_type);
    if (depth_type || stencil_type) return false;
    const TextureMeta *meta = findTextureMeta((FboUint)attachmentObjectName(target, GL_COLOR_ATTACHMENT0_),
                                             color_type == GL_RENDERBUFFER_ ? 1 : 0);
    if (!meta || meta->w <= 0 || meta->h <= 0) return false;
    if (meta->samples > 1) return false;  // a single-sample depth buffer cannot join a multisample colour attachment
    FboUint depth = 0;
    unsigned free_slot = 4;
    bool created = false;
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (repair_depth_names[slot] && repair_depth_w[slot] == meta->w && repair_depth_h[slot] == meta->h) depth = repair_depth_names[slot];
        else if (!repair_depth_names[slot] && free_slot == 4) free_slot = slot;
    }
    FboInt saved_renderbuffer = 0;
    if (glGetIntegervDiagnostic) glGetIntegervDiagnostic(GL_RENDERBUFFER_BINDING_, &saved_renderbuffer);
    if (!depth) {
        if (free_slot == 4) return false;  // four sizes already; do not grow without bound
        glGenRenderbuffersReal(1, &depth);
        if (!depth) return false;
        created = true;
    }
    glBindRenderbufferReal(GL_RENDERBUFFER_, depth);
    if (created) {
        glRenderbufferStorageReal(GL_RENDERBUFFER_, GL_DEPTH_COMPONENT32F_, meta->w, meta->h);
        repair_depth_names[free_slot] = depth;
        repair_depth_w[free_slot] = meta->w;
        repair_depth_h[free_slot] = meta->h;
        atomic_fetch_add(&repair_depth_made, 1);
    }
    glFramebufferRenderbufferReal(target, GL_DEPTH_ATTACHMENT_, GL_RENDERBUFFER_, depth);
    glBindRenderbufferReal(GL_RENDERBUFFER_, (FboUint)saved_renderbuffer);
    return true;
}
static FboEnum checkFramebufferStatus(FboEnum target, FboStatusFn check) {
    FboEnum status = check(target);
    if (status == GL_FRAMEBUFFER_COMPLETE_) return status;
    atomic_fetch_add(&repair_attempts, 1);
    logFboStatus(target, status);
    if (status != GL_FRAMEBUFFER_UNSUPPORTED_) return status;
    bool repaired = false;
    // 1. Depth and stencil attached as two different objects: Gallium has no separate stencil, so keep the depth.
    FboInt depth_type = 0, stencil_type = 0;
    loadFboFunction(&glGetFramebufferAttachmentParameterivReal, "glGetFramebufferAttachmentParameteriv");
    if (glGetFramebufferAttachmentParameterivReal) {
        glGetFramebufferAttachmentParameterivReal(target, GL_DEPTH_ATTACHMENT_, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_, &depth_type);
        glGetFramebufferAttachmentParameterivReal(target, GL_STENCIL_ATTACHMENT_, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_, &stencil_type);
        FboInt depth_name = depth_type ? attachmentObjectName(target, GL_DEPTH_ATTACHMENT_) : 0;
        FboInt stencil_name = stencil_type ? attachmentObjectName(target, GL_STENCIL_ATTACHMENT_) : 0;
        if (depth_type && stencil_type && (depth_type != stencil_type || depth_name != stencil_name)) {
            if (stencil_type == GL_RENDERBUFFER_) {
                loadFboFunction(&glFramebufferRenderbufferReal, "glFramebufferRenderbuffer");
                if (glFramebufferRenderbufferReal) glFramebufferRenderbufferReal(target, GL_STENCIL_ATTACHMENT_, GL_RENDERBUFFER_, 0);
            } else {
                loadFboFunction(&glFramebufferTexture2DReal, "glFramebufferTexture2D");
                if (glFramebufferTexture2DReal) glFramebufferTexture2DReal(target, GL_STENCIL_ATTACHMENT_, GL_TEXTURE_2D_, 0, 0);
            }
            diagnosticsTrace("gl.fbo.repair detach-stencil depth=0x%x stencil=0x%x", depth_type, stencil_type);
            status = check(target);
            repaired = true;
        }
    }
    // 2. A single colour attachment and no depth: give it one, then re-check.
    if (status != GL_FRAMEBUFFER_COMPLETE_ && attachSharedDepth(target)) {
        status = check(target);
        repaired = true;
    }
    if (repaired && status == GL_FRAMEBUFFER_COMPLETE_) {
        unsigned count = atomic_fetch_add(&repair_successes, 1) + 1;
        if (count <= 16) diagnosticsTrace("gl.fbo.repaired target=0x%x attempts=%u successes=%u", target, atomic_load(&repair_attempts), count);
    }
    return status;
}
static FboEnum glCheckFramebufferStatusCompat(FboEnum target) {
    loadFboFunction(&glCheckFramebufferStatusReal, "glCheckFramebufferStatus");
    if (!glCheckFramebufferStatusReal) return GL_FRAMEBUFFER_UNSUPPORTED_;
    return checkFramebufferStatus(target, glCheckFramebufferStatusReal);
}
static FboEnum glCheckFramebufferStatusExtCompat(FboEnum target) {
    loadFboFunction(&glCheckFramebufferStatusExtReal, "glCheckFramebufferStatusEXT");
    if (!glCheckFramebufferStatusExtReal) return GL_FRAMEBUFFER_UNSUPPORTED_;
    return checkFramebufferStatus(target, glCheckFramebufferStatusExtReal);
}
// ---- client render target (loader-62) ------------------------------------------------------------------------------------// An interface size above 100% gives the client a window smaller than the surface, and its picture must then fill the whole
// screen. Copying that picture out of the window framebuffer every present is the obvious way and is what loader-45..61
// did, but ps5-opengl treats the window framebuffer as a display target: the copy drops to a per-pixel CPU loop, measured
// at about 330 ms for 3072x1728 (a 3 fps game at 125%). Instead the client's binds of framebuffer 0 are redirected to a
// texture of the client's own window size. Its picture never leaves the GPU: present only draws that texture scaled over
// the surface, and the game's own read-backs and copies come from a render target, which is on the fast path.
typedef void (*FboBindFn)(FboEnum, FboUint);
typedef void (*FboGetIntegervFn)(FboEnum, FboInt *);
static FboBindFn glBindFramebufferReal, glBindFramebufferExtReal;
static FboGetIntegervFn glGetIntegervReal;
static void (*glBindTextureReal)(FboEnum, FboUint);
static void (*glTexParameteriReal)(FboEnum, FboEnum, FboInt);
static FboUint client_target_fbo, client_target_texture, client_target_depth;
static FboInt client_target_w, client_target_h;
static _Atomic bool client_target_on;
static EGLSurface client_surface[2];    // loader-75: two pbuffers, so present reads the previous frame instead of waiting for the one just drawn
static unsigned client_surface_index;   // the one the client draws into; the other is the read surface and holds the last frame
static bool client_surface_current;      // the client's context is drawing into it (not the window)
static unsigned client_surface_w, client_surface_h;
static bool clientSurfaceActive(void) { return client_surface[0] != EGL_NO_SURFACE; }
static _Atomic unsigned client_clear_calls, client_depth_clears, client_stencil_enables;
static void (*glClearReal)(FboEnum);
static void (*glPushAttribReal)(FboEnum);
static void (*glPopAttribReal)(void);
static void (*glDepthMaskReal)(FboBool);
static void (*glClearDepthReal)(double);
static void (*glDisableReal)(FboEnum);
static bool clientTargetOn(void) { return atomic_load(&client_target_on); }
static void clientTargetReset(void) {
    if (clientSurfaceActive()) {
        if (client_surface_current && egl_display != EGL_NO_DISPLAY && egl_context != EGL_NO_CONTEXT)
            eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
        if (egl_display != EGL_NO_DISPLAY)
            for (unsigned i = 0; i < 2; ++i) eglDestroySurface(egl_display, client_surface[i]);
    }
    client_surface[0] = client_surface[1] = EGL_NO_SURFACE;
    client_surface_index = 0;
    client_surface_current = false;
    client_surface_w = client_surface_h = 0;
    atomic_store(&client_target_on, false);
    client_target_fbo = client_target_texture = client_target_depth = 0;
    client_target_w = client_target_h = 0;
}
// loader-70: an EGL pbuffer surface is a real window-system framebuffer to the client (framebuffer 0 is a surface, with the
// config's depth and stencil, not a user object the loader redirected), and its picture is a render target the driver can
// copy to a texture on the GPU. loader-75: two of them - the client draws into one while the other is the read surface and
// holds the previous frame, so the copy never waits for the frame that was just drawn (that wait was ~5 ms of the frame).
static bool clientSurfaceCreate(FboInt width, FboInt height) {
    if (clientSurfaceActive() && client_surface_w == (unsigned)width && client_surface_h == (unsigned)height) return true;
    if (egl_display == EGL_NO_DISPLAY || !egl_config) return false;
    const EGLint attributes[] = {EGL_WIDTH, (EGLint)width, EGL_HEIGHT, (EGLint)height, EGL_NONE};
    EGLSurface first = eglCreatePbufferSurface(egl_display, egl_config, attributes);
    EGLSurface second = first == EGL_NO_SURFACE ? EGL_NO_SURFACE : eglCreatePbufferSurface(egl_display, egl_config, attributes);
    if (first == EGL_NO_SURFACE || second == EGL_NO_SURFACE) {
        diagnosticsTrace("client.pbuffer=FAIL size=%dx%d egl_error=0x%x", width, height, eglGetError());
        if (first != EGL_NO_SURFACE) eglDestroySurface(egl_display, first);
        if (second != EGL_NO_SURFACE) eglDestroySurface(egl_display, second);
        return false;
    }
    if (clientSurfaceActive())
        for (unsigned i = 0; i < 2; ++i) eglDestroySurface(egl_display, client_surface[i]);
    client_surface[0] = first;
    client_surface[1] = second;
    client_surface_index = 0;
    client_surface_w = (unsigned)width;
    client_surface_h = (unsigned)height;
    client_surface_current = egl_context != EGL_NO_CONTEXT && eglMakeCurrent(egl_display, first, second, egl_context);
    diagnosticsTrace("client.pbuffer=ON size=%dx%d pair current=%d", width, height, client_surface_current ? 1 : 0);
    return true;
}
// Called once the GL context is current and the surface size is known; `width` x `height` is the client's window. The
// depth buffer is Z32F because that is the only depth format ps5-opengl stores (loader-60), and the client asked for no
// stencil. The client starts its first frame with this target bound, so even a client that never binds 0 explicitly draws
// into it.
static void clientTargetCreate(FboInt width, FboInt height) {
    if (clientTargetOn() && client_target_w == width && client_target_h == height) return;
    if (width < 1 || height < 1) return;
    loadFboFunction(&glBindFramebufferReal, "glBindFramebuffer");
    loadFboFunction(&glBindTextureReal, "glBindTexture");
    loadFboFunction(&glTexParameteriReal, "glTexParameteri");
    loadFboFunction(&glCheckFramebufferStatusReal, "glCheckFramebufferStatus");
    if (!glBindFramebufferReal || !glBindTextureReal || !glTexParameteriReal || !glCheckFramebufferStatusReal) {
        diagnosticsTrace("client.target=FAIL reason=entry points");
        return;
    }
    FboUint fbo = 0, texture = 0, depth = 0;
    glGenTexturesCompat(1, &texture);
    glBindTextureReal(GL_TEXTURE_2D_, texture);
    glTexParameteriReal(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
    glTexParameteriReal(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
    glTexParameteriReal(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    glTexParameteriReal(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    glTexImage2DCompat(GL_TEXTURE_2D_, 0, GL_RGBA_, width, height, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, NULL);
    glGenRenderbuffersCompat(1, &depth);
    glRenderbufferStorageCompat(GL_RENDERBUFFER_, GL_DEPTH_COMPONENT32F_, (FboSize)width, (FboSize)height);
    glGenFramebuffersCompat(1, &fbo);
    glBindFramebufferReal(GL_FRAMEBUFFER_, fbo);
    glFramebufferTexture2DCompat(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, texture, 0);
    glFramebufferRenderbufferCompat(GL_FRAMEBUFFER_, GL_DEPTH_ATTACHMENT_, GL_RENDERBUFFER_, depth);
    FboEnum status = glCheckFramebufferStatusReal(GL_FRAMEBUFFER_);
    if (!fbo || !texture || !depth || status != GL_FRAMEBUFFER_COMPLETE_) {
        diagnosticsTrace("client.target=FAIL size=%dx%d status=0x%x", width, height, status);
        glBindFramebufferReal(GL_FRAMEBUFFER_, 0);
        if (fbo) glDeleteFramebuffersCompat(1, &fbo);
        if (texture) glDeleteTexturesCompat(1, &texture);
        if (depth) glDeleteRenderbuffersCompat(1, &depth);
        return;
    }
    client_target_fbo = fbo;
    client_target_texture = texture;
    client_target_depth = depth;
    client_target_w = width;
    client_target_h = height;
    atomic_store(&client_target_on, true);
    diagnosticsTrace("client.target=ON size=%dx%d texture=%u depth=%u fbo=%u", width, height, texture, depth, fbo);
    glBindFramebufferReal(GL_FRAMEBUFFER_, fbo);  // the client's first frame lands in its own picture, not the display target
}
// loader-69: a magenta marker of the real window was used to prove where the client draws; removed in loader-70 (the
// pbuffer makes the question moot and the clear/readback cost frame time).
// The depth/stencil a texture keeps between frames is the window framebuffer's one real difference: the driver's swap does
// not carry the display target's depth to the next frame, while a texture keeps whatever the last frame left. The client
// relies on that (it clears colour every frame but not depth), so without this clear its depth-tested sprites - the player,
// NPCs, some objects - fail against the previous frame's depth in interiors. Clearing depth after present gives the next
// frame the same clean start the window had. The client's own glClear is logged too, to say whether both clear.
static void clientTargetClearDepth(void) {
    if (!clientTargetOn() || !client_target_fbo) return;  // a pbuffer has its own depth and the client clears it
    loadFboFunction(&glClearReal, "glClear");
    loadFboFunction(&glPushAttribReal, "glPushAttrib");
    loadFboFunction(&glPopAttribReal, "glPopAttrib");
    loadFboFunction(&glDepthMaskReal, "glDepthMask");
    loadFboFunction(&glClearDepthReal, "glClearDepth");
    loadFboFunction(&glDisableReal, "glDisable");
    if (!glClearReal || !glPushAttribReal || !glPopAttribReal || !glDepthMaskReal || !glClearDepthReal || !glDisableReal) return;
    glBindFramebufferReal(GL_FRAMEBUFFER_, client_target_fbo);
    glPushAttribReal(GL_ALL_ATTRIB_BITS_);
    glDisableReal(GL_SCISSOR_TEST_);
    glDepthMaskReal(1);
    glClearDepthReal(1.0);  // the GL default the display target's depth starts at
    glClearReal(GL_DEPTH_BUFFER_BIT_);
    glPopAttribReal();
    atomic_fetch_add(&client_depth_clears, 1);
}
// The client's own clears, for the log: a depth clear of its own would mean this repair was not the missing piece.
static void glClearCompat(FboEnum mask) {
    loadFboFunction(&glClearReal, "glClear");
    if (!glClearReal) return;
    if (atomic_fetch_add(&client_clear_calls, 1) < 8)
        diagnosticsTrace("client.clear mask=0x%x depth=%d", mask, (mask & GL_DEPTH_BUFFER_BIT_) != 0);
    glClearReal(mask);
}
// Whether the client is drawing into the loader's texture right now (the draw binding is our target).
static bool clientTargetBound(void) {
    if (!clientTargetOn() || !client_target_fbo) return false;
    loadFboFunction(&glGetIntegervReal, "glGetIntegerv");
    FboInt bound = 0;
    if (glGetIntegervReal) glGetIntegervReal(GL_FRAMEBUFFER_BINDING_, &bound);
    return bound == (FboInt)client_target_fbo;
}
static _Atomic unsigned draw_buffer_maps;
// The client selects its buffers as if the window were bound: GL_BACK is the window's draw/read buffer, and on a texture
// target it is GL_INVALID_OPERATION. It was the *only* GL error in the texture path (the window path logs none), and a
// pass that selects its buffer before drawing is exactly where an error check would abandon it. Map it to the colour
// attachment the texture has, so the call means what the client intended at both sizes.
static void glDrawBufferCompat(FboEnum mode) {
    static void (*real)(FboEnum);
    loadFboFunction(&real, "glDrawBuffer");
    if (!real) return;
    if (mode == GL_BACK_ && clientTargetBound()) {
        if (atomic_fetch_add(&draw_buffer_maps, 1) < 4) diagnosticsTrace("client.drawbuffer back->attachment0");
        mode = GL_COLOR_ATTACHMENT0_;
    } else if (mode == GL_BACK_ && client_surface_current && clientSurfaceActive()) {
        mode = GL_FRONT_;  // loader-70: a pbuffer is single-buffered; GL_BACK is not one of its buffers
    }
    real(mode);
}
static void glReadBufferCompat(FboEnum mode) {
    static void (*real)(FboEnum);
    loadFboFunction(&real, "glReadBuffer");
    if (!real) return;
    if (mode == GL_BACK_ && clientTargetBound()) {
        if (atomic_fetch_add(&draw_buffer_maps, 1) < 8) diagnosticsTrace("client.readbuffer back->attachment0");
        mode = GL_COLOR_ATTACHMENT0_;
    } else if (mode == GL_BACK_ && client_surface_current && clientSurfaceActive()) {
        mode = GL_FRONT_;
    }
    real(mode);
}
static void glDrawBuffersCompat(FboSize n, const FboEnum *buffers) {
    typedef void (*Fn)(FboSize, const FboEnum *);
    static Fn real;
    loadFboFunction(&real, "glDrawBuffers");
    if (!real || !buffers) return;
    FboEnum mapped[8];
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    bool changed = false;
    for (FboSize i = 0; i < n; ++i) {
        mapped[i] = buffers[i];
        if (mapped[i] == GL_BACK_ && clientTargetBound()) {
            mapped[i] = GL_COLOR_ATTACHMENT0_;
            changed = true;
        }
    }
    if (changed && atomic_fetch_add(&draw_buffer_maps, 1) < 12) diagnosticsTrace("client.drawbuffers back->attachment0");
    real(n, changed ? mapped : buffers);
}
static void glBindFramebufferCompat(FboEnum target, FboUint framebuffer) {
    loadFboFunction(&glBindFramebufferReal, "glBindFramebuffer");
    if (!glBindFramebufferReal) return;
    if (framebuffer == 0 && clientTargetOn() &&
        (target == GL_FRAMEBUFFER_ || target == GL_DRAW_FRAMEBUFFER_ || target == GL_READ_FRAMEBUFFER_))
        framebuffer = client_target_fbo;
    glBindFramebufferReal(target, framebuffer);
}
static void glBindFramebufferExtCompat(FboEnum target, FboUint framebuffer) {
    loadFboFunction(&glBindFramebufferExtReal, "glBindFramebufferEXT");
    if (!glBindFramebufferExtReal) return;
    if (framebuffer == 0 && clientTargetOn() &&
        (target == GL_FRAMEBUFFER_ || target == GL_DRAW_FRAMEBUFFER_ || target == GL_READ_FRAMEBUFFER_))
        framebuffer = client_target_fbo;
    glBindFramebufferExtReal(target, framebuffer);
}
// The client saves and restores the binding it found (libGDX does this around its own render targets), so it must keep
// seeing 0 where it is drawing into what it believes is the window. The driver also answers 0 for the depth/stencil bits
// of a texture target (the loader's own "created" line showed depth=0 stencil=0) while the window answers 32 and 8: an
// engine that asks takes its no-depth path and stops drawing what a depth buffer would hold (the player, NPCs, objects).
// Answer what the window would, so both paths are indistinguishable to the client.
static void glGetIntegervCompat(FboEnum pname, FboInt *data) {
    loadFboFunction(&glGetIntegervReal, "glGetIntegerv");
    if (!glGetIntegervReal) return;
    glGetIntegervReal(pname, data);
    if (!clientTargetOn() || !data) return;
    if (clientSurfaceActive() && client_surface_current) {
        // loader-70: a pbuffer is single-buffered; the client was given a double-buffered window and selects GL_BACK.
        switch (pname) {
            case GL_DOUBLEBUFFER_: *data = 1; break;
            case GL_READ_BUFFER_:
            case GL_DRAW_BUFFER_: *data = GL_BACK_; break;
            default: break;
        }
        return;
    }
    FboInt bound = 0;
    glGetIntegervReal(GL_FRAMEBUFFER_BINDING_, &bound);
    if (bound != (FboInt)client_target_fbo) return;
    switch (pname) {
        case GL_DEPTH_BITS_: *data = 32; break;
        case GL_STENCIL_BITS_: *data = 8; break;
        case GL_DOUBLEBUFFER_: *data = 1; break;
        case GL_READ_BUFFER_:
        case GL_DRAW_BUFFER_: *data = GL_BACK_; break;
        case GL_FRAMEBUFFER_BINDING_:
        case GL_READ_FRAMEBUFFER_BINDING_: *data = 0; break;
        default: break;
    }
}
// Whether the client uses a stencil buffer at all (it asked for 0 bits, so this should stay quiet): logged because the
// driver cannot give a texture target a stencil buffer, and a client that used one would lose what it masks.
static void (*glEnableReal)(FboEnum);
static void glEnableCompat(FboEnum cap) {
    loadFboFunction(&glEnableReal, "glEnable");
    if (!glEnableReal) return;
    if (cap == GL_STENCIL_TEST_ && atomic_fetch_add(&client_stencil_enables, 1) < 4) diagnosticsTrace("client.stencil=on");
    glEnableReal(cap);
}
static void logGlObjects(unsigned frame) {
    diagnosticsTrace("gl.objects frame=%u textures=%d renderbuffers=%d framebuffers=%d buffers=%d upload_mib=%llu made=%d freed=%d",
                     frame, atomic_load(&live_textures), atomic_load(&live_renderbuffers), atomic_load(&live_framebuffers),
                     atomic_load(&live_buffers), atomic_load(&texture_upload_kib) / 1024u, atomic_load(&total_textures_generated),
                     atomic_load(&total_textures_deleted));
    diagnosticsTrace("gl.depth_targets frame=%u made=%u raw_mib=%llu padded_mib=%llu", frame, atomic_load(&depth_target_count),
                     atomic_load(&depth_target_bytes) / (1024u * 1024u), atomic_load(&depth_target_padded) / (1024u * 1024u));
    diagnosticsTrace("gl.attachments frame=%u targets=%d padded_mib=%llu", frame, atomic_load(&attached_targets),
                     atomic_load(&attached_target_bytes) / (1024u * 1024u));
}
static void *glEntryPoint(const char *name) {
    if (!strcmp(name, "glCompileShader")) return (void *)glCompileShaderLogging;
    if (!strcmp(name, "glLinkProgram")) return (void *)glLinkProgramLogging;
    if (!strcmp(name, "glRenderbufferStorage")) return (void *)glRenderbufferStorageCompat;
    if (!strcmp(name, "glRenderbufferStorageEXT")) return (void *)glRenderbufferStorageExtCompat;
    if (!strcmp(name, "glRenderbufferStorageMultisample")) return (void *)glRenderbufferStorageMultisampleCompat;
    if (!strcmp(name, "glRenderbufferStorageMultisampleEXT")) return (void *)glRenderbufferStorageMultisampleExtCompat;
    if (!strcmp(name, "glTexImage2D")) return (void *)glTexImage2DCompat;
    if (!strcmp(name, "glTexImage3D")) return (void *)glTexImage3DCompat;
    if (!strcmp(name, "glTexStorage2D")) return (void *)glTexStorage2DCompat;
    if (!strcmp(name, "glTexStorage3D")) return (void *)glTexStorage3DCompat;
    if (!strcmp(name, "glTexImage2DMultisample")) return (void *)glTexImage2DMultisampleCompat;
    if (!strcmp(name, "glTexStorage2DMultisample")) return (void *)glTexStorage2DMultisampleCompat;
    if (!strcmp(name, "glCheckFramebufferStatus")) return (void *)glCheckFramebufferStatusCompat;
    if (!strcmp(name, "glCheckFramebufferStatusEXT")) return (void *)glCheckFramebufferStatusExtCompat;
    if (!strcmp(name, "glBindFramebuffer")) return (void *)glBindFramebufferCompat;
    if (!strcmp(name, "glBindFramebufferEXT")) return (void *)glBindFramebufferExtCompat;
    if (!strcmp(name, "glBindFramebufferARB")) return (void *)glBindFramebufferCompat;  // loader-69: aliases, same redirect
    if (!strcmp(name, "glBindFramebufferOES")) return (void *)glBindFramebufferCompat;
    if (!strcmp(name, "glGetIntegerv")) return (void *)glGetIntegervCompat;
    if (!strcmp(name, "glClear")) return (void *)glClearCompat;
    if (!strcmp(name, "glEnable")) return (void *)glEnableCompat;
    if (!strcmp(name, "glDrawBuffer")) return (void *)glDrawBufferCompat;
    if (!strcmp(name, "glReadBuffer")) return (void *)glReadBufferCompat;
    if (!strcmp(name, "glDrawBuffers")) return (void *)glDrawBuffersCompat;
    if (!strcmp(name, "glFramebufferTexture2D")) return (void *)glFramebufferTexture2DCompat;
    if (!strcmp(name, "glFramebufferTexture")) return (void *)glFramebufferTextureCompat;
    if (!strcmp(name, "glFramebufferRenderbuffer")) return (void *)glFramebufferRenderbufferCompat;
    if (!strcmp(name, "glNamedFramebufferTexture")) return (void *)glNamedFramebufferTextureCompat;
    if (!strcmp(name, "glNamedFramebufferRenderbuffer")) return (void *)glNamedFramebufferRenderbufferCompat;
    if (!strcmp(name, "glGenTextures")) return (void *)glGenTexturesCompat;
    if (!strcmp(name, "glDeleteTextures")) return (void *)glDeleteTexturesCompat;
    if (!strcmp(name, "glGenRenderbuffers")) return (void *)glGenRenderbuffersCompat;
    if (!strcmp(name, "glDeleteRenderbuffers")) return (void *)glDeleteRenderbuffersCompat;
    if (!strcmp(name, "glGenRenderbuffersEXT")) return (void *)glGenRenderbuffersExtCompat;
    if (!strcmp(name, "glDeleteRenderbuffersEXT")) return (void *)glDeleteRenderbuffersExtCompat;
    if (!strcmp(name, "glGenFramebuffers")) return (void *)glGenFramebuffersCompat;
    if (!strcmp(name, "glDeleteFramebuffers")) return (void *)glDeleteFramebuffersCompat;
    if (!strcmp(name, "glGenFramebuffersEXT")) return (void *)glGenFramebuffersExtCompat;
    if (!strcmp(name, "glDeleteFramebuffersEXT")) return (void *)glDeleteFramebuffersExtCompat;
    if (!strcmp(name, "glGenBuffers")) return (void *)glGenBuffersCompat;
    if (!strcmp(name, "glDeleteBuffers")) return (void *)glDeleteBuffersCompat;
    return resolveGlFunction(name);
}
static void logGlCapabilities(const char *when) {
    loadGlDiagnostics();
    if (!glGetStringDiagnostic || !glGetIntegervDiagnostic) return;
    GlInt depth = 0, stencil = 0, red = 0, green = 0, blue = 0, alpha = 0, samples = 0, sample_buffers = 0, max_texture = 0, texture_units = 0, attribs = 0;
    glGetIntegervDiagnostic(GL_DEPTH_BITS_, &depth);
    glGetIntegervDiagnostic(GL_STENCIL_BITS_, &stencil);
    glGetIntegervDiagnostic(GL_RED_BITS_, &red);
    glGetIntegervDiagnostic(GL_GREEN_BITS_, &green);
    glGetIntegervDiagnostic(GL_BLUE_BITS_, &blue);
    glGetIntegervDiagnostic(GL_ALPHA_BITS_, &alpha);
    glGetIntegervDiagnostic(GL_SAMPLES_, &samples);
    glGetIntegervDiagnostic(GL_SAMPLE_BUFFERS_, &sample_buffers);
    glGetIntegervDiagnostic(GL_MAX_TEXTURE_SIZE_, &max_texture);
    glGetIntegervDiagnostic(GL_MAX_TEXTURE_IMAGE_UNITS_, &texture_units);
    glGetIntegervDiagnostic(GL_MAX_VERTEX_ATTRIBS_, &attribs);
    diagnosticsTrace("sdl.GL_Context when=%s vendor=\"%.96s\" renderer=\"%.160s\" version=\"%.96s\" glsl=\"%.96s\"", when, glText(GL_VENDOR_),
                     glText(GL_RENDERER_), glText(GL_VERSION_), glText(GL_SHADING_LANGUAGE_VERSION_));
    diagnosticsTrace("sdl.GL_Framebuffer when=%s rgba=%d/%d/%d/%d depth=%d stencil=%d samples=%d sample_buffers=%d max_texture=%d texture_units=%d attribs=%d", when,
                     red, green, blue, alpha, depth, stencil, samples, sample_buffers, max_texture, texture_units, attribs);
}
static const char *glErrorName(GlEnum code) {
    switch (code) {
        case GL_INVALID_ENUM_: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE_: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION_: return "GL_INVALID_OPERATION";
        case GL_STACK_OVERFLOW_: return "GL_STACK_OVERFLOW";
        case GL_STACK_UNDERFLOW_: return "GL_STACK_UNDERFLOW";
        case GL_OUT_OF_MEMORY_: return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION_: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default: return "GL_ERROR";
    }
}
// glGetError clears what it reads: called every 60 frames, so a recurring driver error is reported (and counted) without
// hiding the game's own checks for long.
static void checkGlErrors(unsigned frame, const char *when) {
    if (!glGetErrorDiagnostic) return;
    static GlEnum codes[8];
    static unsigned counts[8], kinds;
    for (unsigned drains = 0; drains < 8; ++drains) {
        GlEnum error = glGetErrorDiagnostic();
        if (!error) break;
        unsigned slot = 0;
        while (slot < kinds && codes[slot] != error) ++slot;
        if (slot == kinds && kinds < 8) {
            codes[kinds] = error;
            counts[kinds] = 0;
            ++kinds;
        }
        if (slot >= kinds) {
            diagnosticsTrace("sdl.GL_Error when=%s frame=%u code=0x%x %s (not recorded: too many kinds)", when, frame, error, glErrorName(error));
            continue;
        }
        ++counts[slot];
        if (counts[slot] <= 3 || counts[slot] % 600 == 0)
            diagnosticsTrace("sdl.GL_Error when=%s frame=%u code=0x%x %s occurrences=%u", when, frame, error, glErrorName(error), counts[slot]);
    }
}
static void logEglConfig(EGLDisplay display, EGLConfig config) {
    EGLint id = 0, red = 0, green = 0, blue = 0, alpha = 0, depth = 0, stencil = 0, samples = 0, sample_buffers = 0;
    eglGetConfigAttrib(display, config, EGL_CONFIG_ID, &id);
    eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &alpha);
    eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE, &depth);
    eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE, &stencil);
    eglGetConfigAttrib(display, config, EGL_SAMPLES, &samples);
    eglGetConfigAttrib(display, config, EGL_SAMPLE_BUFFERS, &sample_buffers);
    diagnosticsTrace("sdl.GL_Config id=%d rgba=%d/%d/%d/%d depth=%d stencil=%d samples=%d sample_buffers=%d", id, red, green, blue, alpha, depth, stencil,
                     samples, sample_buffers);
}
// SDL_GL_GetAttribute must answer what the window really has, not what was asked for: a client that reads back a 0-bit
// depth or stencil buffer takes fallbacks that look like broken graphics.
static void adoptGlConfig(EGLDisplay display, EGLConfig config) {
    const struct {
        int attribute;
        EGLint egl;
    } map[] = {{GL_ATTRIBUTE_RED, EGL_RED_SIZE},
               {GL_ATTRIBUTE_GREEN, EGL_GREEN_SIZE},
               {GL_ATTRIBUTE_BLUE, EGL_BLUE_SIZE},
               {GL_ATTRIBUTE_ALPHA, EGL_ALPHA_SIZE},
               {GL_ATTRIBUTE_DEPTH, EGL_DEPTH_SIZE},
               {GL_ATTRIBUTE_STENCIL, EGL_STENCIL_SIZE},
               {GL_ATTRIBUTE_MULTISAMPLE_BUFFERS, EGL_SAMPLE_BUFFERS},
               {GL_ATTRIBUTE_MULTISAMPLE_SAMPLES, EGL_SAMPLES}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        EGLint value = 0;
        if (eglGetConfigAttrib(display, config, map[i].egl, &value)) {
            gl_attributes[map[i].attribute] = value;
            gl_attribute_set[map[i].attribute] = true;
        }
    }
}
static bool chooseGlConfig(EGLDisplay display, bool extras, EGLConfig *config, EGLint *count) {
    EGLint attributes[32];
    unsigned n = 0;
    const EGLint fixed[][2] = {{EGL_SURFACE_TYPE, EGL_WINDOW_BIT},
                               {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT},
                               {EGL_RED_SIZE, glAttribute(GL_ATTRIBUTE_RED, 8)},
                               {EGL_GREEN_SIZE, glAttribute(GL_ATTRIBUTE_GREEN, 8)},
                               {EGL_BLUE_SIZE, glAttribute(GL_ATTRIBUTE_BLUE, 8)},
                               {EGL_ALPHA_SIZE, glAttribute(GL_ATTRIBUTE_ALPHA, 8)},
                               {EGL_DEPTH_SIZE, glAttribute(GL_ATTRIBUTE_DEPTH, 24)}};
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        attributes[n++] = fixed[i][0];
        attributes[n++] = fixed[i][1];
    }
    if (extras) {
        int stencil = glAttribute(GL_ATTRIBUTE_STENCIL, 0);
        if (stencil > 0) {
            attributes[n++] = EGL_STENCIL_SIZE;
            attributes[n++] = stencil;
        }
        if (glAttribute(GL_ATTRIBUTE_MULTISAMPLE_BUFFERS, 0) > 0) {
            attributes[n++] = EGL_SAMPLE_BUFFERS;
            attributes[n++] = 1;
            attributes[n++] = EGL_SAMPLES;
            attributes[n++] = glAttribute(GL_ATTRIBUTE_MULTISAMPLE_SAMPLES, 4);
        }
    }
    attributes[n++] = EGL_NONE;
    return eglChooseConfig(display, attributes, config, 1, count) && *count > 0;
}

// Frame pacing: a zone change that stalls the game thread shows up as one long interval or a long eglSwapBuffers.
// Judder without a long stall (a 60 Hz frame that misses its vblank) never reaches the stall threshold, so count the
// intervals in buckets per 600-frame window as well.
static uint64_t last_swap_ns, slowest_swap_ns;
static unsigned frame_stalls, slow_frames;
static unsigned pace_17, pace_25, pace_33, pace_50, pace_100, pace_max_ms;
// loader-88: the last frame's totals for the client's file reads and its texture uploads, for the stall breakdown.
static unsigned long long stall_read_calls, stall_read_bytes, stall_read_ns, stall_upload_calls, stall_upload_ns, stall_recv_ns;
static unsigned long long frame_wall_ns, frame_cpu_ns, frame_wait_ns;
// loader-85: how the intervals are actually spread. The threshold counts say "late" but not "steadily late" (every frame
// at 20 ms, which is a cadence the client's limiter chose) or "bimodal" (16.7 and 33.3, which is missed frames), and the
// two need opposite fixes, so a 1 ms histogram of the window is logged with the counts.
#define FRAME_HIST_BUCKETS 51u
static unsigned frame_hist[FRAME_HIST_BUCKETS];
static void noteFrameTime(uint64_t interval_ns, uint64_t present_ns, uint64_t cpu_ns, unsigned frame) {
    if (interval_ns > slowest_swap_ns) slowest_swap_ns = interval_ns;
    // loader-89: what the frame interval was made of: CPU time on the client's own thread, and the rest (waiting).
    frame_wall_ns += interval_ns;
    frame_cpu_ns += cpu_ns;
    if (interval_ns > cpu_ns) frame_wait_ns += interval_ns - cpu_ns;
    if (frame > 2 && interval_ns) {
        unsigned ms = (unsigned)(interval_ns / 1000000ull);
        if (ms > pace_max_ms) pace_max_ms = ms;
        if (ms > 17) ++pace_17;
        if (ms > 25) ++pace_25;
        if (ms > 33) ++pace_33;
        if (ms > 50) ++pace_50;
        if (ms > 100) ++pace_100;
        ++frame_hist[ms < FRAME_HIST_BUCKETS ? ms : FRAME_HIST_BUCKETS - 1u];
    }
    // Judder without a half-second stall: log the frame numbers so a hitch can be lined up with what the client was doing.
    if (frame > 2 && interval_ns > 40000000ull && slow_frames < 300) {
        ++slow_frames;
        diagnosticsTrace("sdl.frame.slow frame=%u interval_ms=%llu present_ms=%llu cpu_ms=%llu wait_ms=%llu", frame,
                         (unsigned long long)(interval_ns / 1000000ull), (unsigned long long)(present_ns / 1000000ull),
                         (unsigned long long)(cpu_ns / 1000000ull),
                         (unsigned long long)((interval_ns > cpu_ns ? interval_ns - cpu_ns : 0) / 1000000ull));
    }
    // loader-88: what filled the interval: the client's own file reads, its texture uploads, or neither (its own parsing).
    // The snapshot is taken every frame so a stalled interval's work is exactly the difference from the frame before it.
    unsigned long long reads = 0, read_bytes = 0, read_ns = 0;
    linuxFilesReadTotals(&reads, &read_bytes, &read_ns);
    unsigned long long recvs = 0, recv_bytes = 0, recv_ns = 0;
    linuxNetRecvTotals(&recvs, &recv_bytes, &recv_ns);
    unsigned long long uploads = atomic_load(&texture_upload_calls), upload_ns = atomic_load(&texture_upload_ns);
    // The upload counters restart every 600 frames, so a smaller reading means "just restarted", not a negative.
    unsigned long long d_reads = reads >= stall_read_calls ? reads - stall_read_calls : 0;
    unsigned long long d_read_bytes = read_bytes >= stall_read_bytes ? read_bytes - stall_read_bytes : 0;
    unsigned long long d_read_ns = read_ns >= stall_read_ns ? read_ns - stall_read_ns : 0;
    unsigned long long d_recv_ns = recv_ns >= stall_recv_ns ? recv_ns - stall_recv_ns : 0;
    unsigned long long d_uploads = uploads >= stall_upload_calls ? uploads - stall_upload_calls : 0;
    unsigned long long d_upload_ns = upload_ns >= stall_upload_ns ? upload_ns - stall_upload_ns : 0;
    stall_read_calls = reads;
    stall_read_bytes = read_bytes;
    stall_read_ns = read_ns;
    stall_recv_ns = recv_ns;
    stall_upload_calls = uploads;
    stall_upload_ns = upload_ns;
    if (frame <= 2 || (interval_ns <= 500000000ull && present_ns <= 250000000ull)) return;
    ++frame_stalls;
    if (frame_stalls <= 24 || frame_stalls % 100 == 0)
        diagnosticsTrace("sdl.frame.stall frame=%u interval_ms=%llu present_ms=%llu stalls=%u cpu_ms=%llu wait_ms=%llu reads=%llu "
                         "read_ms=%llu read_kib=%llu uploads=%llu upload_ms=%llu recv_ms=%llu",
                         frame, (unsigned long long)(interval_ns / 1000000u), (unsigned long long)(present_ns / 1000000u), frame_stalls,
                         (unsigned long long)(cpu_ns / 1000000ull),
                         (unsigned long long)((interval_ns > cpu_ns ? interval_ns - cpu_ns : 0) / 1000000ull), d_reads,
                         d_read_ns / 1000000ull, d_read_bytes / 1024ull, d_uploads, d_upload_ns / 1000000ull,
                         d_recv_ns / 1000000ull);
}

static void *sdlGlCreateContext(void *window) {
    (void)window;
    if (display_acquire) display_acquire();  // the loading screen still has the one window surface: wait until it lets go
    EGLConfig config;
    EGLint count = 0;
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, NULL, NULL)) {
        trace("sdl.GL_CreateContext=FAIL stage=initialize egl_error=0x%x", eglGetError());
        return NULL;
    }
    diagnosticsTrace("sdl.GL_Request rgba=%d/%d/%d/%d depth=%d stencil=%d samples=%d buffers=%d", glAttribute(GL_ATTRIBUTE_RED, 8),
                     glAttribute(GL_ATTRIBUTE_GREEN, 8), glAttribute(GL_ATTRIBUTE_BLUE, 8), glAttribute(GL_ATTRIBUTE_ALPHA, 8), glAttribute(GL_ATTRIBUTE_DEPTH, 24),
                     glAttribute(GL_ATTRIBUTE_STENCIL, 0), glAttribute(GL_ATTRIBUTE_MULTISAMPLE_SAMPLES, 4), glAttribute(GL_ATTRIBUTE_MULTISAMPLE_BUFFERS, 0));
    bool bound = eglBindAPI(EGL_OPENGL_API);
    bool chosen = bound && chooseGlConfig(egl_display, true, &config, &count);
    if (!chosen && bound && (glAttribute(GL_ATTRIBUTE_STENCIL, 0) > 0 || glAttribute(GL_ATTRIBUTE_MULTISAMPLE_BUFFERS, 0) > 0)) {
        diagnosticsTrace("sdl.GL_Config=RETRY without stencil/multisampling egl_error=0x%x", eglGetError());
        chosen = chooseGlConfig(egl_display, false, &config, &count);
    }
    if (!chosen) {
        trace("sdl.GL_CreateContext=FAIL stage=config egl_error=0x%x", eglGetError());
        return NULL;
    }
    egl_config = config;
    logEglConfig(egl_display, config);
    adoptGlConfig(egl_display, config);
    egl_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)0, NULL);  // ps5-opengl: native window 0, no attributes
    if (egl_surface == EGL_NO_SURFACE) {
        trace("sdl.GL_CreateContext=FAIL stage=surface egl_error=0x%x", eglGetError());
        return NULL;
    }
    int major = glAttribute(GL_ATTRIBUTE_CONTEXT_MAJOR, 2), minor = glAttribute(GL_ATTRIBUTE_CONTEXT_MINOR, 1);
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, major, EGL_CONTEXT_MINOR_VERSION_KHR, minor, EGL_NONE};
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
    if (egl_context == EGL_NO_CONTEXT) {
        trace("sdl.GL_CreateContext=FAIL stage=context egl_error=0x%x", eglGetError());
        return NULL;
    }
    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        trace("sdl.GL_CreateContext=FAIL stage=make_current egl_error=0x%x", eglGetError());
        return NULL;
    }
    eglSwapInterval(egl_display, (EGLint)swap_interval);
    EGLint w = 0, h = 0;
    if (eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &w) && eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &h) && w > 0 && h > 0) {
        surface_width = (unsigned)w;
        surface_height = (unsigned)h;
        refreshMode();
    }
    // loader-62: at an interface size above 100% the client draws into the loader's texture (see "client render target").
    if (render_scale > 1.0f && !upscale_copy && surface_width > 0 && surface_height > 0) {
        unsigned cw = 0, ch = 0;
        clientScreenSize(&cw, &ch);
        if (clientSurfaceCreate((FboInt)cw, (FboInt)ch)) {
            atomic_store(&client_target_on, true);
        } else {
            clientTargetCreate((FboInt)cw, (FboInt)ch);  // fallback: a texture target with the binds redirected
        }
    } else {
        clientTargetReset();
        if (render_scale > 1.0f) diagnosticsTrace("client.target=OFF copy path (upscale-copy switch)");
    }
    ++contexts_created;
    trace("sdl.GL_CreateContext=PASS version=%d.%d context=%p", major, minor, egl_context);
    logGlCapabilities("created");
    checkGlErrors(0, "created");
    last_swap_ns = platformMonotonicNs();  // the first frame is not a stall
    return egl_context;
}
static bool sdlGlMakeCurrent(void *window, void *context) {
    (void)window;
    if (egl_display == EGL_NO_DISPLAY) return false;
    if (!context) {
        client_surface_current = false;
        return eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (clientSurfaceActive()) {  // loader-70: the client's context draws into its own surface pair, not the window
        client_surface_current = eglMakeCurrent(egl_display, client_surface[client_surface_index],
                                                client_surface[1 - client_surface_index], (EGLContext)context);
        return client_surface_current;
    }
    return eglMakeCurrent(egl_display, egl_surface, egl_surface, (EGLContext)context);
}
static void *sdlGlGetCurrentContext(void) { return eglGetCurrentContext() == EGL_NO_CONTEXT ? NULL : eglGetCurrentContext(); }
static bool sdlGlDestroyContext(void *context) {
    (void)context;
    return true;
}  // torn down by linuxSdlReset
static bool sdlGlSetSwapInterval(int interval) {
    trace("sdl.GL_SetSwapInterval %d", interval);
    // loader-50: PokeMMO asks for adaptive vsync (-1) while its V-Sync setting is on, and only falls back to a plain 0
    // because -1 looked unsupported. The old clamp to 0 erased the request outright and, worse, told the client through
    // GL_GetSwapInterval that sync was off - so the client paced itself with its own 60 fps limiter, and the small sleep
    // it takes each frame drifts against the panel. That was the steady judder: the pacing counters showed about 20
    // frames in every 600 landing at 25-29 ms, with no stall to explain it. The console's display is a fixed 60 Hz and
    // the client caps at 60 fps, so one vblank is the right cadence: honour what the client asks, but never below one.
    swap_interval = interval > 1 ? interval : 1;
    return egl_display == EGL_NO_DISPLAY || eglSwapInterval(egl_display, (EGLint)swap_interval);
}
static bool sdlGlGetSwapInterval(int *interval) {
    if (interval) *interval = (int)swap_interval;
    return true;
}
void linuxSdlRequestSize(unsigned width, unsigned height) { atomic_store(&requested_size, width << 16 | height); }
// The picture changes size when the console is docked or undocked. The surface of a window cannot change size while it is in use, so it is
// replaced (the context and everything the game created in it stay) and the game is told that its window was resized. Done by the thread that
// renders, between two frames. If the new surface cannot be made, the old size is made again.
static void applySizeRequest(void) { atomic_store(&requested_size, 0); }  // the PS5 picture has one size
// PS5: the loading screen goes on over the game's own frames until they show something (the client draws black frames
// while it loads its data), checked every tenth frame by reading a few pixels back; at most three minutes.
static void (*loading_draw)(void);
static void (*first_picture)(void);
void linuxSdlSetFirstPicture(void (*callback)(void)) { first_picture = callback; }
static atomic_bool loading_active;
static uint64_t loading_since_ns;
void linuxSdlSetLoadingOverlay(void (*draw)(void)) {
    loading_draw = draw;
    loading_since_ns = platformMonotonicNs();
    atomic_store(&loading_active, draw != NULL);
}
static bool drawLoadingOverlay(unsigned w, unsigned h, unsigned frame) {
    if (!atomic_load(&loading_active)) return false;
    bool timed_out = platformMonotonicNs() - loading_since_ns > 180000000000ull;
    // loader-75: say so if the client has not shown a picture for 20 s ("stuck on Starting PokeMMO" after an update): the
    // log then has the moment and the game state, instead of only the session coming after it.
    static bool slow_logged;
    if (!slow_logged && platformMonotonicNs() - loading_since_ns > 20000000000ull) {
        slow_logged = true;
        diagnosticsTrace("sdl.loading_overlay=SLOW frame=%u elapsed_ms=%llu (no picture yet; Triangle opens Settings > Log)",
                         frame, (unsigned long long)((platformMonotonicNs() - loading_since_ns) / 1000000ull));
    }
    // The content check runs after the upscale on present, so even with a render scale the client's picture fills the
    // surface and the points below are its usual relative positions.
    if (timed_out || (frame % 10 == 0 && overlayPictureHasContent(w, h))) {
        atomic_store(&loading_active, false);
        trace("sdl.loading_overlay=DONE frame=%u reason=%s", frame, timed_out ? "time limit" : "the game shows a picture");
        if (first_picture) first_picture();  // the game got as far as drawing: a new client revision has proven itself
        return false;
    }
    if (overlayBegin(w, h)) {
        loading_draw();
        overlayEnd();
    }
    return true;
}
// L1/R1 move the selection along the client's own hotbar; Square uses the selected slot.  The client binds the slots
// to 1-9 (client.controls.gdx.hotbar1..9) but has no pad binding for using one, so the loader presses the digit itself.
// The client's hotbar is the UI: the loader only draws a thin marker around the selected cell.  A tap of L1/R1 moves the
// selection one slot; a hold pages the whole hotbar (client.controls.gdx.previous/next, PAGE_UP/PAGE_DOWN) once and then
// one page per repeat.  The d-pad is the client's own (KEY_UP/DOWN/LEFT/RIGHT): it walks, navigates the client's windows
// and drives the battle command and move grid, so the loader does not take it.
#define HOTBAR_PREV_SCANCODE 75u  // SDL_SCANCODE_PAGEUP: client.controls.gdx.previous
#define HOTBAR_NEXT_SCANCODE 78u  // SDL_SCANCODE_PAGEDOWN: client.controls.gdx.next
#define HOTBAR_HOLD_NS 450000000ull
#define HOTBAR_REPEAT_NS 300000000ull
static unsigned hotbar_selected = 1;
// The marker stays hidden until the client's own bar is on screen and the player has worked it (padProbeHotbar marks the
// bar's cells when the client draws them, and the first L1/R1 or Square press marks the rest).  The client reports
// no "logged in" of its own (its window title is always "PokeMMO"), so those two signs are what the loader has.
static bool hotbar_marker_visible;
static bool hotbar_cells_visible;  // the client drew its own hotbar cells (set by padProbeHotbar)
static struct {
    unsigned slot;
    uint64_t release_ns;
} hotbar_pending;
// One button of the bar: it pages once when held past the hold time, and then once per repeat.
typedef struct {
    bool down, paged;
    uint64_t down_ns, page_ns;
} HotbarPager;
static void hotbarRelease(uint64_t now) {
    if (!hotbar_pending.slot) return;
    uint32_t scan = 29u + hotbar_pending.slot, key = (uint32_t)('0' + hotbar_pending.slot);
    linuxSdlEventsPushKey(scan, key, false, now);
    hotbar_pending.slot = 0;
}
static void hotbarPage(uint32_t scancode, uint64_t now, const char *page) {
    uint32_t key = linuxSdlKeyFromScancode(scancode);
    linuxSdlEventsPushKey(scancode, key, true, now);
    linuxSdlEventsPushKey(scancode, key, false, now);
    hotbar_marker_visible = true;
    diagnosticsTrace("hotbar.page=%s", page);
}
static void hotbarPager(HotbarPager *p, bool held, uint64_t now, uint32_t scancode, const char *page) {
    if (held && !p->down) {
        p->down_ns = now;
        p->paged = false;
    }
    if (held && p->down && !p->paged && p->down_ns && now - p->down_ns > HOTBAR_HOLD_NS) {  // held: page the hotbar
        p->paged = true;
        p->page_ns = now + HOTBAR_REPEAT_NS;
        hotbarPage(scancode, now, page);
    } else if (held && p->paged && now >= p->page_ns) {  // still held: one more page per repeat
        p->page_ns = now + HOTBAR_REPEAT_NS;
        hotbarPage(scancode, now, page);
    }
    p->down = held;
}
static void hotbarShortcuts(LinuxInputSnapshot *snapshot) {
    static uint32_t last_buttons;
    static bool seen;
    static HotbarPager l1, r1;
    if (!seen) {  // a button held while the client starts is not a press
        last_buttons = snapshot->buttons;
        seen = true;
    }
    uint32_t pressed = snapshot->buttons & ~last_buttons;
    last_buttons = snapshot->buttons;
    uint64_t now = snapshot->timestamp_ns;
    if (hotbar_pending.slot && now >= hotbar_pending.release_ns) hotbarRelease(now);
    bool l1_held = (snapshot->buttons & (1u << 9)) != 0, r1_held = (snapshot->buttons & (1u << 10)) != 0;
    if (!l1_held && l1.down) {  // a tap: move the selection one cell
        if (l1.down_ns && !l1.paged) {
            hotbar_selected = hotbar_selected > 1 ? hotbar_selected - 1 : 9;
            hotbar_marker_visible = true;
            diagnosticsTrace("hotbar.select=%u", hotbar_selected);
        }
        l1.down_ns = 0;
    }
    if (!r1_held && r1.down) {
        if (r1.down_ns && !r1.paged) {
            hotbar_selected = hotbar_selected < 9 ? hotbar_selected + 1 : 1;
            hotbar_marker_visible = true;
            diagnosticsTrace("hotbar.select=%u", hotbar_selected);
        }
        r1.down_ns = 0;
    }
    hotbarPager(&l1, l1_held, now, HOTBAR_PREV_SCANCODE, "previous");
    hotbarPager(&r1, r1_held, now, HOTBAR_NEXT_SCANCODE, "next");
    if (pressed & (1u << 2)) {  // Square uses the slot
        hotbarRelease(now);  // a quick second press must not leave the first key held
        uint32_t scan = 29u + hotbar_selected, key = (uint32_t)('0' + hotbar_selected);
        linuxSdlEventsPushKey(scan, key, true, now);
        hotbar_pending.slot = hotbar_selected;
        hotbar_pending.release_ns = now + 90000000ull;
        hotbar_marker_visible = true;
        diagnosticsTrace("hotbar.use=%u", hotbar_selected);
    }
    // These buttons belong to the bar, not to the game: L1/R1 are not bound in the client, and Square must not also
    // reach it as KEY_X.  The hardware d-pad is left in the gamepad state: the client's controller map binds
    // KEY_UP/DOWN/LEFT/RIGHT to those buttons, so the client navigates with it exactly as it does with the left stick.
    snapshot->buttons &= ~((1u << 9) | (1u << 10) | (1u << 2));
}
// The client's own HUD. The layout numbers live in linux_sdl.h (client.gui.hud.hotkeybar.x/.y's design units), so the
// marker the loader draws and the position the loader writes (main_ps5.c) cannot drift apart.  The overlay draws in a
// virtual 1920-wide, 1080-high space.  The client draws its design units at its UI scale (ui_zoom2, normally 2.0; the
// loader follows the client's log if its own slider moves it) and its window is the surface divided by the render scale,
// whose picture the loader then scales back over the surface: design units on screen = design x UI scale x render scale,
// and overlay units = design x UI scale x render scale x (1920 or 1080) / surface.  At 4K, 200% and a render scale of 1
// all factors are 1.
static float padScaleX(unsigned width) { return padGuiScale() * (float)render_scale * 1920.0f / (float)width; }
static float padScaleY(unsigned height) { return padGuiScale() * (float)render_scale * 1080.0f / (float)height; }
static float padViewportWidth(unsigned width) { return 1920.0f / padScaleX(width); }
static float padViewportHeight(unsigned height) { return 1080.0f / padScaleY(height); }
// The hotbar is anchored bottom centre by main_ps5.c (client.gui.hud.hotkeybar.anchor, client 34702 replaced the old
// x/.y), so the client centres the frame in the design viewport it draws in.  The marker follows that: the client scales
// the frame with its own ui_zoom2, which the render scale then enlarges.
static float padHotbarFrameX(unsigned width) { return (float)width / (padGuiScale() * (float)render_scale) / 2.0f - LINUX_SDL_HOTBAR_FRAME_W / 2.0f; }
static float padHotbarFrameY(unsigned height) { return (float)height / (padGuiScale() * (float)render_scale) - LINUX_SDL_HOTBAR_FRAME_H; }
// The battle scene's move details.  The client shows a move's Power and Accuracy only while the pointer is over the move,
// and has no key for it.  The loader therefore keeps the pointer on the move the client has selected (its own yellow
// frame), so the summary is there without a button.  The panel's measured layout, in design units at a 1080-high
// viewport: a 1328x758 panel whose bottom edge is viewport height - chatframe.height - 127.  main_ps5.c's
// applyHudDefaults writes a 34-unit chat box, which centres it exactly; all offsets below are for that chat box and scale
// with the viewport height, as the panel does at an interface size above 1.
#define PAD_CHATFRAME_HEIGHT 34.0f
static float padBattleScale(unsigned height) { return padViewportHeight(height) / 1080.0f; }
static float padBattleBX(unsigned width, unsigned height) { return padViewportWidth(width) / 2.0f + 580.5f * padBattleScale(height); }
static float padBattleBY(unsigned height) { return padViewportHeight(height) - (PAD_CHATFRAME_HEIGHT + 148.5f) * padBattleScale(height); }
// Cell centres of the 2x2 move grid, and the top border bands that show the client's selected cell.
static float padMoveCellX(unsigned width, unsigned height, unsigned column) { return padViewportWidth(width) / 2.0f - (column ? 347.0f : 564.0f) * padBattleScale(height); }
static float padMoveCellY(unsigned height, unsigned row) { return padViewportHeight(height) - (row ? 188.25f : 241.75f) * padBattleScale(height); }
static float padMoveRowTop(unsigned height, unsigned row) { return padViewportHeight(height) - (row ? 212.5f : 266.0f) * padBattleScale(height); }
static bool pad_battle_visible;  // the client's own "B GO BACK" hint is on screen (set by padProbeBKeycap)
// loader-48: the hardware d-pad is no longer intercepted.  It now reaches the client exactly as the left stick's own
// d-pad does (linux_sdl_input.c: both are SDL buttons 11..14), so every one of the client's GUIs navigates with it the
// same way it always has with the stick.
// Park the pointer on a move, or put it back where it was.  This is the summary: the client draws its own tooltip while
// the pointer is over the selected move.  Coordinates are design units; the client's window pixels are design x UI scale.
static bool pad_move_hovering;
static float pad_move_hover_x, pad_move_hover_y;
static float pad_move_saved_x, pad_move_saved_y;
static void padMoveHover(bool show, float design_x, float design_y) {
    if (linuxSdlCursorActive()) {  // loader-60: the player's pointer owns the mouse position, so the summary parking yields
        if (pad_move_hovering) {
            pad_move_hovering = false;
            diagnosticsTrace("sdl.movesummary=off pointer");
        }
        return;
    }
    uint64_t now = platformMonotonicNs();
    if (show && !pad_move_hovering) {
        linuxSdlEventsMouse(&pad_move_saved_x, &pad_move_saved_y);
        pad_move_hovering = true;
        pad_move_hover_x = pad_move_hover_y = -1e9f;
        diagnosticsTrace("sdl.movesummary=on");
    }
    if (!show) {
        if (pad_move_hovering) {
            linuxSdlEventsWarpMouse(pad_move_saved_x, pad_move_saved_y, now);
            pad_move_hovering = false;
            diagnosticsTrace("sdl.movesummary=off");
        }
        return;
    }
    float scale = padGuiScale();
    float x = design_x * scale, y = design_y * scale;
    if (fabsf(x - pad_move_hover_x) < 1.0f && fabsf(y - pad_move_hover_y) < 1.0f) return;
    linuxSdlEventsWarpMouse(x, y, now);
    pad_move_hover_x = x;
    pad_move_hover_y = y;
}
// Circle is the client's own B (bound in the controller map), so it closes a window and steps back in the battle UI, as
// the client's "B GO BACK" hints say.  Not every panel answers B: the catch screen only draws an X in a corner.  Holding
// Circle for 700 ms therefore sends Escape as well, once per hold (loader-60) - the keyboard's own close, which the
// client's dialog windows listen for.  A tap or an ordinary press-release still sends only B, so nothing closes twice.
// L2 is the client's KEY_RING_FOCUS (loader-59): it focuses the HUD ring, which brings up the party hotbar on the right.
// The client's own menus (the login and register screens, its dialogs) are keyboard screens: the client
// focuses the first field itself, typing goes to it, Tab steps to the next field and Return activates the
// button that is drawn.  Those keys have no pad binding in the client's controller map, so R2 is the menus' keyboard: a
// tap is Return, a still hold steps with Tab, one per repeat, and a hold that moves the left stick is the pointer (loader-60).
#define PAD_UI_RETURN_SCANCODE 40u  // SDL_SCANCODE_RETURN
#define PAD_UI_RETURN_KEYCODE '\r'
#define PAD_UI_TAB_SCANCODE 43u  // SDL_SCANCODE_TAB
#define PAD_UI_TAB_KEYCODE '\t'
#define PAD_UI_ESCAPE_SCANCODE 41u  // SDL_SCANCODE_ESCAPE
#define PAD_UI_ESCAPE_KEYCODE 27
#define PAD_UI_TAB_HOLD_NS 600000000ull
#define PAD_UI_TAB_REPEAT_NS 450000000ull
#define PAD_UI_ESCAPE_HOLD_NS 700000000ull
#define PAD_UI_STICK_TILT 5000
static void padUiShortcuts(LinuxInputSnapshot *snapshot, unsigned width, unsigned height) {
    (void)width;  // the interface is the client's own; the pad only reaches it with keys
    (void)height;
    static bool r2_down, r2_tabbed;
    static uint64_t r2_down_ns, r2_tab_ns;
    static bool circle_down, circle_escaped;
    static uint64_t circle_down_ns;
    static bool touchpad_down;
    uint64_t now = snapshot->timestamp_ns;
    // The touchpad's click is the client's own KEY_INVENTORY (the bag). Log its edges: the client stopped opening the bag,
    // and this says whether the loader still sees the press at all.
    bool touchpad = (snapshot->buttons & (1u << 4)) != 0;
    if (touchpad != touchpad_down) diagnosticsTrace("sdl.touchpad=%s", touchpad ? "press" : "release");
    touchpad_down = touchpad;
    // The move summary needs no button: padProbeMoveSelection keeps the pointer on the selected battle move while the
    // move grid is on screen, and lets it go when the grid is gone.  L3 stays out of the client's map.
    snapshot->buttons &= ~(1u << 7);  // L3 belongs to the loader; the client map leaves it unbound
    bool r2 = snapshot->axes[5] > 16000;  // the right trigger: a tap is Return, a still hold is Tab, a hold that steers is the pointer
    bool stick = snapshot->axes[0] > PAD_UI_STICK_TILT || snapshot->axes[0] < -PAD_UI_STICK_TILT ||
                 snapshot->axes[1] > PAD_UI_STICK_TILT || snapshot->axes[1] < -PAD_UI_STICK_TILT;
    if (r2 && !r2_down) {
        r2_down_ns = now;
        r2_tabbed = false;
    }
    // Once the pointer is armed (or the stick is already asking for it), R2 is not a keyboard key: no Tab and no Return.
    bool cursor = linuxSdlCursorR2Armed() || (r2 && stick);
    if (cursor && r2_down) r2_tabbed = true;  // a hold that became the pointer is not a tap, so releasing it sends no Return
    if (!cursor && r2 && r2_down && !r2_tabbed && r2_down_ns && now - r2_down_ns > PAD_UI_TAB_HOLD_NS) {
        r2_tabbed = true;  // held still: step to the next field, then one more Tab per repeat
        r2_tab_ns = now + PAD_UI_TAB_REPEAT_NS;
        linuxSdlEventsPushKey(PAD_UI_TAB_SCANCODE, PAD_UI_TAB_KEYCODE, true, now);
        linuxSdlEventsPushKey(PAD_UI_TAB_SCANCODE, PAD_UI_TAB_KEYCODE, false, now);
        diagnosticsTrace("sdl.tab=r2");
    } else if (!cursor && r2 && r2_tabbed && now >= r2_tab_ns) {
        r2_tab_ns = now + PAD_UI_TAB_REPEAT_NS;
        linuxSdlEventsPushKey(PAD_UI_TAB_SCANCODE, PAD_UI_TAB_KEYCODE, true, now);
        linuxSdlEventsPushKey(PAD_UI_TAB_SCANCODE, PAD_UI_TAB_KEYCODE, false, now);
        diagnosticsTrace("sdl.tab=r2");
    }
    if (!r2 && r2_down) {
        if (r2_down_ns && !r2_tabbed) {  // a tap: Return, which activates the button the client has drawn
            linuxSdlEventsPushKey(PAD_UI_RETURN_SCANCODE, PAD_UI_RETURN_KEYCODE, true, now);
            linuxSdlEventsPushKey(PAD_UI_RETURN_SCANCODE, PAD_UI_RETURN_KEYCODE, false, now);
            diagnosticsTrace("sdl.return=r2");
        }
        r2_down_ns = 0;
    }
    r2_down = r2;
    // Circle: the client gets B from the button itself.  A hold adds Escape once, for a panel whose only way out is a
    // corner X that B ignores; a tap never sends it, so an ordinary press-release cannot close two windows.
    bool circle = (snapshot->buttons & (1u << 1)) != 0;
    if (circle && !circle_down) {
        circle_down_ns = now;
        circle_escaped = false;
    }
    if (circle && circle_down && !circle_escaped && circle_down_ns && now - circle_down_ns > PAD_UI_ESCAPE_HOLD_NS) {
        circle_escaped = true;
        linuxSdlEventsPushKey(PAD_UI_ESCAPE_SCANCODE, PAD_UI_ESCAPE_KEYCODE, true, now);
        linuxSdlEventsPushKey(PAD_UI_ESCAPE_SCANCODE, PAD_UI_ESCAPE_KEYCODE, false, now);
        diagnosticsTrace("sdl.escape=hold-circle");
    }
    if (!circle && circle_down) circle_down_ns = 0;
    circle_down = circle;
}
// The client says nothing about which of its screens is up, so the loader reads a few pixels of the picture back and
// recognises the client's own "B GO BACK" key-cap (a white ring with a dark letter), which it draws only on the battle
// scene.  While it is there the pad's battle hints are drawn and the pointer follows the selected move.  The block is read
// every 15th frame, and a change needs two readings in a row, so a single odd frame cannot flicker anything.
// loader-61: a read-back waits for the frame the game is still drawing, so each probe takes its turn on its own frame
// (three probes, one every 15 frames each, never two in the same frame) and all of them stand aside while the left stick
// is deflected.  The world scrolls exactly when that stick is held, and a probe's wait landing on that frame was a hitch
// the player could see; the screens being probed cannot change shape mid-step.  The cost is logged as sdl.probe.pace.
// loader-82: the read itself no longer waits at all - the block is queued into a pixel-pack buffer and collected on a
// later frame (overlayReadBackQueue/overlayReadBackFetch), so a probe no longer costs a frame while the scene is busy.
#define PAD_PROBE_EVERY 15u
#define PAD_PROBE_STICK 8000
static _Atomic bool pad_stick_moving;  // the left stick is deflected: the player is walking, so the probes wait
static bool probeDue(unsigned frame, unsigned offset) { return frame % PAD_PROBE_EVERY == offset; }
#define PAD_PROBE_BRIGHT 150u
#define PAD_PROBE_DARK 110u
static void padProbeBKeycap(unsigned width, unsigned height, unsigned frame) {
    static uint8_t pixels[256 * 256 * 4];
    static OverlayReadBack slot;
    static unsigned agree;
    if (width == 0 || height == 0) return;
    unsigned block_w = 0, block_h = 0;
    bool fresh = overlayReadBackFetch(&slot, pixels, sizeof(pixels), &block_w, &block_h);
    if (!fresh && probeDue(frame, 0) && !atomic_load(&pad_stick_moving)) {
        float kx = padScaleX(width), ky = padScaleY(height), scale = padBattleScale(height);
        float bx = padBattleBX(width, height), by = padBattleBY(height), half = 10.0f * scale;
        if (!overlayReadBackQueue(&slot, width, height, (bx - half) * kx, (by - half) * ky, 2.0f * half * kx,
                                  2.0f * half * ky, sizeof(pixels), frame))
            fresh = overlayReadBack(width, height, (bx - half) * kx, (by - half) * ky, 2.0f * half * kx,
                                    2.0f * half * ky, pixels, sizeof(pixels), &block_w, &block_h);
    }
    if (!fresh || block_w < 8 || block_h < 8) return;
    static const float ring[4][2] = {{0.225f, 0.5f}, {0.775f, 0.5f}, {0.5f, 0.225f}, {0.5f, 0.775f}};
    unsigned bright = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned x = (unsigned)(ring[i][0] * (float)(block_w - 1) + 0.5f), y = (unsigned)(ring[i][1] * (float)(block_h - 1) + 0.5f);
        const uint8_t *p = pixels + ((size_t)y * block_w + x) * 4u;
        if (p[0] > PAD_PROBE_BRIGHT || p[1] > PAD_PROBE_BRIGHT || p[2] > PAD_PROBE_BRIGHT) ++bright;
    }
    const uint8_t *centre = pixels + ((size_t)(block_h / 2) * block_w + block_w / 2) * 4u;
    bool dark = centre[0] < PAD_PROBE_DARK && centre[1] < PAD_PROBE_DARK && centre[2] < PAD_PROBE_DARK;
    bool now = bright >= 3 && dark;
    if (now == pad_battle_visible) {
        agree = 0;
        return;
    }
    if (++agree < 2) return;
    agree = 0;
    pad_battle_visible = now;
    if (!now) padMoveHover(false, 0, 0);
    diagnosticsTrace("sdl.battle=%s", now ? "on" : "off");
}
// The client's own hotbar cells are the other thing the loader must see for itself: the marker may only be drawn while
// the bar is actually on screen, so that it cannot sit over the login screen or the character list (a logout returns
// there without the title restarting, so the first-touch flag alone is not enough).  The client draws its slots as the
// same neutral dark grey, which nothing on its other screens matches: a band across four cells after the first is read
// back, three points per cell, and most of them must be that grey.
#define PAD_HOTBAR_GREY_MAX 150u
#define PAD_HOTBAR_GREY_SPREAD 35u
#define PAD_HOTBAR_BAND_CELLS 4u
#define PAD_HOTBAR_BAND_W ((float)(PAD_HOTBAR_BAND_CELLS * LINUX_SDL_HOTBAR_PITCH))
static void padProbeHotbar(unsigned width, unsigned height, unsigned frame) {
    static uint8_t pixels[2048 * 128 * 4];
    static OverlayReadBack slot;
    static unsigned agree;
    if (width == 0 || height == 0) return;
    unsigned block_w = 0, block_h = 0;
    bool fresh = overlayReadBackFetch(&slot, pixels, sizeof(pixels), &block_w, &block_h);
    if (!fresh && probeDue(frame, 5) && !atomic_load(&pad_stick_moving)) {
        // The band sits just under the cells' top edge, clear of the slot number and of an item's icon. It covers four slots
        // after the first: a majority of points is enough, and a shorter band keeps the read-back small at a large size.
        float kx = padScaleX(width), ky = padScaleY(height);
        float cell_x = padHotbarFrameX(width) + LINUX_SDL_HOTBAR_INSET_X;
        float cell_y = padHotbarFrameY(height) + LINUX_SDL_HOTBAR_INSET_Y;
        float left = (cell_x + LINUX_SDL_HOTBAR_PITCH + 6.0f) * kx;
        float top = (cell_y + 4.0f) * ky;
        if (!overlayReadBackQueue(&slot, width, height, left, top, PAD_HOTBAR_BAND_W * kx, 8.0f * ky, sizeof(pixels), frame))
            fresh = overlayReadBack(width, height, left, top, PAD_HOTBAR_BAND_W * kx, 8.0f * ky, pixels, sizeof(pixels),
                                    &block_w, &block_h);
    }
    if (!fresh || block_w < 8 || block_h < 3) return;
    unsigned grey = 0, samples = 0;
    for (unsigned cell = 0; cell < PAD_HOTBAR_BAND_CELLS; ++cell) {
        float fx = ((float)LINUX_SDL_HOTBAR_PITCH * ((float)cell + 0.15f)) / PAD_HOTBAR_BAND_W;
        unsigned x = (unsigned)(fx * (float)(block_w - 1) + 0.5f);
        if (x >= block_w) x = block_w - 1;
        for (unsigned row = 0; row < 3; ++row) {
            unsigned y = (unsigned)(((float)row * 0.35f + 0.12f) * (float)(block_h - 1) + 0.5f);
            const uint8_t *p = pixels + ((size_t)y * block_w + x) * 4u;
            unsigned most = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
            unsigned least = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
            ++samples;
            if (most < PAD_HOTBAR_GREY_MAX && most - least < PAD_HOTBAR_GREY_SPREAD) ++grey;
        }
    }
    bool now = samples && grey * 2 > samples;  // most of the points
    if (now == hotbar_cells_visible) {
        agree = 0;
        return;
    }
    if (++agree < 2) return;
    agree = 0;
    hotbar_cells_visible = now;
    diagnosticsTrace("sdl.hotbarcells=%s", now ? "on" : "off");
}
// The battle move selector: the client marks the move the pad has selected with a bright yellow frame.  Read the top
// border of both rows, find which cell has it, and keep the pointer there: the client then shows that move's Power and
// Accuracy by itself (the move summary), with no L3 and no click.  The pad moves the client's own selection (its
// direction buttons, below), so the pointer just follows whatever the client marks.
static void padProbeMoveSelection(unsigned width, unsigned height, unsigned frame) {
    static struct {
        uint8_t pixels[4096 * 32 * 4];
        OverlayReadBack slot;
    } rows[2];
    if (width == 0 || height == 0) return;
    if (!pad_battle_visible) {  // the battle move summary only exists while the battle scene is up
        padMoveHover(false, 0, 0);
        return;
    }
    float kx = padScaleX(width), ky = padScaleY(height), scale = padBattleScale(height);
    float left = padViewportWidth(width) / 2.0f - 670.5f * scale;
    float band_w = 429.0f * scale, band_h = 5.0f * scale;
    bool due = probeDue(frame, 10) && !atomic_load(&pad_stick_moving);
    int found_row = -1, found_column = -1;
    unsigned settled = 0;
    for (unsigned row = 0; row < 2; ++row) {
        unsigned block_w = 0, block_h = 0;
        bool ready = overlayReadBackFetch(&rows[row].slot, rows[row].pixels, sizeof(rows[row].pixels), &block_w, &block_h);
        if (!ready && due) {
            float top = padMoveRowTop(height, row) - 2.0f * scale;
            if (overlayReadBackQueue(&rows[row].slot, width, height, left * kx, top * ky, band_w * kx, band_h * ky,
                                     sizeof(rows[row].pixels), frame))
                continue;  // queued: it is collected on a later frame, and neither row is acted on until both are in
            ready = overlayReadBack(width, height, left * kx, top * ky, band_w * kx, band_h * ky, rows[row].pixels,
                                    sizeof(rows[row].pixels), &block_w, &block_h);
        }
        if (!ready) continue;
        ++settled;
        if (block_w < 8 || block_h < 2) continue;
        const uint8_t *pixels = rows[row].pixels;
        unsigned left_yellow = 0, right_yellow = 0;
        for (unsigned y = 0; y < block_h; ++y) {
            const uint8_t *p = pixels + (size_t)y * block_w * 4u;
            for (unsigned x = 0; x < block_w; ++x, p += 4) {
                if (p[0] > 140 && p[1] > 110 && p[2] < 90 && (int)p[0] - (int)p[2] > 70 && (int)p[1] - (int)p[2] > 50) {
                    if (x * 2 < block_w)
                        ++left_yellow;
                    else
                        ++right_yellow;
                }
            }
        }
        unsigned need = 2u + (unsigned)((float)block_w * 0.02f);
        if (found_row < 0 && left_yellow + right_yellow >= need) {
            found_row = (int)row;
            found_column = left_yellow >= right_yellow ? 0 : 1;
        }
    }
    if (settled < 2) return;  // one row's block is still being copied: leave the pointer where it is
    if (found_row >= 0)
        padMoveHover(true, padMoveCellX(width, height, (unsigned)found_column), padMoveCellY(height, (unsigned)found_row));
    else
        padMoveHover(false, 0, 0);
}
// The battle scene's key hints.  The client shows "B GO BACK" with a B key-cap; the pad's back button is Circle, so a
// Circle is painted over the letter (the client's own hint text cannot be changed).  The move summary is the client's own
// tooltip over the selected move, so there is no hint pill for it any more.
static void drawBattleHints(unsigned width, unsigned height) {
    float kx = padScaleX(width), ky = padScaleY(height), scale = padBattleScale(height);
    float bx = padBattleBX(width, height), by = padBattleBY(height);
    overlayRect((bx - 4.0f * scale) * kx, (by - 4.5f * scale) * ky, 8.0f * scale * kx, 9.0f * scale * ky, 0x2B2B2BF0u);  // hide the letter
    overlayTextCentered(bx * kx, (by - 5.5f * scale) * ky, "\x02", 11.0f * scale * ky, 0xFFFFFFFFu);
}
// The selection on the client's hotbar cell: a thin, light edge with the faintest wash, so it reads as part of the bar
// and only says which slot the pad will use.  Drawn only while the client's own bar is on screen and the player has
// worked it (hotbar_cells_visible, hotbar_marker_visible).
static void drawPadOverlay(unsigned width, unsigned height) {
    if (width == 0 || height == 0) return;
    bool marker = hotbar_marker_visible && hotbar_cells_visible;
    if (!marker && !pad_battle_visible) return;
    if (!overlayBegin(width, height)) return;
    if (marker) {
        float kx = padScaleX(width), ky = padScaleY(height);
        float cell_x = padHotbarFrameX(width) + LINUX_SDL_HOTBAR_INSET_X;
        float cell_y = padHotbarFrameY(height) + LINUX_SDL_HOTBAR_INSET_Y;
        float x = (cell_x + (float)(hotbar_selected - 1) * LINUX_SDL_HOTBAR_PITCH) * kx;
        float y = cell_y * ky;
        float size = LINUX_SDL_HOTBAR_CELL * kx;  // square: kx and ky are equal on the surface's 16:9 picture
        overlayRect(x + 1.5f * kx, y + 1.5f * ky, size - 3.0f * kx, size - 3.0f * ky, 0xE8E8E81Cu);
        overlayFrame(x, y, size, size, 1.5f * kx, 0xE8E8E8B0u);
    }
    if (pad_battle_visible) drawBattleHints(width, height);
    overlayEnd();
}
static bool sdlGlSwapWindow(void *window) {
    (void)window;
    if (egl_display == EGL_NO_DISPLAY || egl_surface == EGL_NO_SURFACE) return false;
    uint64_t began = platformMonotonicNs();
    // loader-89: the CPU time this thread has used since the previous swap. Read here, at the one place the client's own
    // frame ends, so the interval can be split into "computed" and "waited for something else".
    static uint64_t last_thread_cpu_ns;
    uint64_t thread_cpu = platformThreadCpuNs();
    uint64_t cpu_delta_ns = thread_cpu >= last_thread_cpu_ns ? thread_cpu - last_thread_cpu_ns : 0;
    last_thread_cpu_ns = thread_cpu;
    static uint64_t thread_cpu_total_ns;
    thread_cpu_total_ns = thread_cpu;
    unsigned w, h;
    screenSize(&w, &h);
    // loader-68: drain the client's own GL errors before the loader draws anything, so a log line says whose call failed.
    unsigned swap_frame = atomic_load(&frame_counter) + 1;
    if (swap_frame % 60 == 0) checkGlErrors(swap_frame, "client");
    static uint64_t upscale_ns;
    static unsigned upscale_calls;
    static _Atomic unsigned long long capture_ns, switch_ns, draw_ns;
    // loader-45: with a render scale above 1 the client drew its picture into a window smaller than the surface (the
    // bottom-left of the framebuffer, where GL starts). Stretch it over the whole surface first, so the loading screen's
    // content check, the pad's probes and everything the loader draws afterwards all work in the full-screen space.
    // loader-47: timed, because the copy-and-scale is the one cost the interface size adds to every present.
    if (render_scale > 1.0f) {
        unsigned cw, ch;
        clientScreenSize(&cw, &ch);
        uint64_t upscale_began = platformMonotonicNs();
        if (clientSurfaceActive()) {
            // loader-72: timed in three pieces - the capture (which waits for the client's queued draws), the surface
            // switch, and the upscale draw - because the whole present costs about 5.6 ms of the 16.7 ms frame.
            uint64_t t0 = platformMonotonicNs();
            bool captured = overlayCaptureSurface(cw, ch);
            uint64_t t1 = platformMonotonicNs();
            atomic_fetch_add(&capture_ns, t1 - t0);
            if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) == EGL_TRUE) {
                client_surface_current = false;
                uint64_t t2 = platformMonotonicNs();
                atomic_fetch_add(&switch_ns, t2 - t1);
                if (captured) overlayDrawCapturedSurface(w, h);
                atomic_fetch_add(&draw_ns, platformMonotonicNs() - t2);
            }
        } else if (clientTargetOn()) {
            if (overlayCaptureFramebuffer(client_target_fbo, cw, ch)) overlayDrawCapturedSurface(w, h);  // fallback texture target
        } else {
            overlayUpscale(w, h, cw, ch);
        }
        upscale_ns += platformMonotonicNs() - upscale_began;
        ++upscale_calls;
    }
    if (drawLoadingOverlay(w, h, atomic_load(&frame_counter) + 1)) {
        // the loading screen covers the game's black frames (nothing else is drawn over it)
    } else if (linuxFilePickerOpen())
        linuxFilePickerDraw(w, h);
    else if (oskVisible() || linkBoxVisible()) {  // PS5: the on-screen keyboard and the link box (overlay.c)
        if (overlayBegin(w, h)) {
            oskDraw();
            linkBoxDraw();
            overlayEnd();
        }
    } else
        linuxSdlCursorDraw(w, h);  // the file chooser, the console's own panels or the cursor, over the frame just drawn
    unsigned probe_frame = atomic_load(&frame_counter) + 1;
    if (!atomic_load(&loading_active) && !linuxFilePickerOpen() && !oskVisible() && !linkBoxVisible()) {
        padProbeBKeycap(w, h, probe_frame);        // what the client is showing, before anything of ours is drawn
        padProbeHotbar(w, h, probe_frame);         // one probe per frame, in turn (see above)
        padProbeMoveSelection(w, h, probe_frame);  // the battle move summary: the pointer follows the client's selected move
        drawPadOverlay(w, h);
    }
    bool ok = eglSwapBuffers(egl_display, egl_surface);
    uint64_t ended = platformMonotonicNs();
    if (ok) applySizeRequest();
    if (ok) clientTargetClearDepth();  // give the client's next frame the depth the window's swap would have given it
    if (clientSurfaceActive() && egl_context != EGL_NO_CONTEXT) {
        // loader-75: draw into the other pbuffer next, and read the one just completed; present will copy that one.
        unsigned next = 1 - client_surface_index;
        client_surface_current = eglMakeCurrent(egl_display, client_surface[next], client_surface[client_surface_index], egl_context);
        client_surface_index = next;
    }
    unsigned frame = atomic_fetch_add(&frame_counter, 1) + 1;
    noteFrameTime(last_swap_ns && ended > last_swap_ns ? ended - last_swap_ns : 0, ended > began ? ended - began : 0, cpu_delta_ns, frame);
    last_swap_ns = ended;
    if (frame % 60 == 0) checkGlErrors(frame, "swap");
    if (frame % 600 == 0) {
        diagnosticsTrace("sdl.frame.summary frame=%u slowest_ms=%llu stalls=%u", frame, (unsigned long long)(slowest_swap_ns / 1000000u), frame_stalls);
        diagnosticsTrace("sdl.frame.pace frame=%u over17=%u over25=%u over33=%u over50=%u over100=%u max_ms=%u", frame, pace_17, pace_25,
                         pace_33, pace_50, pace_100, pace_max_ms);
        {
            char hist[512];
            size_t used = 0;
            for (unsigned bucket = 0; bucket < FRAME_HIST_BUCKETS && used + 12 < sizeof(hist); ++bucket) {
                if (!frame_hist[bucket]) continue;
                int wrote = snprintf(hist + used, sizeof(hist) - used, "%s%u:%u", used ? " " : "", bucket, frame_hist[bucket]);
                if (wrote < 1) break;
                used += (size_t)wrote;
                frame_hist[bucket] = 0;
            }
            diagnosticsTrace("sdl.frame.hist frame=%u %s", frame, hist);
        }
        diagnosticsTrace("sdl.frame.cost frame=%u wall_ms=%llu cpu_ms=%llu wait_ms=%llu thread_cpu_s=%llu", frame,
                         (unsigned long long)(frame_wall_ns / 1000000ull), (unsigned long long)(frame_cpu_ns / 1000000ull),
                         (unsigned long long)(frame_wait_ns / 1000000ull), (unsigned long long)(thread_cpu_total_ns / 1000000000ull));
        frame_wall_ns = frame_cpu_ns = frame_wait_ns = 0;
        diagnosticsTrace("gl.upload.pace frame=%u calls=%u cpu_ms=%llu", frame, atomic_load(&texture_upload_calls),
                         (unsigned long long)(atomic_load(&texture_upload_ns) / 1000000ull));
        unsigned drives = 0, drive_max_ms = 0;
        unsigned long long drive_ns = 0;
        overlayReadBackStats(&drives, &drive_ns, &drive_max_ms);
        diagnosticsTrace("sdl.probe.pace frame=%u readbacks=%u cpu_ms=%llu max_ms=%u moving=%d", frame, drives,
                         (unsigned long long)(drive_ns / 1000000ull), drive_max_ms, atomic_load(&pad_stick_moving) ? 1 : 0);
        if (clientTargetOn())
            diagnosticsTrace("client.depth frame=%u loader_clears=%u client_clears=%u stencil_enables=%u", frame,
                             atomic_load(&client_depth_clears), atomic_load(&client_clear_calls), atomic_load(&client_stencil_enables));
        if (upscale_calls || upscale_ns)
            diagnosticsTrace("gl.upscale.pace frame=%u calls=%u cpu_ms=%llu", frame, upscale_calls, (unsigned long long)(upscale_ns / 1000000ull));
        if (clientSurfaceActive())
            diagnosticsTrace("gl.present.pace frame=%u capture_ms=%llu switch_ms=%llu draw_ms=%llu", frame,
                             (unsigned long long)(atomic_exchange(&capture_ns, 0) / 1000000ull),
                             (unsigned long long)(atomic_exchange(&switch_ns, 0) / 1000000ull),
                             (unsigned long long)(atomic_exchange(&draw_ns, 0) / 1000000ull));
        upscale_ns = 0;
        upscale_calls = 0;
        pace_17 = pace_25 = pace_33 = pace_50 = pace_100 = pace_max_ms = 0;
        atomic_store(&texture_upload_ns, 0);
        atomic_store(&texture_upload_calls, 0);
    }
    if (frame % 600 == 0) logGlObjects(frame);
    if (frame == 1 || frame == 2 || frame == 60 || frame == 600 || !ok) trace("sdl.GL_SwapWindow frame=%u result=%s", frame, ok ? "OK" : "FAIL");
    if (!ok) diagnosticsTrace("sdl.GL_SwapWindow=FAIL frame=%u egl_error=0x%x", frame, eglGetError());
    return ok;
}
static void *sdlGlGetProcAddress(const char *name) { return name ? glEntryPoint(name) : NULL; }
static bool sdlGlExtensionSupported(const char *name) {
    (void)name;
    return false;
}

// ---- events, input, time ----------------------------------------------------------------------------------------------------------
// The console's controller and touch screen are sampled when the game pumps events (at most once per millisecond) and
// turned into SDL events by linux_sdl_events.c: touch is a mouse, the controller is gamepad number 1.
static uint8_t keyboard_state[512];  // SDL_GetKeyboardState: indexed by scancode, kept as the key events are polled
static _Atomic uint64_t last_pump_ns;
static bool padAtRest(const LinuxInputSnapshot *snapshot) {  // no button held, sticks and triggers near their rest position
    for (unsigned i = 0; i < LINUX_SDL_GAMEPAD_AXES; ++i)
        if (snapshot->axes[i] > 9000 || snapshot->axes[i] < -9000) return false;
    return snapshot->buttons == 0;
}
// loader-48: the hardware d-pad is left alone.  loader-45..47 translated it into arrow keys (Tab on the login, a pointer
// nudge on panels), but the client's own GUIs are not keyboard screens: its controller map binds KEY_UP/DOWN/LEFT/RIGHT
// to SDL buttons 11..14, exactly the buttons linux_sdl_input.c's linuxSdlInputStickToDpad makes from the left stick - and
// the stick navigates every GUI.  The hardware d-pad reports the same four buttons (buttonBits in linux_sdl_input.c), so
// passing them through unchanged makes the d-pad behave like the stick everywhere: battle, shop and every other window.
// A log line per press is kept, so a future report can show whether the d-pad's own buttons arrive.
static void padDpadTrace(LinuxInputSnapshot *snapshot) {
    static bool seen, down_before[4];
    static const char *const names[4] = {"up", "down", "left", "right"};
    if (!seen) {
        for (unsigned i = 0; i < 4; ++i) down_before[i] = (snapshot->buttons & (1u << (11 + i))) != 0;
        seen = true;
        return;
    }
    for (unsigned i = 0; i < 4; ++i) {
        bool down = (snapshot->buttons & (1u << (11 + i))) != 0;
        if (down != down_before[i]) diagnosticsTrace("sdl.dpad=%s %s", names[i], down ? "down" : "up");
        down_before[i] = down;
    }
}
static void pumpInput(void) {
    linuxSdlInputKeyboardPump();
    uint64_t now = platformMonotonicNs();
    uint64_t before = atomic_load(&last_pump_ns);
    if (now - before < 1000000ull || !atomic_compare_exchange_strong(&last_pump_ns, &before, now)) return;
    LinuxInputSnapshot snapshot;
    static bool keyboard_button_before, keyboard_has_pad;
    if (linuxSdlInputSample(&snapshot)) {
        // loader-61: the probes stand aside while the left stick is deflected. Sample before the cursor zeroes it for the
        // game, so a pointer hold counts as movement too (the read-backs have nothing to learn while it is up).
        atomic_store(&pad_stick_moving,
                     snapshot.gamepad && (snapshot.axes[0] > PAD_PROBE_STICK || snapshot.axes[0] < -PAD_PROBE_STICK ||
                                          snapshot.axes[1] > PAD_PROBE_STICK || snapshot.axes[1] < -PAD_PROBE_STICK));
        if (linuxFilePickerCapturesInput()) {
            // The file chooser owns the controller and the touch screen: the game sees them idle, so that nothing stays pressed behind it.
            linuxFilePickerFeed(&snapshot);
            LinuxInputSnapshot idle = {0};
            idle.gamepad = snapshot.gamepad;
            idle.timestamp_ns = snapshot.timestamp_ns;
            linuxSdlEventsUpdate(&idle);
            keyboard_button_before = true;
            return;
        }
        if (linkBoxVisible()) {  // PS5: the link box has the controller until it is closed, as the file chooser
            linkBoxFeed(&snapshot);
            LinuxInputSnapshot idle = {0};
            idle.gamepad = snapshot.gamepad;
            idle.timestamp_ns = snapshot.timestamp_ns;
            linuxSdlEventsUpdate(&idle);
            keyboard_button_before = true;
            return;
        }
        // Clicking the right stick brings the keyboard up or down, by hand, whatever the game is doing: the game's own requests for text
        // input are not followed (they only came for some fields).
        bool keyboard_button = snapshot.gamepad && ((snapshot.buttons >> 8) & 1u);  // SDL_GAMEPAD_BUTTON_RIGHT_STICK
        if (keyboard_button && !keyboard_button_before && linuxSdlInputKeyboardAvailable()) {
            bool show = !linuxSdlInputKeyboardVisible();
            trace("sdl.keyboard.toggle show=%d", show);
            linuxSdlInputKeyboardRequest(show);
        }
        keyboard_button_before = keyboard_button;
        snapshot.buttons &= ~(1u << 8);  // the keyboard's button is not also a game button
        unsigned width, height;
        screenSize(&width, &height);
        // The console's keyboard uses the controller while it is up: the game sees it at rest until the keyboard is gone and everything is released.
        if (linuxSdlInputKeyboardVisible()) {
            oskFeed(&snapshot);  // PS5: the keyboard is the loader's own (osk.c)
            keyboard_has_pad = true;
        }
        else if (keyboard_has_pad && padAtRest(&snapshot))
            keyboard_has_pad = false;
        if (keyboard_has_pad) {
            snapshot.buttons = 0;
            memset(snapshot.axes, 0, sizeof(snapshot.axes));
        } else {
            hotbarShortcuts(&snapshot);
            padUiShortcuts(&snapshot, width, height);
            padDpadTrace(&snapshot);  // the d-pad's own buttons are left for the client; this only logs their edges
            linuxSdlCursorUpdate(&snapshot, width, height);  // with the cursor up, the sticks are the cursor's (axes zeroed)
            linuxSdlInputStickToDpad(&snapshot);
        }
        linuxSdlEventsUpdate(&snapshot);
    }
}
bool linuxSdlThreadHasContext(void) { return egl_context != EGL_NO_CONTEXT && eglGetCurrentContext() != EGL_NO_CONTEXT; }
bool linuxSdlPresentFrame(void) {
    pumpInput();
    return sdlGlSwapWindow(NULL);
}
static void sdlPumpEvents(void) { pumpInput(); }
static void sdlUpdateJoysticks(void) { pumpInput(); }
static bool sdlPollEvent(void *event) {
    pumpInput();
    if (!event) return linuxSdlEventsPending() > 0 || atomic_load(&stop_requested);
    if (atomic_exchange(&stop_requested, false)) {
        memset(event, 0, 128);
        *(uint32_t *)event = LINUX_SDL_EVENT_QUIT;
        return true;
    }
    bool have = linuxSdlEventsPop(event);
    if (have && event) {
        uint32_t type;
        memcpy(&type, event, sizeof(type));
        if (type == LINUX_SDL_EVENT_KEY_DOWN || type == LINUX_SDL_EVENT_KEY_UP) {
            uint32_t scancode;
            memcpy(&scancode, (unsigned char *)event + 24, sizeof(scancode));
            if (scancode < sizeof(keyboard_state)) keyboard_state[scancode] = ((unsigned char *)event)[36] != 0;
        }
    }
    return have;
}
// Links ("open in the browser"): PS5: the system's web browser opens them (the game goes to the background; the PS button brings
// it back). When the browser cannot be started, the address is shown to type instead (link_box.c).
static bool sdlOpenUrl(const char *url) {
    if (!url || !*url) return false;
    int result = platformOpenUrl(url);
    trace("sdl.OpenURL url=%.200s browser=0x%x", url, (unsigned)result);
    if (result < 0) linkBoxShow(url);
    return true;
}
// Memory streams: the game hands its gamepad database to SDL through one. The mappings are counted and ignored (the controller is described by this project).
static void *sdlIoFromMem(void *memory, size_t size) { return linuxSdlIoFromMemory(memory, size, true); }
static void *sdlIoFromConstMem(const void *memory, size_t size) { return linuxSdlIoFromMemory((void *)memory, size, false); }
static size_t sdlReadIo(void *io, void *buffer, size_t size) { return linuxSdlIoRead(io, buffer, size); }
static size_t sdlWriteIo(void *io, const void *buffer, size_t size) { return linuxSdlIoWrite(io, buffer, size); }
static int64_t sdlSeekIo(void *io, int64_t offset, int whence) { return linuxSdlIoSeek(io, offset, whence); }
static int64_t sdlTellIo(void *io) { return linuxSdlIoTell(io); }
static int64_t sdlGetIoSize(void *io) { return linuxSdlIoSize(io); }
static bool sdlCloseIo(void *io) { return linuxSdlIoClose(io); }
static int sdlAddGamepadMappingsFromIo(void *io, bool close_io) {
    int count = linuxSdlIoCountMappings(io);
    trace("sdl.gamepad.mappings count=%d (accepted, not used)", count);
    if (close_io) linuxSdlIoClose(io);
    return count;
}
static int sdlAddGamepadMapping(const char *mapping) {
    (void)mapping;
    return 0;
}
static int sdlAddGamepadMappingsFromFile(const char *file) {
    (void)file;
    return 0;
}
static void sdlUpdateGamepads(void) { pumpInput(); }
static int sdlJoystickPowerInfo(void *joystick, int *percent) {
    (void)joystick;
    if (percent) *percent = -1;
    return 0;
}  // SDL_POWERSTATE_UNKNOWN
static uint16_t sdlJoystickVendorForId(uint32_t id) {
    (void)id;
    return 0x054c;  // Sony
}
static uint16_t sdlJoystickProductForId(uint32_t id) {
    (void)id;
    return 0x0ce6;  // DualSense
}
static void *sdlCreateColorCursor(void *surface, int hot_x, int hot_y) {
    (void)surface;
    (void)hot_x;
    (void)hot_y;
    static char cursor[8];
    return cursor;
}
// The game's titles are logged once each: the loader has no other cheap sign of which of the client's screens is up,
// and this says whether one day the client names its screens.
static bool sdlSetWindowTitle(void *window, const char *title) {
    (void)window;
    static char last[80];
    static unsigned logged;
    if (title && strcmp(title, last) != 0 && logged++ < 20) {
        snprintf(last, sizeof(last), "%s", title);
        diagnosticsTrace("sdl.title=%.70s", title);
    }
    return true;
}
static uint32_t sdlGetKeyFromScancode(int scancode, uint16_t modifiers, bool key_event) {
    (void)modifiers;
    (void)key_event;
    return scancode < 0 ? 0 : linuxSdlKeyFromScancode((uint32_t)scancode);
}
static const char *sdlGetKeyName(uint32_t key) { return linuxSdlKeyName(key); }
// Text fields: the game's requests are acknowledged and nothing more; the keyboard is the right stick button's (see pumpInput).
static bool sdlStartTextInput(void *window) {
    (void)window;
    trace("sdl.StartTextInput");
    return true;
}
static bool sdlStartTextInputWithProperties(void *window, uint32_t properties) {
    (void)properties;
    return sdlStartTextInput(window);
}
static bool sdlStopTextInput(void *window) {
    (void)window;
    trace("sdl.StopTextInput");
    return true;
}
static bool sdlTextInputActive(void *window) {
    (void)window;
    return linuxSdlInputKeyboardVisible();
}
static uint8_t *sdlGetKeyboardState(int *count) {
    if (count) *count = 512;
    return keyboard_state;
}
static uint32_t sdlGetMouseState(float *x, float *y) {
    pumpInput();
    return linuxSdlEventsMouse(x, y);
}
static uint16_t sdlGetModState(void) { return linuxSdlEventsKeyMods(); }
static uint32_t *sdlEmptyIdList(int *count) {
    if (count) *count = 0;
    return linuxAbiCalloc(2, sizeof(uint32_t));
}
// gamepads: one controller, id 1, while the console reports one
static uint32_t *sdlGamepadIds(int *count) {
    pumpInput();
    int present = linuxSdlEventsGamepadCount();
    if (count) *count = present;
    uint32_t *ids = linuxAbiCalloc(2, sizeof(uint32_t));
    if (ids && present) ids[0] = LINUX_SDL_GAMEPAD_ID;
    return ids;
}
static char gamepad_object[64], joystick_object[64];
static bool sdlIsGamepad(uint32_t id) { return id == LINUX_SDL_GAMEPAD_ID && linuxSdlEventsGamepadCount() > 0; }
static void *sdlOpenGamepad(uint32_t id) {
    trace("sdl.OpenGamepad id=%u", id);
    return sdlIsGamepad(id) ? gamepad_object : NULL;
}
static void *sdlGetGamepadFromId(uint32_t id) { return sdlIsGamepad(id) ? gamepad_object : NULL; }
static void *sdlGetJoystickFromId(uint32_t id) { return sdlIsGamepad(id) ? joystick_object : NULL; }
static void *sdlGetGamepadJoystick(void *gamepad) {
    (void)gamepad;
    return joystick_object;
}
static uint32_t sdlGamepadId(void *object) {
    (void)object;
    return LINUX_SDL_GAMEPAD_ID;
}
// Short on purpose: with a longer name the Android theme of the game loops its layout on the settings pages (lag, and a warning).
#define GAMEPAD_NAME "PS5 Controller"
static const char *sdlGamepadName(void *object) {
    (void)object;
    return GAMEPAD_NAME;
}
static const char *sdlGamepadNameForId(uint32_t id) {
    (void)id;
    return GAMEPAD_NAME;
}
static int sdlGamepadType(void *object) {
    (void)object;
    return 6;
}  // SDL_GAMEPAD_TYPE_PS5
static int sdlGamepadTypeForId(uint32_t id) {
    (void)id;
    return 6;
}
static int sdlJoystickType(void *object) {
    (void)object;
    return 1;
}  // SDL_JOYSTICK_TYPE_GAMEPAD
static uint16_t sdlGamepadVendor(void *object) {
    (void)object;
    return 0x054c;  // Sony
}
static uint16_t sdlGamepadProduct(void *object) {
    (void)object;
    return 0x0ce6;  // DualSense
}
static uint16_t sdlGamepadZero16(void *object) {
    (void)object;
    return 0;
}
static int sdlGamepadZeroInt(void *object) {
    (void)object;
    return 0;
}
static bool sdlGamepadButton(void *object, int button) {
    (void)object;
    return linuxSdlEventsGamepadButton(button);
}
static int16_t sdlGamepadAxis(void *object, int axis) {
    (void)object;
    return linuxSdlEventsGamepadAxis(axis);
}
static bool sdlGamepadHasButton(void *object, int button) {
    (void)object;
    return button >= 0 && button < LINUX_SDL_GAMEPAD_BUTTONS;
}
static bool sdlGamepadHasAxis(void *object, int axis) {
    (void)object;
    return axis >= 0 && axis < LINUX_SDL_GAMEPAD_AXES;
}
static bool sdlHasGamepad(void) { return linuxSdlEventsGamepadCount() > 0; }
static bool sdlGamepadConnected(void *object) {
    (void)object;
    return linuxSdlEventsGamepadCount() > 0;
}
static uint64_t ticksNs(void) { return platformMonotonicNs(); }
// The game waits for the next frame by reading the clock in a loop (millions of calls): every few calls yield the core so that
// the other threads (audio, loading, network) are not kept waiting by it.
static atomic_uint clock_reads;
static void clockYield(void) {
    if ((atomic_fetch_add_explicit(&clock_reads, 1, memory_order_relaxed) & 31u) == 31u) platformYield();
}
static uint64_t sdlGetTicks(void) {
    clockYield();
    return ticksNs() / 1000000u;
}
static uint64_t sdlGetTicksNs(void) {
    clockYield();
    return ticksNs();
}
static uint64_t sdlGetPerformanceCounter(void) { return ticksNs(); }
static uint64_t sdlGetPerformanceFrequency(void) { return 1000000000u; }
static void sdlDelay(uint32_t milliseconds) { platformSleepNs((uint64_t)milliseconds * 1000000u); }
static void sdlDelayNs(uint64_t nanoseconds) { platformSleepNs(nanoseconds); }

// ---- function table ------------------------------------------------------------------------------------------------------------------
#define ENTRY(name, function) {name, (uintptr_t)(function)}
typedef struct {
    const char *name;
    uintptr_t address;
} Entry;
static const Entry sdl_table[] = {
    ENTRY("SDL_Init", sdlInit),
    ENTRY("SDL_InitSubSystem", sdlInitSubSystem),
    ENTRY("SDL_QuitSubSystem", sdlVoid),
    ENTRY("SDL_Quit", sdlVoid),
    ENTRY("SDL_WasInit", sdlWasInit),
    ENTRY("SDL_GetError", sdlGetError),
    ENTRY("SDL_ClearError", sdlClearError),
    ENTRY("SDL_SetError", sdlSetError),
    ENTRY("SDL_SetHint", sdlSetHint),
    ENTRY("SDL_SetHintWithPriority", sdlSetHintWithPriority),
    ENTRY("SDL_GetHint", sdlNull),
    ENTRY("SDL_SetAppMetadata", sdlTrue),
    ENTRY("SDL_SetAppMetadataProperty", sdlTrue),
    ENTRY("SDL_GetVersion", sdlGetVersion),
    ENTRY("SDL_GetRevision", sdlGetRevision),
    ENTRY("SDL_GetPlatform", sdlGetPlatform),
    ENTRY("SDL_malloc", sdlMalloc),
    ENTRY("SDL_calloc", sdlCalloc),
    ENTRY("SDL_realloc", sdlRealloc),
    ENTRY("SDL_free", sdlFree),
    ENTRY("SDL_SetLogOutputFunction", sdlVoid),
    ENTRY("SDL_SetLogPriority", sdlVoid),
    ENTRY("SDL_CreateProperties", sdlCreateProperties),
    ENTRY("SDL_DestroyProperties", sdlDestroyProperties),
    ENTRY("SDL_SetNumberProperty", sdlSetNumberProperty),
    ENTRY("SDL_SetPointerProperty", sdlSetPointerProperty),
    ENTRY("SDL_SetBooleanProperty", sdlSetBooleanProperty),
    ENTRY("SDL_SetStringProperty", sdlSetStringProperty),
    ENTRY("SDL_GetNumberProperty", sdlGetNumberProperty),
    ENTRY("SDL_GetPointerProperty", sdlGetPointerProperty),
    ENTRY("SDL_GetBooleanProperty", sdlGetBooleanProperty),
    ENTRY("SDL_GetNumVideoDrivers", sdlGetNumVideoDrivers),
    ENTRY("SDL_GetVideoDriver", sdlGetVideoDriver),
    ENTRY("SDL_GetCurrentVideoDriver", sdlGetCurrentVideoDriver),
    ENTRY("SDL_GetSystemTheme", sdlFalse),
    ENTRY("SDL_GetDisplays", sdlGetDisplays),
    ENTRY("SDL_GetPrimaryDisplay", sdlGetPrimaryDisplay),
    ENTRY("SDL_GetDisplayName", sdlGetDisplayName),
    ENTRY("SDL_GetDisplayBounds", sdlGetDisplayBounds),
    ENTRY("SDL_GetDisplayUsableBounds", sdlGetDisplayBounds),
    ENTRY("SDL_GetDisplayContentScale", sdlOneFloat),
    ENTRY("SDL_GetDesktopDisplayMode", sdlGetDisplayMode),
    ENTRY("SDL_GetCurrentDisplayMode", sdlGetDisplayMode),
    ENTRY("SDL_GetFullscreenDisplayModes", sdlGetFullscreenDisplayModes),
    ENTRY("SDL_GetDisplayForWindow", sdlGetDisplayForWindow),
    ENTRY("SDL_GetNaturalDisplayOrientation", sdlFalse),
    ENTRY("SDL_GetCurrentDisplayOrientation", sdlFalse),
    ENTRY("SDL_CreateWindow", sdlCreateWindow),
    ENTRY("SDL_CreateWindowWithProperties", sdlCreateWindowWithProperties),
    ENTRY("SDL_DestroyWindow", sdlDestroyWindow),
    ENTRY("SDL_GetWindowSize", sdlGetWindowSize),
    ENTRY("SDL_GetWindowSizeInPixels", sdlGetWindowSize),
    ENTRY("SDL_SetWindowSize", sdlSetWindowSize),
    ENTRY("SDL_GetWindowPosition", sdlGetWindowPosition),
    ENTRY("SDL_GetWindowID", sdlGetWindowID),
    ENTRY("SDL_GetWindowFromID", sdlGetWindowFromID),
    ENTRY("SDL_GetWindowFlags", sdlGetWindowFlags),
    ENTRY("SDL_GetWindowDisplayScale", sdlOneFloat),
    ENTRY("SDL_GetWindowPixelDensity", sdlOneFloat),
    ENTRY("SDL_GetWindowSafeArea", sdlGetWindowSafeArea),
    ENTRY("SDL_GetWindowBordersSize", sdlGetWindowBordersSize),
    ENTRY("SDL_GetWindowFullscreenMode", sdlNull),
    ENTRY("SDL_GetWindowProperties", sdlFalse),
    ENTRY("SDL_SetWindowTitle", sdlSetWindowTitle),
    ENTRY("SDL_SetWindowIcon", sdlTrue),
    ENTRY("SDL_ShowWindow", sdlTrue),
    ENTRY("SDL_HideWindow", sdlTrue),
    ENTRY("SDL_RaiseWindow", sdlTrue),
    ENTRY("SDL_SetWindowPosition", sdlTrue),
    ENTRY("SDL_SetWindowMinimumSize", sdlTrue),
    ENTRY("SDL_SetWindowMaximumSize", sdlTrue),
    ENTRY("SDL_MaximizeWindow", sdlTrue),
    ENTRY("SDL_SetWindowFullscreen", sdlSetWindowFullscreen),
    ENTRY("SDL_SetWindowFullscreenMode", sdlSetWindowFullscreenMode),
    ENTRY("SDL_SetWindowResizable", sdlSetWindowResizable),
    ENTRY("SDL_SetWindowBordered", sdlSetWindowBordered),
    ENTRY("SDL_SyncWindow", sdlTrue),
    ENTRY("SDL_StartTextInput", sdlStartTextInput),
    ENTRY("SDL_StartTextInputWithProperties", sdlStartTextInputWithProperties),
    ENTRY("SDL_StopTextInput", sdlStopTextInput),
    ENTRY("SDL_TextInputActive", sdlTextInputActive),
    ENTRY("SDL_SetTextInputArea", sdlTrue),
    ENTRY("SDL_ClearComposition", sdlTrue),
    ENTRY("SDL_IOFromMem", sdlIoFromMem),
    ENTRY("SDL_IOFromConstMem", sdlIoFromConstMem),
    ENTRY("SDL_ReadIO", sdlReadIo),
    ENTRY("SDL_WriteIO", sdlWriteIo),
    ENTRY("SDL_SeekIO", sdlSeekIo),
    ENTRY("SDL_TellIO", sdlTellIo),
    ENTRY("SDL_GetIOSize", sdlGetIoSize),
    ENTRY("SDL_CloseIO", sdlCloseIo),
    ENTRY("SDL_AddGamepadMappingsFromIO", sdlAddGamepadMappingsFromIo),
    ENTRY("SDL_AddGamepadMapping", sdlAddGamepadMapping),
    ENTRY("SDL_AddGamepadMappingsFromFile", sdlAddGamepadMappingsFromFile),
    ENTRY("SDL_UpdateGamepads", sdlUpdateGamepads),
    ENTRY("SDL_GetJoystickPowerInfo", sdlJoystickPowerInfo),
    ENTRY("SDL_GetJoystickVendorForID", sdlJoystickVendorForId),
    ENTRY("SDL_GetJoystickProductForID", sdlJoystickProductForId),
    ENTRY("SDL_CreateColorCursor", sdlCreateColorCursor),
    ENTRY("SDL_GetKeyFromScancode", sdlGetKeyFromScancode),
    ENTRY("SDL_GetKeyName", sdlGetKeyName),
    ENTRY("SDL_OpenURL", sdlOpenUrl),
    ENTRY("SDL_ShowCursor", sdlTrue),
    ENTRY("SDL_HideCursor", sdlTrue),
    ENTRY("SDL_CreateSystemCursor", sdlCreateSystemCursor),
    ENTRY("SDL_SetCursor", sdlTrue),
    ENTRY("SDL_DestroyCursor", sdlVoid),
    ENTRY("SDL_SetWindowMouseGrab", sdlTrue),
    ENTRY("SDL_SetWindowKeyboardGrab", sdlTrue),
    ENTRY("SDL_SetWindowRelativeMouseMode", sdlTrue),
    ENTRY("SDL_ShowSimpleMessageBox", sdlShowSimpleMessageBox),
    ENTRY("SDL_ShowMessageBox", sdlTrue),
    ENTRY("SDL_CreateSurface", sdlCreateSurface),
    ENTRY("SDL_CreateSurfaceFrom", sdlCreateSurfaceFrom),
    ENTRY("SDL_DestroySurface", sdlDestroySurface),
    ENTRY("SDL_AddSurfaceAlternateImage", sdlTrue),
    ENTRY("SDL_GL_SetAttribute", sdlGlSetAttribute),
    ENTRY("SDL_GL_GetAttribute", sdlGlGetAttribute),
    ENTRY("SDL_GL_ResetAttributes", sdlGlResetAttributes),
    ENTRY("SDL_GL_LoadLibrary", sdlTrue),
    ENTRY("SDL_GL_CreateContext", sdlGlCreateContext),
    ENTRY("SDL_GL_DestroyContext", sdlGlDestroyContext),
    ENTRY("SDL_GL_MakeCurrent", sdlGlMakeCurrent),
    ENTRY("SDL_GL_GetCurrentContext", sdlGlGetCurrentContext),
    ENTRY("SDL_GL_SetSwapInterval", sdlGlSetSwapInterval),
    ENTRY("SDL_GL_GetSwapInterval", sdlGlGetSwapInterval),
    ENTRY("SDL_GL_SwapWindow", sdlGlSwapWindow),
    ENTRY("SDL_GL_GetProcAddress", sdlGlGetProcAddress),
    ENTRY("SDL_EGL_GetProcAddress", sdlGlGetProcAddress),
    ENTRY("SDL_GL_ExtensionSupported", sdlGlExtensionSupported),
    ENTRY("SDL_PumpEvents", sdlPumpEvents),
    ENTRY("SDL_PollEvent", sdlPollEvent),
    ENTRY("SDL_WaitEventTimeout", sdlFalse),
    ENTRY("SDL_PushEvent", sdlTrue),
    ENTRY("SDL_SetEventEnabled", sdlVoid),
    ENTRY("SDL_FlushEvents", sdlVoid),
    ENTRY("SDL_UpdateJoysticks", sdlUpdateJoysticks),
    ENTRY("SDL_GetKeyboardState", sdlGetKeyboardState),
    ENTRY("SDL_GetModState", sdlGetModState),
    ENTRY("SDL_GetMouseState", sdlGetMouseState),
    ENTRY("SDL_GetGamepads", sdlGamepadIds),
    ENTRY("SDL_GetJoysticks", sdlGamepadIds),
    ENTRY("SDL_GetSensors", sdlEmptyIdList),
    ENTRY("SDL_GetKeyboards", sdlEmptyIdList),
    ENTRY("SDL_GetMice", sdlEmptyIdList),
    ENTRY("SDL_OpenGamepad", sdlOpenGamepad),
    ENTRY("SDL_CloseGamepad", sdlVoid),
    ENTRY("SDL_IsGamepad", sdlIsGamepad),
    ENTRY("SDL_GetGamepadFromID", sdlGetGamepadFromId),
    ENTRY("SDL_GetJoystickFromID", sdlGetJoystickFromId),
    ENTRY("SDL_GetGamepadJoystick", sdlGetGamepadJoystick),
    ENTRY("SDL_GetGamepadID", sdlGamepadId),
    ENTRY("SDL_GetJoystickID", sdlGamepadId),
    ENTRY("SDL_GetGamepadName", sdlGamepadName),
    ENTRY("SDL_GetJoystickName", sdlGamepadName),
    ENTRY("SDL_GetGamepadNameForID", sdlGamepadNameForId),
    ENTRY("SDL_GetJoystickNameForID", sdlGamepadNameForId),
    ENTRY("SDL_GetGamepadType", sdlGamepadType),
    ENTRY("SDL_GetRealGamepadType", sdlGamepadType),
    ENTRY("SDL_GetGamepadTypeForID", sdlGamepadTypeForId),
    ENTRY("SDL_GetRealGamepadTypeForID", sdlGamepadTypeForId),
    ENTRY("SDL_GetJoystickType", sdlJoystickType),
    ENTRY("SDL_GetGamepadVendor", sdlGamepadVendor),
    ENTRY("SDL_GetJoystickVendor", sdlGamepadVendor),
    ENTRY("SDL_GetGamepadProduct", sdlGamepadProduct),
    ENTRY("SDL_GetJoystickProduct", sdlGamepadProduct),
    ENTRY("SDL_GetGamepadProductVersion", sdlGamepadZero16),
    ENTRY("SDL_GetJoystickProductVersion", sdlGamepadZero16),
    ENTRY("SDL_GetGamepadFirmwareVersion", sdlGamepadZero16),
    ENTRY("SDL_GetGamepadPlayerIndex", sdlGamepadZeroInt),
    ENTRY("SDL_GetJoystickPlayerIndex", sdlGamepadZeroInt),
    ENTRY("SDL_GetGamepadButton", sdlGamepadButton),
    ENTRY("SDL_GetGamepadAxis", sdlGamepadAxis),
    ENTRY("SDL_GamepadHasButton", sdlGamepadHasButton),
    ENTRY("SDL_GamepadHasAxis", sdlGamepadHasAxis),
    ENTRY("SDL_GamepadConnected", sdlGamepadConnected),
    ENTRY("SDL_JoystickConnected", sdlGamepadConnected),
    ENTRY("SDL_RumbleGamepad", sdlFalse),
    ENTRY("SDL_RumbleGamepadTriggers", sdlFalse),
    ENTRY("SDL_HasGamepad", sdlHasGamepad),
    ENTRY("SDL_GetTicks", sdlGetTicks),
    ENTRY("SDL_GetTicksNS", sdlGetTicksNs),
    ENTRY("SDL_GetPerformanceCounter", sdlGetPerformanceCounter),
    ENTRY("SDL_GetPerformanceFrequency", sdlGetPerformanceFrequency),
    ENTRY("SDL_Delay", sdlDelay),
    ENTRY("SDL_DelayNS", sdlDelayNs),
};
static uintptr_t lookupIn(const Entry *table, size_t count, const char *name) {
    atomic_fetch_add(&call_counter, 1);
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, name)) return table[i].address;
    atomic_fetch_add(&unknown_counter, 1);
    if (logged_unknown < 40) {
        ++logged_unknown;
        trace("sdl.lookup.unknown=%s", name);
    }
    return 0;
}
static uintptr_t sdlLookup(const char *name) { return lookupIn(sdl_table, sizeof(sdl_table) / sizeof(sdl_table[0]), name); }
const LinuxVirtualLibrary linuxSdlLibrary = {"libSDL3", sdlLookup, "SDL_", false};

// ---- libEGL: Mesa's own entry points ---------------------------------------------------------------------------------------------
static void *glxGetProcAddress(const char *name) { return name ? glEntryPoint(name) : NULL; }
static uintptr_t eglLookup(const char *name) {
    if (!strcmp(name, "glXGetProcAddress") || !strcmp(name, "glXGetProcAddressARB")) return (uintptr_t)glxGetProcAddress;
    if (!strcmp(name, "eglGetProcAddress")) return (uintptr_t)eglGetProcAddress;
    if (!strncmp(name, "egl", 3)) return (uintptr_t)eglGetProcAddress(name);
    if (!strncmp(name, "gl", 2)) return (uintptr_t)glEntryPoint(name);
    return 0;
}
const LinuxVirtualLibrary linuxEglLibrary = {"libEGL", eglLookup, NULL, false};
// LWJGL looks for the GLX library first (libGLX.so.0, libGL.so.1, libGL.so) and complains loudly when none exists before it settles for EGL. It
// only asks it for glXGetProcAddress and the GL functions, which are the ones above: the first name it tries gets them.
const LinuxVirtualLibrary linuxGlxLibrary = {"libGLX", eglLookup, NULL, false};

LinuxSdlStats linuxSdlStats(void) {
    unsigned w = 0, h = 0;
    if (egl_surface != EGL_NO_SURFACE) screenSize(&w, &h);
    return (LinuxSdlStats){windows_created,
                           contexts_created,
                           atomic_load(&frame_counter),
                           atomic_load(&call_counter),
                           atomic_load(&unknown_counter),
                           egl_context != EGL_NO_CONTEXT,
                           w,
                           h};
}
bool linuxSdlReset(void) {
    bool ok = true;
    linuxSdlIoReset();
    linuxFilePickerReset();
    linuxAudioReset();  // the game is over: stop the audio output before anything else goes away
    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_context != EGL_NO_CONTEXT && !eglDestroyContext(egl_display, egl_context)) ok = false;
        if (egl_surface != EGL_NO_SURFACE && !eglDestroySurface(egl_display, egl_surface)) ok = false;
        if (!eglTerminate(egl_display)) ok = false;
    }
    egl_display = EGL_NO_DISPLAY;
    egl_surface = EGL_NO_SURFACE;
    egl_context = EGL_NO_CONTEXT;
    memset(windows, 0, sizeof(windows));
    memset(property_sets, 0, sizeof(property_sets));
    memset(gl_attributes, 0, sizeof(gl_attributes));
    windows_created = contexts_created = 0;
    next_window_id = 1;
    logged_unknown = 0;
    atomic_store(&frame_counter, 0);
    atomic_store(&call_counter, 0);
    atomic_store(&unknown_counter, 0);
    atomic_store(&stop_requested, false);
    atomic_store(&requested_size, 0);
    clientTargetReset();  // its names belonged to the context just destroyed
    linuxSdlEventsReset();
    atomic_store(&last_pump_ns, 0);
    return ok;
}
