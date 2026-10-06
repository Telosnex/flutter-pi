// SPDX-License-Identifier: MIT
/*
 * WPE WebKit webviews on the WPEPlatform API. See webview_direct.h.
 *
 * Threading: everything that touches WebKit or WPEPlatform objects runs on
 * one shared "flutterpi-webkit" thread with its own GMainContext. Other
 * threads only queue work onto it. struct webview_direct is reference
 * counted so queued work can outlive webview_direct_destroy() safely; once
 * destroyed, its web_view is NULL and queued work does nothing.
 */
#define _GNU_SOURCE
#include "plugins/gstreamer_video_player/webview_direct.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gst/video/navigation.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <xkbcommon/xkbcommon.h>

#include "compositor_ng.h"
#include "dmabuf_surface.h"
#include "plugins/gstreamer_video_player/webview_gpu.h"
#include "util/logging.h"

#define WD_EGL_PLATFORM_SURFACELESS_MESA 0x31DD

/// FLUTTERPI_WEBVIEW_STATS=1: log view timing (release builds drop LOG_DEBUG).
#define LOG_STATS(fmtstring, ...)                                                   \
    do {                                                                            \
        if (getenv("FLUTTERPI_WEBVIEW_STATS") != NULL)                              \
            LOG_ERROR_UNPREFIXED("webview direct: " fmtstring, ##__VA_ARGS__);      \
    } while (0)

struct webview_direct {
    gatomicrefcount n_refs;

    // Set at creation, read-only afterwards.
    struct webview_gpu *gpu;
    struct dmabuf_surface *surface;
    struct compositor *compositor;
    int width, height;
    webview_direct_configure_cb configure;
    void *userdata;

    // WebKit thread only.
    WebKitWebView *web_view;
    WPEView *wpe_view;
    gint64 created_us, first_frame_us;
    guint64 n_frames, n_copy_failures, n_not_dmabuf;
    /// The frame WebKit is waiting on: it starts the next one only after
    /// wpe_view_buffer_rendered(), which we send at the next page flip.
    WPEBuffer *waiting_buffer;
    GSource *waiting_timeout;

    // Latest coalesced pointer move, guarded by move_lock.
    GMutex move_lock;
    GstEvent *pending_move;
};

static struct webview_direct *view_ref(struct webview_direct *view) {
    g_atomic_ref_count_inc(&view->n_refs);
    return view;
}

static void view_unref(struct webview_direct *view) {
    if (g_atomic_ref_count_dec(&view->n_refs)) {
        if (view->pending_move != NULL) gst_event_unref(view->pending_move);
        g_mutex_clear(&view->move_lock);
        free(view);
    }
}

/* ------------------------------------------------------------------------- */
/* WPEPlatform subclasses                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    WPEView parent;
    struct webview_direct *owner;  // WebKit thread only; not a reference.
} FlutterpiWpeView;
typedef struct {
    WPEViewClass parent;
} FlutterpiWpeViewClass;
G_DEFINE_TYPE(FlutterpiWpeView, flutterpi_wpe_view, WPE_TYPE_VIEW)

typedef struct {
    WPEToplevel parent;
} FlutterpiWpeToplevel;
typedef struct {
    WPEToplevelClass parent;
} FlutterpiWpeToplevelClass;
G_DEFINE_TYPE(FlutterpiWpeToplevel, flutterpi_wpe_toplevel, WPE_TYPE_TOPLEVEL)

typedef struct {
    WPEDisplay parent;
    EGLDisplay egl_display;
    WPEDRMDevice *drm_device;
    WPEKeymap *keymap;
} FlutterpiWpeDisplay;
typedef struct {
    WPEDisplayClass parent;
} FlutterpiWpeDisplayClass;
G_DEFINE_TYPE(FlutterpiWpeDisplay, flutterpi_wpe_display, WPE_TYPE_DISPLAY)

static void wait_for_fence(int fd) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (fd < 0) return;
    // A sync_file becomes readable once the GPU work behind it is done.
    if (poll(&pfd, 1, 100) == 0) {
        LOG_ERROR("WebKit frame fence did not signal within 100 ms; copying anyway.\n");
    }
}

static void present_frame(struct webview_direct *view, WPEBuffer *buffer) {
    WPEBufferDMABuf *dmabuf_buffer;
    struct dmabuf dmabuf;
    int fd, ok;

    if (!WPE_IS_BUFFER_DMA_BUF(buffer)) {
        if (view->n_not_dmabuf++ == 0) {
            LOG_ERROR("WebKit handed over a %s frame; only DMA-BUF frames are shown.\n", G_OBJECT_TYPE_NAME(buffer));
        }
        return;
    }
    dmabuf_buffer = WPE_BUFFER_DMA_BUF(buffer);
    if (wpe_buffer_dma_buf_get_n_planes(dmabuf_buffer) != 1) {
        if (view->n_not_dmabuf++ == 0) {
            LOG_ERROR("WebKit handed over a multi-plane frame; only single-plane frames are shown.\n");
        }
        return;
    }
    fd = wpe_buffer_dma_buf_get_fd(dmabuf_buffer, 0);
    wait_for_fence(wpe_buffer_get_rendering_fence(buffer));

    ok = webview_gpu_copy_dmabuf(
        view->gpu,
        &(struct webview_gpu_source){
            .width = wpe_buffer_get_width(buffer),
            .height = wpe_buffer_get_height(buffer),
            .fourcc = wpe_buffer_dma_buf_get_format(dmabuf_buffer),
            .fd = fd,
            .stride = wpe_buffer_dma_buf_get_stride(dmabuf_buffer, 0),
            .offset = wpe_buffer_dma_buf_get_offset(dmabuf_buffer, 0),
            .modifier = wpe_buffer_dma_buf_get_modifier(dmabuf_buffer),
        },
        &dmabuf
    );
    if (ok == 0) {
        ok = dmabuf_surface_push_dmabuf(view->surface, &dmabuf, webview_gpu_release_dmabuf);
        if (ok != 0) webview_gpu_release_dmabuf(&dmabuf);
    } else if (ok != EAGAIN && view->n_copy_failures++ == 0) {
        LOG_ERROR("Could not copy the webview frame on the GPU: %s\n", strerror(ok));
    }
    if (view->n_frames++ == 0) {
        view->first_frame_us = g_get_monotonic_time();
        LOG_STATS("first frame after %.0f ms\n", (view->first_frame_us - view->created_us) / 1000.0);
    }
}

/// Hands the waiting frame back to WebKit, letting it start the next one.
/// WebKit thread.
static void finish_waiting_frame(struct webview_direct *view) {
    WPEBuffer *buffer = view->waiting_buffer;
    if (view->waiting_timeout != NULL) {
        g_source_destroy(view->waiting_timeout);
        g_source_unref(view->waiting_timeout);
        view->waiting_timeout = NULL;
    }
    if (buffer == NULL) return;
    view->waiting_buffer = NULL;
    if (view->wpe_view != NULL) {
        wpe_view_buffer_rendered(view->wpe_view, buffer);
        wpe_view_buffer_released(view->wpe_view, buffer);
    }
    g_object_unref(buffer);
}

static gboolean finish_on_thread(void *userdata) {
    finish_waiting_frame(userdata);
    return G_SOURCE_REMOVE;
}

static void destroy_view_ref(void *userdata) {
    view_unref(userdata);
}

static GMainContext *webkit_context(void);

/// Display thread, at the page flip after the frame was committed.
static void on_frame_scanout(void *userdata) {
    g_main_context_invoke_full(webkit_context(), G_PRIORITY_HIGH, finish_on_thread, userdata, destroy_view_ref);
}

/// Safety net if no flip comes (display off, commit failed early).
static gboolean on_frame_timeout(void *userdata) {
    struct webview_direct *view = userdata;
    // finish_waiting_frame destroys the source; this callback returning
    // REMOVE afterwards is fine.
    finish_waiting_frame(view);
    return G_SOURCE_REMOVE;
}

static gboolean flutterpi_wpe_view_render_buffer(
    WPEView *wpe_view,
    WPEBuffer *buffer,
    const WPERectangle *damage_rects,
    guint n_damage_rects,
    GError **error
) {
    struct webview_direct *view = ((FlutterpiWpeView *) wpe_view)->owner;
    (void) damage_rects;
    (void) n_damage_rects;
    (void) error;

    if (view == NULL) {
        // Closing: hand the buffer straight back.
        wpe_view_buffer_rendered(wpe_view, buffer);
        wpe_view_buffer_released(wpe_view, buffer);
        return TRUE;
    }

    // WebKit should not send a frame before the previous one is finished;
    // if it does, finish the old one first.
    finish_waiting_frame(view);
    present_frame(view, buffer);

    // The copy has finished reading the buffer (webview_gpu waits for the
    // GPU), but WebKit only gets it back at the next page flip, which paces
    // WebKit to the display instead of letting it draw hundreds of frames a
    // second that are never shown. The repaint also puts the frame on screen
    // when Flutter itself is idle. Only this thread holds WebKit buffers.
    view->waiting_buffer = g_object_ref(buffer);
    if (compositor_request_repaint(view->compositor, on_frame_scanout, view_ref(view)) != 0) {
        // No listener was registered (nothing composited yet, or no KMS).
        view_unref(view);
        finish_waiting_frame(view);
        return TRUE;
    }
    view->waiting_timeout = g_timeout_source_new(100);
    g_source_set_callback(view->waiting_timeout, on_frame_timeout, view, NULL);
    g_source_attach(view->waiting_timeout, webkit_context());
    return TRUE;
}

static void flutterpi_wpe_view_init(FlutterpiWpeView *self) {
    self->owner = NULL;
}

static void flutterpi_wpe_view_class_init(FlutterpiWpeViewClass *klass) {
    WPE_VIEW_CLASS(klass)->render_buffer = flutterpi_wpe_view_render_buffer;
}

static gboolean resize_view(WPEToplevel *toplevel, WPEView *view, gpointer userdata) {
    int width, height;
    (void) userdata;
    wpe_toplevel_get_size(toplevel, &width, &height);
    wpe_view_resized(view, width, height);
    return FALSE;
}

static gboolean flutterpi_wpe_toplevel_resize(WPEToplevel *toplevel, int width, int height) {
    wpe_toplevel_resized(toplevel, width, height);
    wpe_toplevel_foreach_view(toplevel, resize_view, NULL);
    return TRUE;
}

static void flutterpi_wpe_toplevel_init(FlutterpiWpeToplevel *self) {
    (void) self;
}

static void flutterpi_wpe_toplevel_class_init(FlutterpiWpeToplevelClass *klass) {
    WPE_TOPLEVEL_CLASS(klass)->resize = flutterpi_wpe_toplevel_resize;
}

static gboolean flutterpi_wpe_display_connect(WPEDisplay *display, GError **error) {
    FlutterpiWpeDisplay *self = (FlutterpiWpeDisplay *) display;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    PFNEGLQUERYDISPLAYATTRIBEXTPROC query_display_attrib;
    PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string;
    const char *primary = NULL, *render = NULL;
    EGLAttrib device = 0;

    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display != NULL) {
        self->egl_display = get_platform_display(WD_EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    }
    if (self->egl_display == EGL_NO_DISPLAY || !eglInitialize(self->egl_display, NULL, NULL)) {
        g_set_error(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "Could not open a surfaceless EGL display (0x%x)", eglGetError());
        return FALSE;
    }

    // WebKit allocates its render buffers on this DRM device. Ask EGL which one
    // it is, so WebKit and the copy in webview_gpu use the same GPU.
    query_display_attrib = (PFNEGLQUERYDISPLAYATTRIBEXTPROC) eglGetProcAddress("eglQueryDisplayAttribEXT");
    query_device_string = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress("eglQueryDeviceStringEXT");
    if (query_display_attrib != NULL && query_device_string != NULL && query_display_attrib(self->egl_display, EGL_DEVICE_EXT, &device)) {
        const char *extensions = query_device_string((EGLDeviceEXT) device, EGL_EXTENSIONS);
        if (extensions != NULL && strstr(extensions, "EGL_EXT_device_drm") != NULL) {
            primary = query_device_string((EGLDeviceEXT) device, EGL_DRM_DEVICE_FILE_EXT);
        }
#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
        if (extensions != NULL && strstr(extensions, "EGL_EXT_device_drm_render_node") != NULL) {
            render = query_device_string((EGLDeviceEXT) device, EGL_DRM_RENDER_NODE_FILE_EXT);
        }
#endif
    }
    if (primary == NULL) primary = "/dev/dri/card0";
    if (render == NULL) render = "/dev/dri/renderD128";
    self->drm_device = wpe_drm_device_new(primary, render);
    LOG_STATS("WebKit renders on %s (%s)\n", render, primary);
    return TRUE;
}

static WPEView *flutterpi_wpe_display_create_view(WPEDisplay *display) {
    WPEView *view = WPE_VIEW(g_object_new(flutterpi_wpe_view_get_type(), "display", display, NULL));
    WPEToplevel *toplevel = WPE_TOPLEVEL(g_object_new(flutterpi_wpe_toplevel_get_type(), "display", display, NULL));
    wpe_view_set_toplevel(view, toplevel);
    g_object_unref(toplevel);
    return view;
}

static gpointer flutterpi_wpe_display_get_egl_display(WPEDisplay *display, GError **error) {
    (void) error;
    return ((FlutterpiWpeDisplay *) display)->egl_display;
}

static WPEDRMDevice *flutterpi_wpe_display_get_drm_device(WPEDisplay *display) {
    return ((FlutterpiWpeDisplay *) display)->drm_device;
}

static WPEKeymap *flutterpi_wpe_display_get_keymap(WPEDisplay *display) {
    FlutterpiWpeDisplay *self = (FlutterpiWpeDisplay *) display;
    if (self->keymap == NULL) self->keymap = wpe_keymap_xkb_new();
    return self->keymap;
}

static void flutterpi_wpe_display_init(FlutterpiWpeDisplay *self) {
    self->egl_display = EGL_NO_DISPLAY;
}

static void flutterpi_wpe_display_class_init(FlutterpiWpeDisplayClass *klass) {
    WPEDisplayClass *display_class = WPE_DISPLAY_CLASS(klass);
    display_class->connect = flutterpi_wpe_display_connect;
    display_class->create_view = flutterpi_wpe_display_create_view;
    display_class->get_egl_display = flutterpi_wpe_display_get_egl_display;
    display_class->get_drm_device = flutterpi_wpe_display_get_drm_device;
    display_class->get_keymap = flutterpi_wpe_display_get_keymap;
}

/* ------------------------------------------------------------------------- */
/* The shared WebKit thread                                                  */
/* ------------------------------------------------------------------------- */

static struct {
    GMutex lock;
    GCond cond;
    bool started, ready;
    GMainContext *context;
    GThread *thread;
    WPEDisplay *display;
    WebKitWebContext *web_context;
} shared;

static void *run_webkit_thread(void *userdata) {
    GMainLoop *loop;
    GError *error = NULL;
    WPEDisplay *display;
    (void) userdata;

    g_main_context_push_thread_default(shared.context);

    display = WPE_DISPLAY(g_object_new(flutterpi_wpe_display_get_type(), NULL));
    if (!wpe_display_connect(display, &error)) {
        LOG_ERROR("webview direct: %s\n", error != NULL ? error->message : "could not connect the WPE display");
        g_clear_error(&error);
        g_object_unref(display);
        display = NULL;
    }

    g_mutex_lock(&shared.lock);
    shared.display = display;
    shared.web_context = display != NULL ? WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, NULL)) : NULL;
    if (shared.web_context != NULL) {
        // No web process cache or back/forward page cache: a closed webview's
        // web process exits instead of idling at ~100 MB for reuse. Costs a
        // slower reopen and slower Back navigation.
        webkit_web_context_set_cache_model(shared.web_context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    }
    shared.ready = true;
    g_cond_broadcast(&shared.cond);
    g_mutex_unlock(&shared.lock);

    if (display == NULL) {
        g_main_context_pop_thread_default(shared.context);
        return NULL;
    }

    // Runs for the life of the process, like wpevideosrc's WebKit thread.
    loop = g_main_loop_new(shared.context, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return NULL;
}

static GMainContext *webkit_context(void) {
    return shared.context;
}

static bool ensure_thread(void) {
    bool ok;
    g_mutex_lock(&shared.lock);
    if (!shared.started) {
        shared.started = true;
        shared.context = g_main_context_new();
        shared.thread = g_thread_new("flutterpi-webkit", run_webkit_thread, NULL);
    }
    while (!shared.ready) g_cond_wait(&shared.cond, &shared.lock);
    ok = shared.display != NULL;
    g_mutex_unlock(&shared.lock);
    return ok;
}

struct sync_call {
    GSourceFunc func;
    void *data;
    GMutex lock;
    GCond cond;
    bool done;
};

static gboolean run_sync_call(void *userdata) {
    struct sync_call *call = userdata;
    call->func(call->data);
    g_mutex_lock(&call->lock);
    call->done = true;
    g_cond_signal(&call->cond);
    g_mutex_unlock(&call->lock);
    return G_SOURCE_REMOVE;
}

/// Runs @func on the WebKit thread and waits for it.
static void call_sync(GSourceFunc func, void *data) {
    struct sync_call call = { .func = func, .data = data, .done = false };

    if (g_thread_self() == shared.thread) {
        func(data);
        return;
    }
    g_mutex_init(&call.lock);
    g_cond_init(&call.cond);
    g_main_context_invoke(shared.context, run_sync_call, &call);
    g_mutex_lock(&call.lock);
    while (!call.done) g_cond_wait(&call.cond, &call.lock);
    g_mutex_unlock(&call.lock);
    g_mutex_clear(&call.lock);
    g_cond_clear(&call.cond);
}

/* ------------------------------------------------------------------------- */
/* Creating and closing views                                                */
/* ------------------------------------------------------------------------- */

bool webview_direct_requested(void) {
    return getenv("FLUTTERPI_WEBVIEW_DIRECT") != NULL;
}

struct create_args {
    struct webview_direct *view;
    const char *location;
};

static gboolean create_on_thread(void *userdata) {
    struct create_args *args = userdata;
    struct webview_direct *view = args->view;
    WebKitWebView *web_view;
    WPEView *wpe_view;

    web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", shared.web_context, "display", shared.display, NULL));
    wpe_view = webkit_web_view_get_wpe_view(web_view);
    if (wpe_view == NULL || !G_TYPE_CHECK_INSTANCE_TYPE(wpe_view, flutterpi_wpe_view_get_type())) {
        LOG_ERROR("webview direct: WebKit did not create its view on the flutter-pi display.\n");
        g_object_unref(web_view);
        return G_SOURCE_REMOVE;
    }

    view->web_view = web_view;
    view->wpe_view = wpe_view;
    view->created_us = g_get_monotonic_time();
    ((FlutterpiWpeView *) wpe_view)->owner = view;

    webkit_settings_set_enable_webaudio(webkit_web_view_get_settings(web_view), TRUE);
    // Scroll physics come from Flutter, which sends precise pixel deltas each
    // frame. WebKit's own scroll animation would smooth them a second time,
    // making the page lag behind the fingers.
    webkit_settings_set_enable_smooth_scrolling(webkit_web_view_get_settings(web_view), FALSE);
    if (view->configure != NULL) {
        view->configure(G_OBJECT(web_view), view->userdata);
    }

    wpe_toplevel_resize(wpe_view_get_toplevel(wpe_view), view->width, view->height);
    wpe_view_map(wpe_view);
    wpe_view_focus_in(wpe_view);

    if (args->location != NULL && args->location[0] != '\0') {
        webkit_web_view_load_uri(web_view, args->location);
    }
    return G_SOURCE_REMOVE;
}

struct webview_direct *webview_direct_new(
    struct webview_gpu *gpu,
    struct dmabuf_surface *surface,
    struct compositor *compositor,
    int width,
    int height,
    const char *location,
    webview_direct_configure_cb configure,
    void *userdata
) {
    struct webview_direct *view;

    if (gpu == NULL || surface == NULL || compositor == NULL || width <= 0 || height <= 0) {
        LOG_ERROR("webview direct: needs the GPU frame path, a plane surface, and a size.\n");
        return NULL;
    }
    if (!ensure_thread()) {
        return NULL;
    }

    view = calloc(1, sizeof *view);
    if (view == NULL) return NULL;
    g_atomic_ref_count_init(&view->n_refs);
    g_mutex_init(&view->move_lock);
    view->gpu = gpu;
    view->surface = surface;
    view->compositor = compositor;
    view->width = width;
    view->height = height;
    view->configure = configure;
    view->userdata = userdata;

    call_sync(create_on_thread, &(struct create_args){ .view = view, .location = location });
    if (view->web_view == NULL) {
        view_unref(view);
        return NULL;
    }
    return view;
}

static gboolean destroy_on_thread(void *userdata) {
    struct webview_direct *view = userdata;
    WebKitWebView *web_view = view->web_view;
    WebKitUserContentManager *manager;
    gint64 now = g_get_monotonic_time();

    if (web_view == NULL) return G_SOURCE_REMOVE;

    // After this, render_buffer still returns buffers to WebKit but no longer
    // touches the surface, and no signal reaches the owner's userdata.
    finish_waiting_frame(view);
    ((FlutterpiWpeView *) view->wpe_view)->owner = NULL;
    manager = webkit_web_view_get_user_content_manager(web_view);
    if (view->userdata != NULL) {
        g_signal_handlers_disconnect_by_data(web_view, view->userdata);
        if (manager != NULL) g_signal_handlers_disconnect_by_data(manager, view->userdata);
    }

    LOG_STATS(
        "closed after %.1f s, %" G_GUINT64_FORMAT " frames (%.1f fps)\n",
        (now - view->created_us) / 1e6,
        view->n_frames,
        view->first_frame_us != 0 && now > view->first_frame_us ? view->n_frames / ((now - view->first_frame_us) / 1e6) : 0.0
    );

    view->web_view = NULL;
    view->wpe_view = NULL;
    wpe_view_unmap(webkit_web_view_get_wpe_view(web_view));
    g_object_unref(web_view);
    return G_SOURCE_REMOVE;
}

void webview_direct_destroy(struct webview_direct *view) {
    if (view == NULL) return;
    call_sync(destroy_on_thread, view);
    view_unref(view);
}

/* ------------------------------------------------------------------------- */
/* Page control                                                              */
/* ------------------------------------------------------------------------- */

enum page_task_type { kLoadUrl, kLoadBytes, kRunJavaScript };

struct page_task {
    struct webview_direct *view;
    enum page_task_type type;
    char *string;
    GBytes *bytes;
};

static void destroy_page_task(void *userdata) {
    struct page_task *task = userdata;
    view_unref(task->view);
    g_free(task->string);
    if (task->bytes != NULL) g_bytes_unref(task->bytes);
    g_free(task);
}

static gboolean run_page_task(void *userdata) {
    struct page_task *task = userdata;
    WebKitWebView *web_view = task->view->web_view;
    if (web_view == NULL) return G_SOURCE_REMOVE;
    switch (task->type) {
        case kLoadUrl: webkit_web_view_load_uri(web_view, task->string); break;
        case kLoadBytes: webkit_web_view_load_bytes(web_view, task->bytes, NULL, NULL, NULL); break;
        case kRunJavaScript: webkit_web_view_evaluate_javascript(web_view, task->string, -1, NULL, NULL, NULL, NULL, NULL); break;
    }
    return G_SOURCE_REMOVE;
}

static int queue_page_task(struct webview_direct *view, enum page_task_type type, char *string, GBytes *bytes) {
    struct page_task *task = g_new0(struct page_task, 1);
    task->view = view_ref(view);
    task->type = type;
    task->string = string;
    task->bytes = bytes;
    g_main_context_invoke_full(shared.context, G_PRIORITY_DEFAULT, run_page_task, task, destroy_page_task);
    return 0;
}

int webview_direct_load_url(struct webview_direct *view, const char *url) {
    return queue_page_task(view, kLoadUrl, g_strdup(url), NULL);
}

int webview_direct_load_bytes(struct webview_direct *view, const uint8_t *data, size_t size) {
    return queue_page_task(view, kLoadBytes, NULL, g_bytes_new(data, size));
}

int webview_direct_run_javascript(struct webview_direct *view, const char *source) {
    return queue_page_task(view, kRunJavaScript, g_strdup(source), NULL);
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

static WPEModifiers modifiers_from_navigation(GstEvent *event) {
    GstNavigationModifierType state = 0;
    WPEModifiers modifiers = 0;
    if (!gst_navigation_event_parse_modifier_state(event, &state)) return 0;
    if (state & GST_NAVIGATION_MODIFIER_SHIFT_MASK) modifiers |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if (state & GST_NAVIGATION_MODIFIER_LOCK_MASK) modifiers |= WPE_MODIFIER_KEYBOARD_CAPS_LOCK;
    if (state & GST_NAVIGATION_MODIFIER_CONTROL_MASK) modifiers |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if (state & GST_NAVIGATION_MODIFIER_MOD1_MASK) modifiers |= WPE_MODIFIER_KEYBOARD_ALT;
    if (state & (GST_NAVIGATION_MODIFIER_META_MASK | GST_NAVIGATION_MODIFIER_SUPER_MASK)) modifiers |= WPE_MODIFIER_KEYBOARD_META;
    if (state & GST_NAVIGATION_MODIFIER_BUTTON1_MASK) modifiers |= WPE_MODIFIER_POINTER_BUTTON1;
    if (state & GST_NAVIGATION_MODIFIER_BUTTON2_MASK) modifiers |= WPE_MODIFIER_POINTER_BUTTON2;
    if (state & GST_NAVIGATION_MODIFIER_BUTTON3_MASK) modifiers |= WPE_MODIFIER_POINTER_BUTTON3;
    if (state & GST_NAVIGATION_MODIFIER_BUTTON4_MASK) modifiers |= WPE_MODIFIER_POINTER_BUTTON4;
    if (state & GST_NAVIGATION_MODIFIER_BUTTON5_MASK) modifiers |= WPE_MODIFIER_POINTER_BUTTON5;
    return modifiers;
}

static guint32 event_time(void) {
    return (guint32) (g_get_monotonic_time() / 1000);
}

static guint keyval_from_name(const char *key) {
    xkb_keysym_t keysym = XKB_KEY_NoSymbol;
    // One character: the character itself ("a", "é", "1"). Longer: a keysym
    // name ("Return", "BackSpace", "Left").
    if (g_utf8_strlen(key, -1) == 1) {
        keysym = xkb_utf32_to_keysym(g_utf8_get_char(key));
    }
    if (keysym == XKB_KEY_NoSymbol) {
        keysym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
    }
    if (keysym == XKB_KEY_NoSymbol) {
        keysym = xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE);
    }
    return keysym;
}

static guint keycode_for_keyval(guint keyval) {
    WPEKeymap *keymap = wpe_display_get_keymap(shared.display);
    WPEKeymapEntry *entries = NULL;
    guint n_entries = 0, keycode = 0;
    if (keymap != NULL && wpe_keymap_get_entries_for_keyval(keymap, keyval, &entries, &n_entries) && n_entries > 0) {
        keycode = entries[0].keycode;
    }
    g_free(entries);
    return keycode;
}

static WPEEvent *wpe_event_from_navigation(WPEView *wpe_view, GstEvent *event) {
    WPEModifiers modifiers = modifiers_from_navigation(event);
    guint32 time = event_time();
    double x = 0, y = 0, delta_x = 0, delta_y = 0;
    const char *key = NULL;
    guint touch_id = 0;
    int button = 0;

    switch (gst_navigation_event_get_type(event)) {
        case GST_NAVIGATION_EVENT_MOUSE_MOVE:
            if (!gst_navigation_event_parse_mouse_move_event(event, &x, &y)) return NULL;
            return wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, wpe_view, WPE_INPUT_SOURCE_MOUSE, time, modifiers, x, y, 0, 0);
        case GST_NAVIGATION_EVENT_MOUSE_BUTTON_PRESS:
            if (!gst_navigation_event_parse_mouse_button_event(event, &button, &x, &y)) return NULL;
            if (button <= 0) button = WPE_BUTTON_PRIMARY;
            return wpe_event_pointer_button_new(
                WPE_EVENT_POINTER_DOWN,
                wpe_view,
                WPE_INPUT_SOURCE_MOUSE,
                time,
                modifiers,
                (guint) button,
                x,
                y,
                wpe_view_compute_press_count(wpe_view, x, y, (guint) button, time)
            );
        case GST_NAVIGATION_EVENT_MOUSE_BUTTON_RELEASE:
            if (!gst_navigation_event_parse_mouse_button_event(event, &button, &x, &y)) return NULL;
            if (button <= 0) button = WPE_BUTTON_PRIMARY;
            return wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, wpe_view, WPE_INPUT_SOURCE_MOUSE, time, modifiers, (guint) button, x, y, 0);
        case GST_NAVIGATION_EVENT_MOUSE_SCROLL:
            if (!gst_navigation_event_parse_mouse_scroll_event(event, &x, &y, &delta_x, &delta_y)) return NULL;
            // Same sign as the old path: negative delta_y advances down the page.
            // Pixel deltas, so touch drags track the finger exactly.
            return wpe_event_scroll_new(wpe_view, WPE_INPUT_SOURCE_MOUSE, time, modifiers, delta_x, delta_y, TRUE, FALSE, x, y);
        case GST_NAVIGATION_EVENT_TOUCH_DOWN:
        case GST_NAVIGATION_EVENT_TOUCH_MOTION:
        case GST_NAVIGATION_EVENT_TOUCH_UP: {
            WPEEventType type = gst_navigation_event_get_type(event) == GST_NAVIGATION_EVENT_TOUCH_DOWN ? WPE_EVENT_TOUCH_DOWN :
                                gst_navigation_event_get_type(event) == GST_NAVIGATION_EVENT_TOUCH_UP   ? WPE_EVENT_TOUCH_UP :
                                                                                                          WPE_EVENT_TOUCH_MOVE;
            // Touch-up has its own parser; the generic one asserts on it.
            if (type == WPE_EVENT_TOUCH_UP ? !gst_navigation_event_parse_touch_up_event(event, &touch_id, &x, &y)
                                           : !gst_navigation_event_parse_touch_event(event, &touch_id, &x, &y, NULL)) {
                return NULL;
            }
            return wpe_event_touch_new(type, wpe_view, WPE_INPUT_SOURCE_TOUCHSCREEN, time, modifiers, touch_id, x, y);
        }
        case GST_NAVIGATION_EVENT_KEY_PRESS:
        case GST_NAVIGATION_EVENT_KEY_RELEASE: {
            guint keyval;
            if (!gst_navigation_event_parse_key_event(event, &key) || key == NULL) return NULL;
            keyval = keyval_from_name(key);
            if (keyval == XKB_KEY_NoSymbol) {
                LOG_DEBUG("webview direct: unknown key \"%s\"\n", key);
                return NULL;
            }
            return wpe_event_keyboard_new(
                gst_navigation_event_get_type(event) == GST_NAVIGATION_EVENT_KEY_PRESS ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
                wpe_view,
                WPE_INPUT_SOURCE_KEYBOARD,
                time,
                modifiers,
                keycode_for_keyval(keyval),
                keyval
            );
        }
        default:
            // Touch frames carry nothing here: each touch point goes to WebKit
            // as it arrives.
            return NULL;
    }
}

