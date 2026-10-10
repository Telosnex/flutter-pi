// SPDX-License-Identifier: MIT
/*
 * WPE WebKit webviews driven directly through the WPEPlatform API, with no
 * GStreamer pipeline in between.
 *
 * WebKit runs on one shared thread. Frames arrive as DMA-BUFs for direct
 * scanout, with a GPU-copy fallback. Frame acknowledgement is flip-paced;
 * releasing the buffer is separate and waits until KMS retires it. Display
 * holds can outlive a closed view, but their WPE objects are released only on
 * the WebKit thread. Closing a view does not forcibly terminate its process.
 *
 * This is the only web view backend; it needs the plane and the GPU copy.
 */
#ifndef _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_DIRECT_H
#define _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <glib-object.h>
#include <gst/gst.h>

struct compositor;

struct webview_gpu;
struct dmabuf_surface;
struct webview_direct;

/// Called on the WebKit thread with the new WebKitWebView, before the first load.
typedef void (*webview_direct_configure_cb)(GObject *web_view, void *userdata);

/// Creates a view and starts loading @location. Blocks until the view exists.
/// Borrows gpu, surface and compositor until destroy returns. After stopping
/// the producer, the owner must clear/unregister/unref its surface separately;
/// in-flight display buffers keep their own independent references.
struct webview_direct *webview_direct_new(
    struct webview_gpu *gpu,
    struct dmabuf_surface *surface,
    struct compositor *compositor,
    int width,
    int height,
    const char *location,
    webview_direct_configure_cb configure,
    void *userdata
);

/// Closes the view. Blocks until WebKit has dropped it; after this returns, no
/// callback reaches @userdata again.
void webview_direct_destroy(struct webview_direct *view);

int webview_direct_load_url(struct webview_direct *view, const char *url);
int webview_direct_load_bytes(struct webview_direct *view, const uint8_t *data, size_t size);
int webview_direct_run_javascript(struct webview_direct *view, const char *source);
/// Resizes the existing document on the WebKit thread; no reload or new view.
/// Queued work is dropped safely after close. Dimensions must be 1..4096.
int webview_direct_resize(struct webview_direct *view, int width, int height);

/// Translates a GstNavigation event into a WPE event. Takes ownership of @event.
int webview_direct_send_navigation_event(struct webview_direct *view, GstEvent *event);

#endif  // _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_DIRECT_H
