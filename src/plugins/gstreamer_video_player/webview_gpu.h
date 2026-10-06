// SPDX-License-Identifier: MIT
/*
 * WPE webview frames that stay on the GPU.
 *
 * wpevideosrc in GL mode makes the WebProcess render with the GPU and hands
 * over each frame as an EGLImage. This module copies that image, on the GPU,
 * into one of a few scanout buffers owned by flutter-pi, and returns that
 * buffer for a DRM overlay plane. No frame is read back into CPU memory.
 *
 * Compare the system-memory path: there, the WebProcess composites with Mesa's
 * software rasterizer (wl_shm has no GPU buffers), and every frame is then
 * copied by the CPU into a freshly allocated scanout BO.
 */
#ifndef _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_GPU_H
#define _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_GPU_H

#include <stdbool.h>

#include <gst/gst.h>

struct gbm_device;
struct dmabuf;
struct webview_gpu;

/// True when FLUTTERPI_WEBVIEW_GPU is set. Requires the plane path.
bool webview_gpu_requested(void);

/// Creates the GPU frame path on flutter-pi's GBM device. Returns NULL, with a
/// log line, when the EGL extensions it needs are missing.
struct webview_gpu *webview_gpu_new(struct gbm_device *gbm_device);

/// Drops the owner reference. Scanout buffers still on screen keep the object
/// alive until KMS releases them.
void webview_gpu_unref(struct webview_gpu *gpu);

/// Prepares a parsed pipeline before it leaves NULL state: shares the EGL
/// display with wpevideosrc, and changes the caps after "websrc" to GL memory.
int webview_gpu_configure_pipeline(struct webview_gpu *gpu, GstElement *pipeline, GstElement *websrc);

/// Caps the appsink must accept for GL frames.
GstCaps *webview_gpu_appsink_caps(void);

/// Turns off WebKit damage features that leave stale regions in GL frames
/// (seen on WPE 2.54 with WPEBackend-fdo). @webview is a WebKitWebView.
void webview_gpu_configure_web_view(GObject *webview);

/// True when @sample carries a wpevideosrc EGLImage.
bool webview_gpu_sample_has_image(GstSample *sample);

/// Copies the frame into a free scanout buffer and fills @dmabuf_out.
/// Release the buffer with webview_gpu_release_dmabuf(), which is the
/// dmabuf_surface release callback. Returns EAGAIN when all buffers are on
/// screen; drop the frame in that case.
int webview_gpu_copy_sample(struct webview_gpu *gpu, GstSample *sample, struct dmabuf *dmabuf_out);

/// Return the source image while its WPE view is still alive, including drops.
void webview_gpu_release_sample(GstSample *sample);

void webview_gpu_release_dmabuf(struct dmabuf *dmabuf);

#endif  // _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_GPU_H
