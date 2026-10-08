// SPDX-License-Identifier: MIT
// End-to-end test of webview_direct.c + webview_gpu.c on the Pi, without the
// compositor: a stub dmabuf_surface keeps the newest frame on "screen" and
// reads its centre pixel back from the copied scanout buffer.
#define _GNU_SOURCE
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <dirent.h>
#include <gbm.h>
#include <gst/gst.h>
#include <gst/video/navigation.h>
#include <wpe/webkit.h>

#include "compositor_ng.h"
#include "dmabuf_surface.h"
#include "modesetting.h"
#include "plugins/gstreamer_video_player/webview_direct.h"
#include "plugins/gstreamer_video_player/webview_gpu.h"

/* ---- stub surface: one frame on screen, release the previous one ---- */
struct dmabuf_surface {
    GMutex lock;
    struct dmabuf shown;
    dmabuf_release_cb_t shown_release;
    bool has_shown;
    guint32 pixel;  // 0xRRGGBB of the centre of the newest frame
    guint64 frames;
};

static guint32 centre_pixel(const struct dmabuf *b) {
    size_t size = (size_t) b->strides[0] * b->height;
    uint8_t *map = mmap(NULL, size, PROT_READ, MAP_SHARED, b->fds[0], 0);
    if (map == MAP_FAILED)
        return 0xFFFFFFFF;
    const uint8_t *p = map + (size_t) (b->height / 2) * b->strides[0] + (size_t) (b->width / 2) * 4;
    guint32 v = ((guint32) p[2] << 16) | ((guint32) p[1] << 8) | p[0];
    munmap(map, size);
    return v;
}

int dmabuf_surface_push_dmabuf(struct dmabuf_surface *s, const struct dmabuf *buf, dmabuf_release_cb_t release_cb) {
    guint32 px = centre_pixel(buf);
    g_mutex_lock(&s->lock);
    if (s->has_shown)
        s->shown_release(&s->shown);
    s->shown = *buf;
    s->shown_release = release_cb;
    s->has_shown = true;
    s->pixel = px;
    s->frames++;
    g_mutex_unlock(&s->lock);
    return 0;
}

static void surface_clear(struct dmabuf_surface *s) {
    g_mutex_lock(&s->lock);
    if (s->has_shown)
        s->shown_release(&s->shown);
    s->has_shown = false;
    s->pixel = 0;
    s->frames = 0;
    g_mutex_unlock(&s->lock);
}

static guint32 surface_pixel(struct dmabuf_surface *s, guint64 *frames) {
    g_mutex_lock(&s->lock);
    guint32 v = s->pixel;
    if (frames)
        *frames = s->frames;
    g_mutex_unlock(&s->lock);
    return v;
}

/* ---- stubs for the compositor side ---- */
bool dmabuf_surface_take_import_failed(struct dmabuf_surface *s) {
    (void) s;
    return false;
}
unsigned dmabuf_surface_take_unshown_count(void) {
    return 0;
}
bool kms_trace_enabled(void) {
    return false;
}
void kms_trace(const char *fmt, ...) {
    (void) fmt;
}

struct flip {
    void_callback_t cb;
    void *userdata;
};
static gpointer flip_thread(gpointer p) {
    struct flip *f = p;
    g_usleep(16667);  // one 60 Hz refresh
    f->cb(f->userdata);
    g_free(f);
    return NULL;
}
static struct compositor {
    int unused;
} fake_compositor;
int compositor_request_repaint(struct compositor *c, void_callback_t on_scanout, void *userdata) {
    (void) c;
    struct flip *f = g_new(struct flip, 1);
    f->cb = on_scanout;
    f->userdata = userdata;
    g_thread_unref(g_thread_new("flip", flip_thread, f));
    return 0;
}

bool compositor_cancel_repaint(struct compositor *c, void_callback_t cb, void *data) {
    // The fake flip thread takes the callback immediately (already dispatched).
    (void) c;
    (void) cb;
    (void) data;
    return false;
}

/* ---- helpers ---- */
static int fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    while (d && readdir(d))
        n++;
    if (d)
        closedir(d);
    return n - 2;
}
static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char l[256];
    long v = -1;
    while (f && fgets(l, sizeof l, f))
        if (sscanf(l, "VmRSS: %ld", &v) == 1)
            break;
    if (f)
        fclose(f);
    return v;
}

