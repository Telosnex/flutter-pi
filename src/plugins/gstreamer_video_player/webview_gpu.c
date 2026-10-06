// SPDX-License-Identifier: MIT
/*
 * WPE webview frames that stay on the GPU. See webview_gpu.h.
 */
#define _GNU_SOURCE
#define GST_USE_UNSTABLE_API 1

#include "plugins/gstreamer_video_player/webview_gpu.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/egl/gsteglimage.h>
#include <gst/gl/gl.h>
#include <gst/video/video.h>
#include <pthread.h>
#include <wpe/webkit.h>

#include "dmabuf_surface.h"
#include "egl.h"
#include "gles.h"
#include "pixel_format.h"
#include "util/logging.h"
#include "util/refcounting.h"

// Spelled out so the file builds with LINT_EGL_HEADERS, which hides these.
#define WG_EGL_PLATFORM_GBM 0x31D7
#define WG_EGL_LINUX_DMA_BUF 0x3270
#define WG_EGL_LINUX_DRM_FOURCC 0x3271
#define WG_EGL_DMA_BUF_PLANE0_FD 0x3272
#define WG_EGL_DMA_BUF_PLANE0_OFFSET 0x3273
#define WG_EGL_DMA_BUF_PLANE0_PITCH 0x3274
#define WG_EGL_DMA_BUF_PLANE0_MODIFIER_LO 0x3443
#define WG_EGL_DMA_BUF_PLANE0_MODIFIER_HI 0x3444
#define WG_EGL_NO_IMAGE ((void *) 0)

typedef EGLDisplay (*wg_get_platform_display_t)(EGLenum platform, void *native_display, const EGLint *attribs);
typedef void *(*wg_create_image_t)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attribs);
typedef EGLBoolean (*wg_destroy_image_t)(EGLDisplay dpy, void *image);
typedef EGLBoolean (*wg_export_query_t)(EGLDisplay dpy, void *image, int *fourcc, int *num_planes, uint64_t *modifiers);
typedef EGLBoolean (*wg_export_t)(EGLDisplay dpy, void *image, int *fds, EGLint *strides, EGLint *offsets);
typedef void (*wg_image_target_texture_t)(GLenum target, void *image);

/// Quark wpevideosrc uses to attach its GstEGLImage to each GL memory.
#define WPE_EGL_IMAGE_QUARK "GstWPEEGLImage"

/// Scanout buffers in rotation: one on screen, one queued, one being drawn.
#define N_SLOTS 3

/// WebKit cycles through a few render buffers. Imports are cached per buffer.
#define N_SOURCE_CACHE 6

struct slot {
    struct webview_gpu *gpu;
    atomic_bool busy;
    int width, height;
    struct gbm_bo *bo;
    int fd;
    uint32_t stride;
    uint64_t modifier;
    void *image;
    GLuint texture, framebuffer;
};

struct source_entry {
    dev_t dev;
    ino_t ino;
    int width, height;
    void *image;
    GLuint texture;
    uint64_t last_use;
};

struct webview_gpu {
    refcount_t n_refs;
    struct gbm_device *gbm_device;
    EGLDisplay display;
    EGLContext context;
    pthread_mutex_t lock;

    wg_create_image_t create_image;
    wg_destroy_image_t destroy_image;
    wg_export_query_t export_query;
    wg_export_t export_image;
    wg_image_target_texture_t image_target_texture;

    bool gl_ready;
    GLuint program, vbo;
    GLint attr_pos, uni_tex;

    struct slot slots[N_SLOTS];
    struct source_entry sources[N_SOURCE_CACHE];
    uint64_t use_counter;

    uint64_t n_frames, n_dropped, n_imports;
};

static void webview_gpu_destroy(struct webview_gpu *gpu);

static struct webview_gpu *webview_gpu_ref(struct webview_gpu *gpu) {
    refcount_inc(&gpu->n_refs);
    return gpu;
}

static void webview_gpu_unref_internal(struct webview_gpu *gpu) {
    if (!refcount_dec(&gpu->n_refs)) {
        webview_gpu_destroy(gpu);
    }
}

bool webview_gpu_requested(void) {
    return getenv("FLUTTERPI_WEBVIEW_GPU") != NULL;
}