struct input_task {
    struct webview_direct *view;
    GstEvent *event;  // NULL: take the pending pointer move.
};

static void destroy_input_task(void *userdata) {
    struct input_task *task = userdata;
    if (task->event != NULL) gst_event_unref(task->event);
    view_unref(task->view);
    g_free(task);
}

static gboolean run_input_task(void *userdata) {
    struct input_task *task = userdata;
    struct webview_direct *view = task->view;
    GstEvent *event = task->event;
    WPEEvent *wpe_event;

    if (event == NULL) {
        g_mutex_lock(&view->move_lock);
        event = view->pending_move;
        view->pending_move = NULL;
        g_mutex_unlock(&view->move_lock);
        task->event = event;
    }
    if (event == NULL || view->wpe_view == NULL) return G_SOURCE_REMOVE;

    wpe_event = wpe_event_from_navigation(view->wpe_view, event);
    if (wpe_event != NULL) {
        wpe_view_event(view->wpe_view, wpe_event);
        wpe_event_unref(wpe_event);
    }
    return G_SOURCE_REMOVE;
}

int webview_direct_send_navigation_event(struct webview_direct *view, GstEvent *event) {
    struct input_task *task;

    if (view == NULL || event == NULL) {
        if (event != NULL) gst_event_unref(event);
        return EINVAL;
    }

    if (gst_navigation_event_get_type(event) == GST_NAVIGATION_EVENT_MOUSE_MOVE) {
        // Only the newest move matters; queue one task per burst.
        bool queued;
        g_mutex_lock(&view->move_lock);
        queued = view->pending_move != NULL;
        if (queued) gst_event_unref(view->pending_move);
        view->pending_move = event;
        g_mutex_unlock(&view->move_lock);
        if (queued) return 0;
        event = NULL;
    }

    task = g_new0(struct input_task, 1);
    task->view = view_ref(view);
    task->event = event;
    g_main_context_invoke_full(shared.context, G_PRIORITY_DEFAULT, run_input_task, task, destroy_input_task);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Pipeline description                                                      */
/* ------------------------------------------------------------------------- */

bool webview_direct_parse_pipeline(const char *description, char **location_out, int *width_out, int *height_out) {
    const char *p, *caps_start;
    GString *location;
    GstCaps *caps;
    GstStructure *structure;
    char *caps_string;
    int width = 0, height = 0;

    if (description == NULL || !g_str_has_prefix(description, "wpevideosrc ")) return false;

    // location="..." with \\ and \" escapes, as the Dart side writes it.
    p = strstr(description, "location=\"");
    if (p == NULL) return false;
    p += strlen("location=\"");
    location = g_string_new(NULL);
    for (; *p != '\0' && *p != '"'; p++) {
        if (*p == '\\' && (p[1] == '\\' || p[1] == '"')) p++;
        g_string_append_c(location, *p);
    }
    if (*p != '"') {
        g_string_free(location, TRUE);
        return false;
    }

    // The caps between the first and second "!".
    caps_start = strchr(p, '!');
    if (caps_start == NULL) {
        g_string_free(location, TRUE);
        return false;
    }
    caps_start++;
    {
        const char *caps_end = strchr(caps_start, '!');
        caps_string = caps_end != NULL ? g_strndup(caps_start, (gsize) (caps_end - caps_start)) : g_strdup(caps_start);
    }
    caps = gst_caps_from_string(g_strstrip(caps_string));
    g_free(caps_string);
    if (caps != NULL && gst_caps_get_size(caps) > 0) {
        structure = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(structure, "width", &width);
        gst_structure_get_int(structure, "height", &height);
    }
    if (caps != NULL) gst_caps_unref(caps);
    if (width <= 0 || height <= 0) {
        g_string_free(location, TRUE);
        return false;
    }

    *location_out = g_string_free(location, FALSE);
    *width_out = width;
    *height_out = height;
    return true;
}