static int failures;
static bool near(guint32 a, guint32 b) {
    for (int s = 0; s < 24; s += 8) {
        int d = (int) ((a >> s) & 0xFF) - (int) ((b >> s) & 0xFF);
        if (d < -12 || d > 12)
            return false;
    }
    return true;
}
static bool wait_pixel(struct dmabuf_surface *s, guint32 want, int timeout_ms, const char *what) {
    gint64 start = g_get_monotonic_time();
    guint32 got = 0;
    while ((g_get_monotonic_time() - start) / 1000 < timeout_ms) {
        got = surface_pixel(s, NULL);
        if (near(got, want)) {
            printf("  PASS %-34s #%06x after %4.0f ms\n", what, got, (g_get_monotonic_time() - start) / 1000.0);
            return true;
        }
        g_usleep(5000);
    }
    printf("  FAIL %-34s want #%06x got #%06x\n", what, want, got);
    failures++;
    return false;
}

struct gview {
    struct webview_direct *direct;
    unsigned id;
    atomic_uint expected_document, loaded_document, failed_document;
};
static atomic_int configured;
static void on_load(WebKitWebView *wv, WebKitLoadEvent event, gpointer userdata) {
    struct gview *g = userdata;
    if (event != WEBKIT_LOAD_FINISHED)
        return;
    const char *uri = webkit_web_view_get_uri(wv);
    const char *url = getenv("HARNESS_URL");
    unsigned id = 0, document = 0;
    const char *title = webkit_web_view_get_title(wv);
    bool match = title && sscanf(title, "tpi-fixture-%u-%u", &id, &document) == 2 && id == g->id;
    if (url && uri && g_str_has_prefix(uri, url)) {
        match = true;
        document = atomic_load(&g->expected_document);
    }
    printf("  LOAD view=%u document=%u matched=%d uri=%s\n", g->id, document, match, uri ? uri : "(null)");
    if (match)
        atomic_store(&g->loaded_document, document);
}
static void on_title(GObject *wv, GParamSpec *pspec, gpointer userdata) {
    (void) pspec;
    if (!getenv("HARNESS_URL"))
        on_load(WEBKIT_WEB_VIEW(wv), WEBKIT_LOAD_FINISHED, userdata);
}
static gboolean on_load_failed(WebKitWebView *wv, WebKitLoadEvent event, const char *uri, GError *error, gpointer userdata) {
    (void) wv;
    (void) event;
    struct gview *g = userdata;
    printf("  LOAD FAILED view=%u uri=%s: %s\n", g->id, uri, error->message);
    atomic_store(&g->failed_document, atomic_load(&g->expected_document));
    return FALSE;
}
static void on_configure(GObject *web_view, void *userdata) {
    g_signal_connect(web_view, "load-failed", G_CALLBACK(on_load_failed), userdata);
    g_signal_connect(web_view, "notify::title", G_CALLBACK(on_title), userdata);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load), userdata);
    atomic_fetch_add(&configured, 1);
}
static bool wait_document(struct gview *g) {
    gint64 end = g_get_monotonic_time() + 15000000;
    unsigned document = atomic_load(&g->expected_document);
    while (atomic_load(&g->loaded_document) != document && atomic_load(&g->failed_document) != document && g_get_monotonic_time() < end)
        g_usleep(5000);
    bool loaded = atomic_load(&g->loaded_document) == document && atomic_load(&g->failed_document) != document;
    printf("  %s requested document view=%u\n", loaded ? "PASS" : "FAIL", g->id);
    if (!loaded)
        failures++;
    return loaded;
}

static const char *PAGE =
    "<!doctype html><html><body style='margin:0;height:8000px;background:rgb(0,0,255)'><script>"
    "const c=(r,g,b)=>document.body.style.background=`rgb(${r},${g},${b})`;"
    "addEventListener('scroll',()=>{if(scrollY>0)c(0,255,0)});"
    "let touched=false;"
    "addEventListener('mousedown',e=>{if(e.button===0&&!touched)c(255,255,0)});"
    "addEventListener('click',e=>{if(touched)c(128,0,128)});"
    "addEventListener('keydown',e=>{if(e.key==='a')c(0,255,255);if(e.key==='Enter')c(255,128,0)});"
    "addEventListener('touchstart',e=>{touched=true;c(255,0,255)});"
    "</script></body></html>";

