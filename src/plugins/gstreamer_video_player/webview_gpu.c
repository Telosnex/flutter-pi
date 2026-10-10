// SPDX-License-Identifier: MIT
/*
 * WPE webview frames that stay on the GPU. See webview_gpu.h.
 */
#define _GNU_SOURCE

#include "plugins/gstreamer_video_player/webview_gpu.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <pthread.h>

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
typedef void (*wg_image_target_texture_t)(GLenum target, void *image);

/// Scanout buffers in rotation: one on screen, one queued, one being drawn.
// One slot on screen, one in a pending page flip, one queued as the
// surface's next frame, and one to copy the newest WebKit frame into. With
// three, a fast producer dropped its newest frame and the queued, older one
// stayed visible.
#define N_SLOTS 4

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
    /// When the copy into this slot finished (diagnostics).
    _Atomic uint64_t filled_ns;
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
    wg_image_target_texture_t image_target_texture;

    bool gl_ready;
    GLuint program, vbo;
    GLint attr_pos, uni_tex;

    struct slot slots[N_SLOTS];
    struct source_entry sources[N_SOURCE_CACHE];
    uint64_t use_counter;

    uint64_t n_frames, n_dropped, n_imports;

    /// Copy timing, for comparing frame paths (FLUTTERPI_WEBVIEW_STATS=1 logs
    /// a line every 5 s; a summary is always logged on destroy).
    bool log_stats;
    uint64_t copy_ns_total, copy_ns_max;
    uint64_t window_start_ns, window_frames, window_dropped, window_copy_ns, window_copy_max_ns;

    /// Drop diagnostics. A drop with every held slot younger than ~2 refreshes
    /// means WebKit delivered frames faster than the screen can show them
    /// (harmless); an old held slot means something kept a buffer too long.
    uint64_t last_arrival_ns;
    uint64_t window_fast_arrivals; // < 8 ms after the previous WebKit frame
    uint64_t window_drops_burst, window_drops_stuck, window_drop_oldest_max_ns;
    /// Updated from the display thread on release.
    _Atomic uint64_t window_releases, window_hold_ns, window_hold_max_ns;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

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
    gpu->log_stats = getenv("FLUTTERPI_WEBVIEW_STATS") != NULL;

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
    gpu->image_target_texture = (wg_image_target_texture_t) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!gpu->create_image || !gpu->destroy_image || !gpu->image_target_texture) {
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
        "webview GPU frames: destroyed after %" PRIu64 " frames, %" PRIu64 " dropped, %" PRIu64 " source imports, copy avg %.2f ms max %.2f ms.\n",
        gpu->n_frames,
        gpu->n_dropped,
        gpu->n_imports,
        gpu->n_frames ? (double) gpu->copy_ns_total / (double) gpu->n_frames / 1e6 : 0.0,
        (double) gpu->copy_ns_max / 1e6
    );
    free(gpu);
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

