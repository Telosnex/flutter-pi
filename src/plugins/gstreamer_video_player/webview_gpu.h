// SPDX-License-Identifier: MIT
/*
 * GPU copy for WPE web view frames.
 *
 * When a WebKit DMA-BUF cannot be scanned out as is, this module copies it,
 * on the GPU, into one of a few scanout buffers owned by flutter-pi, and
 * returns that buffer for a DRM overlay plane. No frame is read back into CPU
 * memory.
 */
#ifndef _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_GPU_H
#define _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_GPU_H

#include <stdbool.h>
#include <stdint.h>

struct gbm_device;
struct dmabuf;
struct webview_gpu;

/// A single-plane DMA-BUF frame, as WPEPlatform hands it over.
struct webview_gpu_source {
    int width, height;
    uint32_t fourcc;
    int fd;
    uint32_t stride, offset;
    uint64_t modifier;
};

/// Creates the GPU frame path on flutter-pi's GBM device. Returns NULL, with a
/// log line, when the EGL extensions it needs are missing.
struct webview_gpu *webview_gpu_new(struct gbm_device *gbm_device);

/// Drops the owner reference. Scanout buffers still on screen keep the object
/// alive until KMS releases them.
void webview_gpu_unref(struct webview_gpu *gpu);

/// Copies the frame into a free scanout buffer and fills @dmabuf_out. Does not
/// take ownership of @src->fd. When this returns, the GPU has finished reading
/// the source, so the caller can hand it back to its producer right away.
/// Release the buffer with webview_gpu_release_dmabuf(), which is the
/// dmabuf_surface release callback. Returns EAGAIN when all buffers are on
/// screen; drop the frame in that case.
int webview_gpu_copy_dmabuf(struct webview_gpu *gpu, const struct webview_gpu_source *src, struct dmabuf *dmabuf_out);

void webview_gpu_release_dmabuf(struct dmabuf *dmabuf);

#endif  // _FLUTTERPI_SRC_PLUGINS_GSTREAMER_VIDEO_PLAYER_WEBVIEW_GPU_H