static bool has_extension(const char *extensions, const char *name) {
    size_t len = strlen(name);
    const char *p = extensions;

    while (p != NULL && (p = strstr(p, name)) != NULL) {
        if ((p == extensions || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
            return true;
        }
        p += len;
    }
    return false;
}

struct webview_gpu *webview_gpu_new(struct gbm_device *gbm_device) {
    struct webview_gpu *gpu;
    wg_get_platform_display_t get_platform_display;
    const char *extensions;
    EGLint major, minor;

    if (gbm_device == NULL) {
        LOG_ERROR("webview GPU frames need a GBM device.\n");
        return NULL;
    }

    get_platform_display = (wg_get_platform_display_t) eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display == NULL) {
        LOG_ERROR("webview GPU frames need eglGetPlatformDisplayEXT.\n");
        return NULL;
    }

    gpu = calloc(1, sizeof *gpu);
    if (gpu == NULL) {
        return NULL;
    }

    // For the same gbm_device this returns flutter-pi's own EGLDisplay when the
    // GL renderer is active. Initializing it again is a no-op, and this module
    // never terminates it.
    gpu->display = get_platform_display(WG_EGL_PLATFORM_GBM, gbm_device, NULL);
    if (gpu->display == EGL_NO_DISPLAY || !eglInitialize(gpu->display, &major, &minor)) {
        LOG_ERROR("webview GPU frames: could not open an EGL display on the GBM device (0x%x).\n", eglGetError());
        goto fail_free;
    }

    extensions = eglQueryString(gpu->display, EGL_EXTENSIONS);
    const char *required[] = {
        "EGL_EXT_image_dma_buf_import",
        "EGL_EXT_image_dma_buf_import_modifiers",
        "EGL_MESA_image_dma_buf_export",
        "EGL_KHR_surfaceless_context",
        "EGL_KHR_no_config_context",
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
        if (!has_extension(extensions, required[i])) {
            LOG_ERROR("webview GPU frames need %s.\n", required[i]);
            goto fail_free;
        }
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        goto fail_free;
    }
    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    gpu->context = eglCreateContext(gpu->display, (EGLConfig) 0, EGL_NO_CONTEXT, context_attribs);
    if (gpu->context == EGL_NO_CONTEXT) {
        LOG_ERROR("webview GPU frames: could not create an EGL context (0x%x).\n", eglGetError());
        goto fail_free;
    }

    gpu->create_image = (wg_create_image_t) eglGetProcAddress("eglCreateImageKHR");
    gpu->destroy_image = (wg_destroy_image_t) eglGetProcAddress("eglDestroyImageKHR");
    gpu->export_query = (wg_export_query_t) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    gpu->export_image = (wg_export_t) eglGetProcAddress("eglExportDMABUFImageMESA");
    gpu->image_target_texture = (wg_image_target_texture_t) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!gpu->create_image || !gpu->destroy_image || !gpu->export_query || !gpu->export_image || !gpu->image_target_texture) {
        LOG_ERROR("webview GPU frames: missing EGL image entry points.\n");
        goto fail_destroy_context;
    }

    // GPU frames only help with a hardware renderer. A software display (for
    // example vkms) also crashes inside WPEBackend-fdo's EGL setup.
    if (!eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gpu->context)) {
        LOG_ERROR("webview GPU frames: eglMakeCurrent failed (0x%x).\n", eglGetError());
        goto fail_destroy_context;
    }
    const char *renderer = (const char *) glGetString(GL_RENDERER);
    bool software = renderer == NULL || strstr(renderer, "llvmpipe") != NULL || strstr(renderer, "softpipe") != NULL ||
                    strstr(renderer, "swrast") != NULL;
    LOG_ERROR_UNPREFIXED("webview GPU frames: renderer %s.\n", renderer != NULL ? renderer : "(unknown)");
    eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (software) {
        LOG_ERROR("webview GPU frames: the GBM device has no hardware renderer. Using system-memory frames.\n");
        goto fail_destroy_context;
    }

    pthread_mutex_init(&gpu->lock, NULL);
    gpu->gbm_device = gbm_device;
    gpu->n_refs = REFCOUNT_INIT_1;
    for (int i = 0; i < N_SLOTS; i++) {
        gpu->slots[i].gpu = gpu;
        gpu->slots[i].fd = -1;
        atomic_init(&gpu->slots[i].busy, false);
    }

    LOG_ERROR_UNPREFIXED("webview GPU frames: EGL %d.%d on the flutter-pi GBM device.\n", major, minor);
    return gpu;

fail_destroy_context:
    eglDestroyContext(gpu->display, gpu->context);

fail_free:
    free(gpu);
    return NULL;
}