static const char *LAT =
    "<!doctype html><body style='margin:0;background:#000'><script>let n=0;"
    "addEventListener('mousedown',()=>{n++;document.body.style.background=n%2?'#fff':'#000'});"
    "</script>";

static const char *ANIM =
    "<!doctype html><body style='margin:0'><canvas id=c></canvas><script>"
    "const c=document.getElementById('c');c.width=innerWidth;c.height=innerHeight;"
    "const g=c.getContext('2d');let t=0;function f(){t++;g.fillStyle='hsl('+(t%360)+',70%,40%)';"
    "g.fillRect(0,0,c.width,c.height);requestAnimationFrame(f)}f();</script>";

/* ---- real direct backend; the compositor/scanout clock above is a fixture ---- */
static struct webview_gpu *g_gpu;
static struct dmabuf_surface *g_surface;
static struct gview *gv_new_on_surface(int w, int h, struct dmabuf_surface *surface) {
    static unsigned next_id = 1;
    struct gview *g = g_new0(struct gview, 1);
    g->id = next_id++;
    atomic_init(&g->expected_document, 1);
    atomic_init(&g->loaded_document, 0);
    atomic_init(&g->failed_document, 0);
    g->direct = webview_direct_new(g_gpu, surface, &fake_compositor, w, h, "about:blank", on_configure, g);
    if (!g->direct) {
        g_free(g);
        return NULL;
    }
    return g;
}
static struct gview *gv_new(int w, int h) {
    return gv_new_on_surface(w, h, g_surface);
}
static void gv_load(struct gview *g, const char *html) {
    unsigned document = atomic_fetch_add(&g->expected_document, 1) + 1;
    char *page = g_strdup_printf("%s<script>document.title='tpi-fixture-%u-%u'</script>", html, g->id, document);
    webview_direct_load_bytes(g->direct, (const uint8_t *) page, strlen(page));
    g_free(page);
    wait_document(g);
}
static void gv_js(struct gview *g, const char *src) {
    webview_direct_run_javascript(g->direct, src);
}
static void gv_send(struct gview *g, GstEvent *e) {
    webview_direct_send_navigation_event(g->direct, e);
}
static void gv_free(struct gview *g) {
    webview_direct_destroy(g->direct);  // Disconnects load handlers by userdata.
    printf("  CLOSE view=%u\n", g->id);
    g_free(g);
}

/* Real WPE viewport + frame-size regression, without changing the app service. */
static bool wait_size(struct dmabuf_surface *s, unsigned width, unsigned height) {
    gint64 end = g_get_monotonic_time() + 5000000;
    do {
        g_mutex_lock(&s->lock);
        bool match = s->has_shown && s->shown.width == width && s->shown.height == height;
        g_mutex_unlock(&s->lock);
        if (match) {
            printf("  PASS native frame size %ux%u\n", width, height);
            return true;
        }
        g_usleep(5000);
    } while (g_get_monotonic_time() < end);
    printf("  FAIL native frame size %ux%u\n", width, height);
    failures++;
    return false;
}

