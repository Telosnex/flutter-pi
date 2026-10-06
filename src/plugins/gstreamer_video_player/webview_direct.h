// SPDX-License-Identifier: MIT
/*
 * WPE WebKit webviews driven directly through the WPEPlatform API, with no
 * GStreamer pipeline in between.
 *
 * WebKit runs on one shared thread. Each frame arrives as a DMA-BUF, is copied
 * on the GPU into a flutter-pi scanout buffer (webview_gpu), and is handed
 * back to WebKit right away. Nothing outside the WebKit thread ever holds a
 * WebKit buffer, so closing a view cannot leave one behind.
 *
 * Opt-in with FLUTTERPI_WEBVIEW_DIRECT=1; needs the plane and GPU paths too.
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

/// True when FLUTTERPI_WEBVIEW_DIRECT is set.
bool webview_direct_requested(void);

/// Reads location, width and height from the `wpevideosrc ... ! video/x-raw,...`
/// description the Dart side builds. Returns false if it is not one.
bool webview_direct_parse_pipeline(const char *description, char **location_out, int *width_out, int *height_out);

/// Creates a view and starts loading @location. Blocks until the view exists.
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

/// Translates a GstNavigation event into a WPE event. Takes ownership of @event.
int webview_direct_send_navigation_event(struct webview_direct *view, GstEvent *event);

#endif  // _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_DIRECT_H