void webview_gpu_unref(struct webview_gpu *gpu) {
    if (gpu != NULL) {
        webview_gpu_unref_internal(gpu);
    }
}

static void release_slot_gl(struct webview_gpu *gpu, struct slot *slot) {
    if (slot->framebuffer) glDeleteFramebuffers(1, &slot->framebuffer);
    if (slot->texture) glDeleteTextures(1, &slot->texture);
    if (slot->image) gpu->destroy_image(gpu->display, slot->image);
    if (slot->fd >= 0) close(slot->fd);
    if (slot->bo) gbm_bo_destroy(slot->bo);
    slot->framebuffer = slot->texture = 0;
    slot->image = NULL;
    slot->fd = -1;
    slot->bo = NULL;
    slot->width = slot->height = 0;
}

static void release_source_gl(struct webview_gpu *gpu, struct source_entry *entry) {
    if (entry->texture) glDeleteTextures(1, &entry->texture);
    if (entry->image) gpu->destroy_image(gpu->display, entry->image);
    memset(entry, 0, sizeof *entry);
}

static void webview_gpu_destroy(struct webview_gpu *gpu) {
    // The last reference can drop on the KMS thread. The context is not
    // current anywhere else at this point, so take it here.
    eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gpu->context);
    for (int i = 0; i < N_SLOTS; i++) {
        release_slot_gl(gpu, &gpu->slots[i]);
    }
    for (int i = 0; i < N_SOURCE_CACHE; i++) {
        release_source_gl(gpu, &gpu->sources[i]);
    }
    if (gpu->program) glDeleteProgram(gpu->program);
    if (gpu->vbo) glDeleteBuffers(1, &gpu->vbo);
    eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(gpu->display, gpu->context);
    pthread_mutex_destroy(&gpu->lock);
    LOG_ERROR_UNPREFIXED(
        "webview GPU frames: destroyed after %" PRIu64 " frames, %" PRIu64 " dropped, %" PRIu64 " source imports.\n",
        gpu->n_frames,
        gpu->n_dropped,
        gpu->n_imports
    );
    free(gpu);
}

int webview_gpu_configure_pipeline(struct webview_gpu *gpu, GstElement *pipeline, GstElement *websrc) {
    GstGLDisplayEGL *gl_display;
    GstContext *context;
    GstPad *src_pad, *peer;
    GstElement *filter;
    GstCaps *caps, *gl_caps;

    // One EGL display for WebKit's frames and for this module's copies, so
    // wpevideosrc's EGLImages are valid here without a dmabuf round trip, and
    // GStreamer does not open a second DRM device of its own.
    gl_display = gst_gl_display_egl_new_with_egl_display(gpu->display);
    if (gl_display == NULL) {
        LOG_ERROR("webview GPU frames: could not wrap the EGL display for GStreamer.\n");
        return EIO;
    }
    context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
    gst_context_set_gl_display(context, GST_GL_DISPLAY(gl_display));
    gst_element_set_context(pipeline, context);
    gst_context_unref(context);
    gst_object_unref(gl_display);

    // The app describes the pipeline with system-memory caps. Keep its size and
    // rate, and only change the memory type and format.
    src_pad = gst_element_get_static_pad(websrc, "src");
    peer = src_pad != NULL ? gst_pad_get_peer(src_pad) : NULL;
    filter = peer != NULL ? gst_pad_get_parent_element(peer) : NULL;
    if (src_pad) gst_object_unref(src_pad);
    if (peer) gst_object_unref(peer);

    if (filter == NULL || g_object_class_find_property(G_OBJECT_GET_CLASS(filter), "caps") == NULL) {
        LOG_ERROR("webview GPU frames: \"websrc\" must link to a capsfilter.\n");
        if (filter) gst_object_unref(filter);
        return EINVAL;
    }

    g_object_get(filter, "caps", &caps, NULL);
    gl_caps = gst_caps_new_empty();
    for (guint i = 0; caps != NULL && i < gst_caps_get_size(caps); i++) {
        GstStructure *s = gst_structure_copy(gst_caps_get_structure(caps, i));
        gst_structure_set(s, "format", G_TYPE_STRING, "RGBA", NULL);
        gst_caps_append_structure_full(gl_caps, s, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
    }
    if (caps == NULL || gst_caps_is_empty(gl_caps)) {
        gst_caps_append_structure_full(
            gl_caps,
            gst_structure_new("video/x-raw", "format", G_TYPE_STRING, "RGBA", NULL),
            gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL)
        );
    }
    g_object_set(filter, "caps", gl_caps, NULL);
    {
        gchar *s = gst_caps_to_string(gl_caps);
        LOG_ERROR_UNPREFIXED("webview GPU frames: websrc caps %s\n", s);
        g_free(s);
    }
    if (caps) gst_caps_unref(caps);
    gst_caps_unref(gl_caps);
    gst_object_unref(filter);
    return 0;
}