/// Imports a single-plane DMA-BUF as a texture, cached by the buffer's inode.
/// Does not take ownership of @src->fd.
static GLuint dmabuf_texture(struct webview_gpu *gpu, const struct webview_gpu_source *src) {
    struct source_entry *entry = NULL, *victim = NULL;
    int width = src->width, height = src->height, fd = src->fd;
    uint32_t fourcc = src->fourcc;
    uint64_t modifier = src->modifier;
    EGLint stride = (EGLint) src->stride;
    struct stat st;

    if (fstat(fd, &st) != 0) {
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
        victim->image = import_dmabuf(gpu, width, height, fourcc, fd, src->stride, src->offset, modifier);
        if (victim->image == WG_EGL_NO_IMAGE) {
            LOG_ERROR("webview GPU frames: could not import the WebKit frame (0x%x).\n", eglGetError());
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
    entry->last_use = ++gpu->use_counter;
    return entry->texture;
}

static int copy_common(
    struct webview_gpu *gpu,
    int width,
    int height,
    const struct webview_gpu_source *src,
    struct dmabuf *dmabuf_out
) {
    struct slot *slot = NULL;
    GLuint texture;
    int ok = 0;
    // The direct path copies on WebKit's thread; keep whatever it had current.
    EGLDisplay saved_display = eglGetCurrentDisplay();
    EGLContext saved_context = eglGetCurrentContext();
    EGLSurface saved_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface saved_read = eglGetCurrentSurface(EGL_READ);

    {
        uint64_t arrival = now_ns();
        if (gpu->last_arrival_ns != 0 && arrival - gpu->last_arrival_ns < 8000000ull) gpu->window_fast_arrivals++;
        gpu->last_arrival_ns = arrival;
    }

    for (int i = 0; i < N_SLOTS; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&gpu->slots[i].busy, &expected, true)) {
            slot = &gpu->slots[i];
            break;
        }
    }
    if (slot == NULL) {
        uint64_t now = now_ns(), oldest = 0;
        for (int i = 0; i < N_SLOTS; i++) {
            uint64_t filled = atomic_load(&gpu->slots[i].filled_ns);
            if (filled != 0 && now - filled > oldest) oldest = now - filled;
        }
        if (oldest < 34000000ull) gpu->window_drops_burst++;
        else gpu->window_drops_stuck++;
        if (oldest > gpu->window_drop_oldest_max_ns) gpu->window_drop_oldest_max_ns = oldest;
        gpu->n_dropped++;
        gpu->window_dropped++;
        return EAGAIN;
    }

    pthread_mutex_lock(&gpu->lock);
    uint64_t copy_start_ns = now_ns();
    if (!eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gpu->context)) {
        LOG_ERROR("webview GPU frames: eglMakeCurrent failed (0x%x).\n", eglGetError());
        ok = EIO;
        goto out_unlock;
    }
    if (!ensure_gl(gpu) || !ensure_slot(gpu, slot, width, height)) {
        ok = EIO;
        goto out_release;
    }
    texture = dmabuf_texture(gpu, src);
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
    atomic_store(&slot->filled_ns, now_ns());
    webview_gpu_ref(gpu);
    gpu->n_frames++;
    {
        uint64_t end_ns = now_ns(), took = end_ns - copy_start_ns;
        gpu->copy_ns_total += took;
        if (took > gpu->copy_ns_max) gpu->copy_ns_max = took;
        if (gpu->window_start_ns == 0) gpu->window_start_ns = end_ns;
        gpu->window_frames++;
        gpu->window_copy_ns += took;
        if (took > gpu->window_copy_max_ns) gpu->window_copy_max_ns = took;
        if (gpu->log_stats && end_ns - gpu->window_start_ns >= 5000000000ull) {
            double seconds = (double) (end_ns - gpu->window_start_ns) / 1e9;
            LOG_ERROR_UNPREFIXED(
                "webview GPU frames: %.1f fps, copy avg %.2f ms max %.2f ms, %" PRIu64 " dropped (%dx%d)\n",
                (double) gpu->window_frames / seconds,
                (double) gpu->window_copy_ns / (double) gpu->window_frames / 1e6,
                (double) gpu->window_copy_max_ns / 1e6,
                gpu->window_dropped,
                width,
                height
            );
            {
                uint64_t releases = atomic_exchange(&gpu->window_releases, 0);
                uint64_t hold = atomic_exchange(&gpu->window_hold_ns, 0);
                uint64_t hold_max = atomic_exchange(&gpu->window_hold_max_ns, 0);
                LOG_ERROR_UNPREFIXED(
                    "webview drops: %" PRIu64 " burst, %" PRIu64 " stuck (oldest held %.1f ms); %" PRIu64
                    " frames <8 ms apart; slot held avg %.1f ms max %.1f ms; %u never shown\n",
                    gpu->window_drops_burst,
                    gpu->window_drops_stuck,
                    (double) gpu->window_drop_oldest_max_ns / 1e6,
                    gpu->window_fast_arrivals,
                    releases ? (double) hold / (double) releases / 1e6 : 0.0,
                    (double) hold_max / 1e6,
                    dmabuf_surface_take_unshown_count()
                );
                gpu->window_drops_burst = gpu->window_drops_stuck = gpu->window_drop_oldest_max_ns = 0;
                gpu->window_fast_arrivals = 0;
            }
            gpu->window_start_ns = end_ns;
            gpu->window_frames = gpu->window_dropped = gpu->window_copy_ns = gpu->window_copy_max_ns = 0;
        }
    }

    if (saved_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
    } else {
        eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    pthread_mutex_unlock(&gpu->lock);
    return 0;

out_release:
    if (saved_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
    } else {
        eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
out_unlock:
    pthread_mutex_unlock(&gpu->lock);
    atomic_store(&slot->busy, false);
    return ok;
}

int webview_gpu_copy_dmabuf(struct webview_gpu *gpu, const struct webview_gpu_source *src, struct dmabuf *dmabuf_out) {
    if (src == NULL || src->fd < 0 || src->width <= 0 || src->height <= 0) {
        return EINVAL;
    }
    return copy_common(gpu, src->width, src->height, src, dmabuf_out);
}

void webview_gpu_release_dmabuf(struct dmabuf *dmabuf) {
    struct slot *slot;

    if (dmabuf == NULL || dmabuf->userdata == NULL) {
        return;
    }
    // The slot owns the fd and the BO. Only mark it free for the next frame.
    slot = dmabuf->userdata;
    dmabuf->userdata = NULL;
    {
        uint64_t filled = atomic_exchange(&slot->filled_ns, 0);
        if (filled != 0) {
            uint64_t held = now_ns() - filled, max = atomic_load(&slot->gpu->window_hold_max_ns);
            atomic_fetch_add(&slot->gpu->window_releases, 1);
            atomic_fetch_add(&slot->gpu->window_hold_ns, held);
            while (held > max && !atomic_compare_exchange_weak(&slot->gpu->window_hold_max_ns, &max, held)) {}
        }
    }
    atomic_store(&slot->busy, false);
    webview_gpu_unref_internal(slot->gpu);
}