static void viewport_regression(struct dmabuf_surface *surface) {
    struct gview *v = gv_new(720, 600);
    if (!v) { failures++; return; }
    gv_load(v,
        "<!doctype html><body style='margin:0;background:#0000ff'><script>"
        "window.fixtureState=42;"
        "addEventListener('resize',()=>{document.body.style.background="
        "innerWidth===720&&innerHeight===178&&fixtureState===84?'#00ff00':"
        "innerWidth===480&&innerHeight===420&&fixtureState===85?'#800080':'#ff0000'});"
        "addEventListener('mousedown',e=>{if(e.clientX===200&&e.clientY===100&&fixtureState===84){"
        "fixtureState=85;document.body.style.background='#00ffff'}else{document.body.style.background='#ff0000'}});"
        "</script>");
    wait_pixel(surface, 0x0000FF, 5000, "viewport initial page");
    gv_js(v, "fixtureState=84;document.body.style.background='#ffff00'");
    wait_pixel(surface, 0xFFFF00, 2000, "mutated state before resize");
    if (webview_direct_resize(v->direct, 720, 178) != 0) failures++;
    wait_size(surface, 720, 178);
    wait_pixel(surface, 0x00FF00, 5000, "short viewport + state retained");
    gv_send(v, gst_navigation_event_new_mouse_button_press(1, 200, 100, 0));
    gv_send(v, gst_navigation_event_new_mouse_button_release(1, 200, 100, 0));
    wait_pixel(surface, 0x00FFFF, 2000, "click uses resized coordinates");
    if (webview_direct_resize(v->direct, 480, 420) != 0) failures++;
    wait_size(surface, 480, 420);
    wait_pixel(surface, 0x800080, 5000, "grown viewport + clicked state retained");
    // Repeated resize while frames are outstanding, then close with work queued.
    for (int i = 0; i < 20; i++) {
        unsigned width = i % 2 ? 480 : 720, height = i % 2 ? 420 : 178;
        if (webview_direct_resize(v->direct, width, height) != 0) failures++;
        wait_size(surface, width, height);
    }
    webview_direct_resize(v->direct, 720, 178);
    gv_free(v);
    surface_clear(surface);
}

/* CPU time of this process and all its descendants, in seconds. */
static double tree_cpu(void) {
    long hz = sysconf(_SC_CLK_TCK);
    double total = 0;
    pid_t self = getpid();
    DIR *d = opendir("/proc");
    struct dirent *e;
    // Collect (pid, ppid, cpu) and sum the tree rooted at self.
    int pids[4096], ppids[4096];
    double cpu[4096];
    int n = 0;
    while (d && (e = readdir(d)) && n < 4096) {
        int pid = atoi(e->d_name);
        if (pid <= 0)
            continue;
        char path[64], buf[1024];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        size_t len = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[len] = 0;
        char *rp = strrchr(buf, ')');
        if (!rp)
            continue;
        int ppid;
        unsigned long ut, st;
        if (sscanf(rp + 2, "%*c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ppid, &ut, &st) != 3)
            continue;
        pids[n] = pid;
        ppids[n] = ppid;
        cpu[n] = (double) (ut + st) / hz;
        n++;
    }
    if (d)
        closedir(d);
    bool in[4096] = { 0 };
    for (int i = 0; i < n; i++)
        if (pids[i] == self)
            in[i] = true;
    for (int pass = 0; pass < 6; pass++)
        for (int i = 0; i < n; i++)
            if (!in[i])
                for (int j = 0; j < n; j++)
                    if (in[j] && ppids[i] == pids[j]) {
                        in[i] = true;
                        break;
                    }
    for (int i = 0; i < n; i++)
        if (in[i])
            total += cpu[i];
    return total;
}

/* WebKit child processes (direct children or under bwrap): count of
 * WPEWebProcess and RSS of all descendants, in KB. */
static bool print_children;
static void children(int *n_web, long *rss_kb_out) {
    pid_t self = getpid();
    static int pids[4096], ppids[4096];
    static long rss[4096];
    static bool web[4096];
    int n = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 4096) {
        int pid = atoi(e->d_name);
        if (pid <= 0)
            continue;
        char path[64], line[256];
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        int ppid = -1;
        long r = 0;
        char name[64] = "";
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "Name: %63s", name);
            sscanf(line, "PPid: %d", &ppid);
            sscanf(line, "VmRSS: %ld", &r);
        }
        fclose(f);
        pids[n] = pid;
        ppids[n] = ppid;
        rss[n] = r;
        web[n] = strncmp(name, "WPEWebProcess", 13) == 0;
        n++;
    }
    if (d)
        closedir(d);
    static bool in[4096];
    memset(in, 0, sizeof in);
    for (int pass = 0; pass < 6; pass++)
        for (int i = 0; i < n; i++)
            if (!in[i])
                for (int j = 0; j < n; j++)
                    if (ppids[i] == self || (in[j] && ppids[i] == pids[j])) {
                        in[i] = true;
                        break;
                    }
    *n_web = 0;
    *rss_kb_out = 0;
    for (int i = 0; i < n; i++)
        if (in[i]) {
            *rss_kb_out += rss[i];
            if (web[i])
                (*n_web)++;
            if (print_children)
                printf("    child pid=%d ppid=%d web=%d rss=%ld KB\n", pids[i], ppids[i], web[i], rss[i]);
        }
}