GstCaps *webview_gpu_appsink_caps(void) {
    GstCaps *caps = gst_caps_new_empty();
    gst_caps_append_structure_full(
        caps,
        gst_structure_new("video/x-raw", "format", G_TYPE_STRING, "RGBA", NULL),
        gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL)
    );
    return caps;
}

void webview_gpu_configure_web_view(GObject *object) {
    WebKitWebView *webview = WEBKIT_WEB_VIEW(object);
    WebKitSettings *settings = webkit_web_view_get_settings(webview);
    WebKitFeatureList *features = webkit_settings_get_all_features();

    // WPE 2.54 repaints only the damaged region of each frame, but tracks one
    // render target while WPEBackend-fdo rotates several EGL buffers. The other
    // buffers keep stale or never-painted (black) regions. The system-memory
    // path is not affected. Full repaints cost GPU time only.
    for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
        WebKitFeature *feature = webkit_feature_list_get(features, i);
        const char *id = webkit_feature_get_identifier(feature);
        if (strcmp(id, "PropagateDamagingInformation") == 0 || strcmp(id, "UseDamagingInformationForCompositing") == 0) {
            webkit_settings_set_feature_enabled(settings, feature, FALSE);
        }
    }
    webkit_feature_list_unref(features);
}

static GstEGLImage *sample_image(GstSample *sample, GstMemory **memory_out) {
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMemory *memory;

    if (buffer == NULL || gst_buffer_n_memory(buffer) < 1) {
        return NULL;
    }
    memory = gst_buffer_peek_memory(buffer, 0);
    if (memory_out != NULL) {
        *memory_out = memory;
    }
    return gst_mini_object_get_qdata(GST_MINI_OBJECT_CAST(memory), g_quark_from_static_string(WPE_EGL_IMAGE_QUARK));
}

bool webview_gpu_sample_has_image(GstSample *sample) {
    return sample_image(sample, NULL) != NULL;
}

static const char *kVertexShader =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  uv = pos * 0.5 + 0.5;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char *kFragmentShader =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        LOG_ERROR("webview GPU frames: shader compile failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool ensure_gl(struct webview_gpu *gpu) {
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vs, fs;
    GLint ok = 0;

    if (gpu->gl_ready) {
        return true;
    }
    vs = compile(GL_VERTEX_SHADER, kVertexShader);
    fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) {
        return false;
    }
    gpu->program = glCreateProgram();
    glAttachShader(gpu->program, vs);
    glAttachShader(gpu->program, fs);
    glLinkProgram(gpu->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(gpu->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOG_ERROR("webview GPU frames: program link failed.\n");
        return false;
    }
    gpu->attr_pos = glGetAttribLocation(gpu->program, "pos");
    gpu->uni_tex = glGetUniformLocation(gpu->program, "tex");
    glGenBuffers(1, &gpu->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    gpu->gl_ready = true;
    return true;
}

static void *import_dmabuf(struct webview_gpu *gpu, int width, int height, uint32_t fourcc, int fd, uint32_t stride, uint32_t offset, uint64_t modifier) {
    EGLint attribs[32];
    int n = 0;

    attribs[n++] = EGL_WIDTH;
    attribs[n++] = width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = height;
    attribs[n++] = WG_EGL_LINUX_DRM_FOURCC;
    attribs[n++] = (EGLint) fourcc;
    attribs[n++] = WG_EGL_DMA_BUF_PLANE0_FD;
    attribs[n++] = fd;
    attribs[n++] = WG_EGL_DMA_BUF_PLANE0_OFFSET;
    attribs[n++] = (EGLint) offset;
    attribs[n++] = WG_EGL_DMA_BUF_PLANE0_PITCH;
    attribs[n++] = (EGLint) stride;
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        attribs[n++] = WG_EGL_DMA_BUF_PLANE0_MODIFIER_LO;
        attribs[n++] = (EGLint) (modifier & 0xFFFFFFFF);
        attribs[n++] = WG_EGL_DMA_BUF_PLANE0_MODIFIER_HI;
        attribs[n++] = (EGLint) (modifier >> 32);
    }
    attribs[n++] = EGL_NONE;
    return gpu->create_image(gpu->display, EGL_NO_CONTEXT, WG_EGL_LINUX_DMA_BUF, NULL, attribs);
}

static bool ensure_slot(struct webview_gpu *gpu, struct slot *slot, int width, int height) {
    if (slot->bo != NULL && slot->width == width && slot->height == height) {
        return true;
    }
    release_slot_gl(gpu, slot);

    // XRGB8888: the plane ignores the fourth byte, and web content is opaque.
    slot->bo = gbm_bo_create(gpu->gbm_device, width, height, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (slot->bo == NULL) {
        LOG_ERROR("webview GPU frames: could not allocate a %dx%d scanout buffer.\n", width, height);
        return false;
    }
    slot->fd = gbm_bo_get_fd(slot->bo);
    slot->stride = gbm_bo_get_stride(slot->bo);
    slot->modifier = gbm_bo_get_modifier(slot->bo);
    slot->width = width;
    slot->height = height;
    if (slot->fd < 0) {
        release_slot_gl(gpu, slot);
        return false;
    }

    slot->image = import_dmabuf(gpu, width, height, DRM_FORMAT_XRGB8888, slot->fd, slot->stride, gbm_bo_get_offset(slot->bo, 0), slot->modifier);
    if (slot->image == WG_EGL_NO_IMAGE) {
        LOG_ERROR("webview GPU frames: could not import the scanout buffer into EGL (0x%x).\n", eglGetError());
        release_slot_gl(gpu, slot);
        return false;
    }

    glGenTextures(1, &slot->texture);
    glBindTexture(GL_TEXTURE_2D, slot->texture);
    gpu->image_target_texture(GL_TEXTURE_2D, slot->image);
    glGenFramebuffers(1, &slot->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot->texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("webview GPU frames: scanout buffer is not renderable.\n");
        release_slot_gl(gpu, slot);
        return false;
    }
    LOG_ERROR_UNPREFIXED(
        "webview GPU frames: scanout buffer %dx%d stride %u modifier 0x%016" PRIx64 "\n",
        width,
        height,
        slot->stride,
        slot->modifier
    );
    return true;
}

/// Returns a texture for the source frame. Same-display EGLImages bind
/// directly. Otherwise the image is exported and imported again, cached per
/// WebKit buffer, since WebKit reuses a few buffers.
static GLuint source_texture(struct webview_gpu *gpu, GstEGLImage *gst_image, int width, int height) {
    GstGLContext *gl_context = gst_image->context;
    EGLDisplay source_display = EGL_NO_DISPLAY;
    struct source_entry *entry = NULL, *victim = NULL;
    void *image = gst_egl_image_get_image(gst_image);
    int fourcc = 0, n_planes = 0, fd = -1;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    EGLint stride = 0, offset = 0;
    struct stat st;

    if (gl_context != NULL) {
        GstGLDisplayEGL *egl = gst_gl_display_egl_from_gl_display(gl_context->display);
        if (egl != NULL) {
            source_display = (EGLDisplay) gst_gl_display_get_handle(GST_GL_DISPLAY(egl));
            gst_object_unref(egl);
        }
    }

    if (source_display == EGL_NO_DISPLAY) {
        return 0;
    }

    if (!gpu->export_query(source_display, image, &fourcc, &n_planes, &modifier) || n_planes != 1 ||
        !gpu->export_image(source_display, image, &fd, &stride, &offset)) {
        LOG_ERROR("webview GPU frames: could not export the WebKit frame (0x%x).\n", eglGetError());
        return 0;
    }

    if (fstat(fd, &st) != 0) {
        close(fd);
        return 0;
    }
    for (int i = 0; i < N_SOURCE_CACHE; i++) {
        struct source_entry *e = &gpu->sources[i];
        if (e->image != NULL && e->dev == st.st_dev && e->ino == st.st_ino && e->width == width && e->height == height) {
            entry = e;
            break;
        }
        if (victim == NULL || e->image == NULL || (victim->image != NULL && e->last_use < victim->last_use)) {
            victim = e;
        }
    }

    if (entry == NULL) {
        release_source_gl(gpu, victim);
        victim->image = import_dmabuf(gpu, width, height, (uint32_t) fourcc, fd, (uint32_t) stride, (uint32_t) offset, modifier);
        if (victim->image == WG_EGL_NO_IMAGE) {
            LOG_ERROR("webview GPU frames: could not import the WebKit frame (0x%x).\n", eglGetError());
            close(fd);
            memset(victim, 0, sizeof *victim);
            return 0;
        }
        glGenTextures(1, &victim->texture);
        glBindTexture(GL_TEXTURE_2D, victim->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gpu->image_target_texture(GL_TEXTURE_2D, victim->image);
        victim->dev = st.st_dev;
        victim->ino = st.st_ino;
        victim->width = width;
        victim->height = height;
        entry = victim;
        if (gpu->n_imports++ == 0) {
            LOG_ERROR_UNPREFIXED(
                "webview GPU frames: WebKit frame %dx%d fourcc %.4s modifier 0x%016" PRIx64 " stride %d\n",
                width,
                height,
                (const char *) &fourcc,
                modifier,
                stride
            );
        }
    }
    close(fd);
    entry->last_use = ++gpu->use_counter;
    return entry->texture;
}

int webview_gpu_copy_sample(struct webview_gpu *gpu, GstSample *sample, struct dmabuf *dmabuf_out) {
    GstEGLImage *gst_image = sample_image(sample, NULL);
    struct slot *slot = NULL;
    GstVideoInfo info;
    GLuint texture;
    int width, height, ok = 0;

    if (gst_image == NULL || !gst_video_info_from_caps(&info, gst_sample_get_caps(sample))) {
        return EINVAL;
    }
    width = GST_VIDEO_INFO_WIDTH(&info);
    height = GST_VIDEO_INFO_HEIGHT(&info);

    for (int i = 0; i < N_SLOTS; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&gpu->slots[i].busy, &expected, true)) {
            slot = &gpu->slots[i];
            break;
        }
    }
    if (slot == NULL) {
        gpu->n_dropped++;
        return EAGAIN;
    }

    pthread_mutex_lock(&gpu->lock);
    if (!eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gpu->context)) {
        LOG_ERROR("webview GPU frames: eglMakeCurrent failed (0x%x).\n", eglGetError());
        ok = EIO;
        goto out_unlock;
    }
    if (!ensure_gl(gpu) || !ensure_slot(gpu, slot, width, height)) {
        ok = EIO;
        goto out_release;
    }
    texture = source_texture(gpu, gst_image, width, height);
    if (texture == 0) {
        ok = EIO;
        goto out_release;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glUseProgram(gpu->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(gpu->uni_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
    glEnableVertexAttribArray((GLuint) gpu->attr_pos);
    glVertexAttribPointer((GLuint) gpu->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // Prototype: wait for the copy here. An IN_FENCE_FD on the KMS commit would
    // let this thread return before the GPU finishes.
    glFinish();

    memset(dmabuf_out, 0, sizeof *dmabuf_out);
    dmabuf_out->format = PIXFMT_XRGB8888;
    dmabuf_out->width = width;
    dmabuf_out->height = height;
    dmabuf_out->fds[0] = slot->fd;
    dmabuf_out->fds[1] = dmabuf_out->fds[2] = dmabuf_out->fds[3] = -1;
    dmabuf_out->strides[0] = (int) slot->stride;
    dmabuf_out->offsets[0] = 0;
    dmabuf_out->has_modifiers = slot->modifier != DRM_FORMAT_MOD_INVALID;
    dmabuf_out->modifiers[0] = slot->modifier;
    dmabuf_out->userdata = slot;
    webview_gpu_ref(gpu);
    gpu->n_frames++;

    eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    pthread_mutex_unlock(&gpu->lock);
    return 0;

out_release:
    eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
out_unlock:
    pthread_mutex_unlock(&gpu->lock);
    atomic_store(&slot->busy, false);
    return ok;
}

void webview_gpu_release_sample(GstSample *sample) {
    GstMemory *memory = NULL;
    if (sample_image(sample, &memory) != NULL) {
        // Release even dropped/failed copies. Otherwise pooled memory can keep
        // the image until after the WPE view is destroyed and crash on close.
        gst_mini_object_set_qdata(GST_MINI_OBJECT_CAST(memory), g_quark_from_static_string(WPE_EGL_IMAGE_QUARK), NULL, NULL);
    }
}

void webview_gpu_release_dmabuf(struct dmabuf *dmabuf) {
    struct slot *slot;

    if (dmabuf == NULL || dmabuf->userdata == NULL) {
        return;
    }
    // The slot owns the fd and the BO. Only mark it free for the next frame.
    slot = dmabuf->userdata;
    dmabuf->userdata = NULL;
    atomic_store(&slot->busy, false);
    webview_gpu_unref_internal(slot->gpu);
}