/* After a close: how long until the web process is gone (ms), or -1. */
static double wait_web_gone(int timeout_ms) {
    gint64 t0 = g_get_monotonic_time();
    int nw;
    long r;
    do {
        children(&nw, &r);
        if (nw == 0)
            return (g_get_monotonic_time() - t0) / 1000.0;
        g_usleep(20000);
    } while ((g_get_monotonic_time() - t0) / 1000 < timeout_ms);
    return -1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int cycles = argc > 1 ? atoi(argv[1]) : 30;
    if (cycles < 1 || cycles > 100) {
        fprintf(stderr, "cycles must be 1..100\n");
        return 1;
    }
    int w = 1920, h = 1141;
    gst_init(NULL, NULL);
    setenv("FLUTTERPI_WEBVIEW_STATS", "1", 0);

    const char *device = getenv("HARNESS_DRM_DEVICE");
    int drm = open(device ? device : "/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        perror("open DRM device");
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(drm);
    struct webview_gpu *gpu = webview_gpu_new(gbm);
    if (!gpu) {
        printf("FAIL webview_gpu_new\n");
        return 1;
    }
    struct dmabuf_surface surface = { 0 };
    g_mutex_init(&surface.lock);
    g_gpu = gpu;
    g_surface = &surface;
    printf("backend: direct (WPEPlatform), pid=%d\n", getpid());

    if (getenv("HARNESS_VIEWPORT_ONLY")) {
        viewport_regression(&surface);
        g_usleep(200000);
        webview_gpu_unref(gpu);
        printf("%s: viewport regression, %d failures\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 2 : 0;
    }

    printf("== functional (%dx%d)\n", w, h);
    struct gview *v;
    if (getenv("HARNESS_LIFECYCLE"))
        goto cycles;
    v = gv_new(w, h);
    if (!v) {
        printf("FAIL create\n");
        return 1;
    }
    if (atomic_load(&configured) != 1) {
        printf("FAIL configure callback\n");
        failures++;
    }
    gv_load(v, PAGE);
    wait_pixel(&surface, 0x0000FF, 5000, "load_bytes renders (blue)");
    gv_js(v, "c(255,0,0)");
    wait_pixel(&surface, 0xFF0000, 2000, "run_javascript (red)");
    gv_send(v, gst_navigation_event_new_mouse_move(400, 300, 0));
    gv_send(v, gst_navigation_event_new_mouse_button_press(1, 400, 300, 0));
    gv_send(v, gst_navigation_event_new_mouse_button_release(1, 400, 300, 0));
    wait_pixel(&surface, 0xFFFF00, 2000, "mouse button 1 (yellow)");
    gv_send(v, gst_navigation_event_new_key_press("a", 0));
    gv_send(v, gst_navigation_event_new_key_release("a", 0));
    wait_pixel(&surface, 0x00FFFF, 2000, "key 'a' (cyan)");
    gv_send(v, gst_navigation_event_new_key_press("Return", 0));
    gv_send(v, gst_navigation_event_new_key_release("Return", 0));
    wait_pixel(&surface, 0xFF8000, 2000, "key Return (orange)");
    gv_send(v, gst_navigation_event_new_touch_down(0, 500, 500, 1.0, 0));
    gv_send(v, gst_navigation_event_new_touch_up(0, 500, 500, 0));
    wait_pixel(&surface, 0x800080, 2000, "touch tap -> touchstart + click");
    // Negative dy should advance down the page, as on the wpevideosrc path.
    for (int i = 0; i < 5; i++)
        gv_send(v, gst_navigation_event_new_mouse_scroll(400, 300, 0, -40, 0));
    wait_pixel(&surface, 0x00FF00, 2000, "scroll dy<0 moves down (green)");

    printf("== local VP8 video: red then green\n");
    const char *video_path = getenv("HARNESS_VIDEO_FIXTURE");
    char *video_html = NULL;
    if (!video_path || !g_file_get_contents(video_path, &video_html, NULL, NULL)) {
        printf("  FAIL set HARNESS_VIDEO_FIXTURE to test/webview/video.html\n");
        failures++;
    } else {
        gv_load(v, video_html);
        g_free(video_html);
        wait_pixel(&surface, 0xFF0000, 5000, "video first segment (red)");
        wait_pixel(&surface, 0x00FF00, 5000, "video playback advances (green)");
        gv_load(v, PAGE);
        wait_pixel(&surface, 0x0000FF, 5000, "navigate away from video (blue)");
    }

    printf("== input-to-screen latency, 40 clicks\n");
    gv_load(v, LAT);
    wait_pixel(&surface, 0x000000, 5000, "latency page (black)");
    double lat[40];
    int nlat = 0;
    for (int i = 0; i < 40; i++) {
        guint32 want = (i % 2 == 0) ? 0xFFFFFF : 0x000000;
        gint64 t0 = g_get_monotonic_time();
        gv_send(v, gst_navigation_event_new_mouse_button_press(1, 300, 300, 0));
        gv_send(v, gst_navigation_event_new_mouse_button_release(1, 300, 300, 0));
        while (!near(surface_pixel(&surface, NULL), want) && g_get_monotonic_time() - t0 < 1000000)
            g_usleep(500);
        if (near(surface_pixel(&surface, NULL), want))
            lat[nlat++] = (g_get_monotonic_time() - t0) / 1000.0;
        g_usleep(60000);
    }
    for (int i = 0; i < nlat; i++)
        for (int j = i + 1; j < nlat; j++)
            if (lat[j] < lat[i]) {
                double t = lat[i];
                lat[i] = lat[j];
                lat[j] = t;
            }
    if (nlat)
        printf(
            "  %d/40 seen; median %.1f ms, p90 %.1f ms, min %.1f ms, max %.1f ms\n",
            nlat,
            lat[nlat / 2],
            lat[nlat * 9 / 10],
            lat[0],
            lat[nlat - 1]
        );
    if (nlat != 40) {
        printf("  FAIL missed %d/40 clicks\n", 40 - nlat);
        failures++;
    }

    printf("== animated canvas, 10 s\n");
    gv_load(v, ANIM);
    g_usleep(1500000);
    guint64 f0, f1;
    surface_pixel(&surface, &f0);
    double c0 = tree_cpu();
    gint64 t0 = g_get_monotonic_time();
    g_usleep(10000000);
    double secs = (g_get_monotonic_time() - t0) / 1e6;
    double c1 = tree_cpu();
    surface_pixel(&surface, &f1);
    printf("  %.1f fps on screen; CPU %.0f%% of one core (this process + WebKit children)\n", (f1 - f0) / secs, (c1 - c0) / secs * 100);

    printf("== static page, 10 s (idle cost)\n");
    gv_load(v, PAGE);
    g_usleep(1500000);
    surface_pixel(&surface, &f0);
    c0 = tree_cpu();
    t0 = g_get_monotonic_time();
    g_usleep(10000000);
    secs = (g_get_monotonic_time() - t0) / 1e6;
    c1 = tree_cpu();
    surface_pixel(&surface, &f1);
    printf("  %.1f fps on screen; CPU %.0f%% of one core\n", (f1 - f0) / secs, (c1 - c0) / secs * 100);
    gv_free(v);
    surface_clear(&surface);
    {
        double gone = wait_web_gone(5000);
        int nw;
        long r;
        children(&nw, &r);
        if (gone < 0) {
            printf("  FAIL web process still running 5 s after close (%d, children %ld KB)\n", nw, r);
            failures++;
        } else
            printf("  PASS web process exited %.0f ms after close; children now %ld KB\n", gone, r);
    }

cycles:
    printf("== %d open/close cycles (half closed mid-animation)\n", cycles);
    int fds0 = fd_count();
    long rss0 = rss_kb();
    double gone_max = 0;
    int not_gone = 0;
    for (int c = 0; c < cycles; c++) {
        v = gv_new(1280, 720);
        if (!v) {
            printf("  FAIL create in cycle %d\n", c);
            failures++;
            break;
        }
        if (getenv("HARNESS_URL")) {
            webview_direct_load_url(v->direct, getenv("HARNESS_URL"));
            wait_document(v);
            g_usleep(2000000);  // Let subframes/workers start, not just about:blank.
        } else
            gv_load(v, ANIM);
        gint64 start = g_get_monotonic_time();
        guint64 frames = 0;
        int want = (c % 2) ? 1 : 20;
        while (frames < (guint64) want && g_get_monotonic_time() - start < 5000000) {
            surface_pixel(&surface, &frames);
            g_usleep(2000);
        }
        if (frames < (guint64) want) {
            printf("  FAIL insufficient frames in cycle %d\n", c);
            failures++;
        }
        // Queue input and JS right before closing; they must be dropped safely.
        gv_send(v, gst_navigation_event_new_mouse_move(10, 10, 0));
        gv_js(v, "1+1");
        gv_free(v);
        surface_clear(&surface);
        double gone = wait_web_gone(5000);
        if (gone < 0)
            not_gone++;
        else if (gone > gone_max)
            gone_max = gone;
        if (getenv("HARNESS_URL") || c % 10 == 9 || c == cycles - 1) {
            int nw;
            long cr;
            print_children = true;
            children(&nw, &cr);
            print_children = false;
            printf(
                "  cycle %3d: fds %d (start %d), rss %ld KB (start %ld), children %ld KB, web processes %d\n",
                c + 1,
                fd_count(),
                fds0,
                rss_kb(),
                rss0,
                cr,
                nw
            );
        }
    }
    if (not_gone) {
        printf("  %s web process outlived close in %d/%d cycles\n", getenv("HARNESS_URL") ? "RETENTION" : "FAIL", not_gone, cycles);
        if (!getenv("HARNESS_URL"))
            failures++;
    } else
        printf("  PASS web process exited after every close (slowest %.0f ms)\n", gone_max);
    const char *settle_env = getenv("HARNESS_SETTLE_SECONDS");
    int settle = settle_env ? atoi(settle_env) : 0;
    if (settle < 0 || settle > 120) {
        printf("FAIL settle must be 0..120 seconds\n");
        return 1;
    }
    bool settled = false;
    if (settle) {
        printf("== post-close settle, up to %d seconds\n", settle);
        double gone = wait_web_gone(settle * 1000);
        int nw;
        long cr;
        print_children = true;
        children(&nw, &cr);
        print_children = false;
        settled = gone >= 0;
        if (!settled)
            failures++;
        printf(
            "  %s SETTLED after %.0f ms: web processes=%d descendant RSS=%ld KB fds=%d parent RSS=%ld KB\n",
            settled ? "PASS" : "FAIL",
            gone,
            nw,
            cr,
            fd_count(),
            rss_kb()
        );
    }
    if (!getenv("HARNESS_URL")) {
        printf("== simultaneous views, close while loading\n");
        struct dmabuf_surface second = { 0 };
        g_mutex_init(&second.lock);
        v = gv_new(640, 480);
        struct gview *other = gv_new_on_surface(640, 480, &second);
        if (!v || !other)
            return 1;
        gv_load(v, PAGE);
        gv_load(other, PAGE);
        gv_free(v);
        surface_clear(&surface);
        gv_js(other, "c(255,0,0)");
        wait_pixel(&second, 0xFF0000, 5000, "other view survives first close");
        gv_free(other);
        surface_clear(&second);
        g_mutex_clear(&second.lock);
        for (int i = 0; i < 4; i++) {
            v = gv_new(640, 480);
            if (!v)
                return 1;
            webview_direct_load_url(v->direct, "http://127.0.0.1:9/closing");
            gv_js(v, "1+1");
            gv_send(v, gst_navigation_event_new_mouse_move(10, 10, 0));
            gv_free(v);
            surface_clear(&surface);
        }
        if (wait_web_gone(5000) < 0) {
            printf("FAIL web process after close-during-load\n");
            failures++;
        }
    }
    g_usleep(200000);  // Let already-dispatched fake flips drop their references.
    webview_gpu_unref(gpu);
    printf(
        "%s: %d functional failures; retention cycles=%d/%d\n",
        failures ? "FAILED" :
                   (not_gone ? (settled ? "PASSED (delayed worker/process exit)" : "FUNCTIONAL PASS / RETENTION UNRESOLVED") : "PASSED"),
        failures,
        not_gone,
        cycles
    );
    return failures ? 2 : 0;
}
