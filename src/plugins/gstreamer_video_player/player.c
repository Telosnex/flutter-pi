#define _GNU_SOURCE

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "dmabuf_surface.h"
#include "surface.h"
#include "compositor_ng.h"

#include <pthread.h>

#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstmemory.h>
#include <gst/gstpad.h>
#include <gst/video/gstvideometa.h>
#include <jsc/jsc.h>
#include <sys/eventfd.h>
#include <wpe/webkit.h>

#include "flutter-pi.h"
#include "notifier_listener.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/gstreamer_video_player.h"
#include "texture_registry.h"
#include "util/collection.h"
#include "util/logging.h"

static void on_release_plane_dmabuf(struct dmabuf *dmabuf) {
    if (dmabuf == NULL) {
        return;
    }

    // Frees in the reverse order of dup_gst_frame_as_scanout_dmabuf(): the BO
    // outlives the fb because of the self-import handle cache (see frame.c).
    if (dmabuf->userdata != NULL) {
        gbm_bo_destroy((struct gbm_bo *) dmabuf->userdata);
        dmabuf->userdata = NULL;
    }

    if (dmabuf->fds[0] >= 0) {
        close(dmabuf->fds[0]);
        dmabuf->fds[0] = -1;
    }
}

#ifdef DEBUG
    #define DEBUG_TRACE_BEGIN(player, name) trace_begin(player, name)
    #define DEBUG_TRACE_END(player, name) trace_end(player, name)
    #define DEBUG_TRACE_INSTANT(player, name) trace_instant(player, name)
#else
    #define DEBUG_TRACE_BEGIN(player, name) \
        do {                                \
        } while (0)
    #define DEBUG_TRACE_END(player, name) \
        do {                              \
        } while (0)
    #define DEBUG_TRACE_INSTANT(player, name) \
        do {                                  \
        } while (0)
#endif

#define LOG_GST_SET_STATE_ERROR(_element)                                                                               \
    LOG_ERROR(                                                                                                          \
        "setting gstreamer playback state failed. gst_element_set_state(element name: %s): GST_STATE_CHANGE_FAILURE\n", \
        GST_ELEMENT_NAME(_element)                                                                                      \
    )
#define LOG_GST_GET_STATE_ERROR(_element)                                                                          \
    LOG_ERROR(                                                                                                     \
        "last gstreamer state change failed. gst_element_get_state(element name: %s): GST_STATE_CHANGE_FAILURE\n", \
        GST_ELEMENT_NAME(_element)                                                                                 \
    )

struct incomplete_video_info {
    bool has_resolution;
    bool has_fps;
    bool has_duration;
    bool has_seeking_info;
    struct video_info info;
};

enum playpause_state { kPaused, kPlaying, kStepping };

enum playback_direction { kForward, kBackward };

#define PLAYPAUSE_STATE_AS_STRING(playpause_state) \
    ((playpause_state) == kPaused   ? "paused" :   \
     (playpause_state) == kPlaying  ? "playing" :  \
     (playpause_state) == kStepping ? "stepping" : \
                                      "?")

struct gstplayer {
    pthread_mutex_t lock;

    struct flutterpi *flutterpi;
    void *userdata;

    char *video_uri;
    char *pipeline_description;

    GstStructure *headers;

    /**
     * @brief The desired playback rate that should be used when @ref playpause_state is kPlayingForward. (should be > 0)
     *
     */
    double playback_rate_forward;

    /**
     * @brief The desired playback rate that should be used when @ref playpause_state is kPlayingBackward. (should be < 0)
     *
     */
    double playback_rate_backward;

    /**
     * @brief True if the video should seemlessly start from the beginning once the end is reached.
     *
     */
    atomic_bool looping;

    /**
     * @brief The desired playback state. Either paused, playing, or single-frame stepping.
     *
     */
    enum playpause_state playpause_state;

    /**
     * @brief The desired playback direction.
     *
     */
    enum playback_direction direction;

    /**
     * @brief The actual, currently used playback rate.
     *
     */
    double current_playback_rate;

    /**
     * @brief The position reported if gstreamer position queries fail (for example, because gstreamer is currently
     * seeking to a new position. In that case, fallback_position_ms will be the seeking target position, so we report the
     * new position while we're seeking to it)
     */
    int64_t fallback_position_ms;

    /**
     * @brief True if there's a position that apply_playback_state should seek to.
     *
     */
    bool has_desired_position;

    /**
     * @brief True if gstplayer should seek to the nearest keyframe instead, which is a bit faster.
     *
     */
    bool do_fast_seeking;

    /**
     * @brief The position, if any, that apply_playback_state should seek to.
     *
     */
    int64_t desired_position_ms;

    struct notifier video_info_notifier, buffering_state_notifier, error_notifier, web_event_notifier;

    bool is_initialized;
    bool has_sent_info;
    struct incomplete_video_info info;

    bool has_gst_info;
    GstVideoInfo gst_info;

    struct texture *texture;
    int64_t texture_id;

    /// Set when FLUTTERPI_WEBVIEW_ON_PLANE is in the environment: frames go to a
    /// DRM overlay plane via this surface instead of being uploaded as a texture.
    /// Renderer-agnostic, so it survives under Vulkan where external textures do not.
    struct dmabuf_surface *dmabuf_surface;
    int64_t platform_view_id;

    struct frame_interface *frame_interface;

    GstElement *pipeline, *sink, *websrc;
    GstBus *bus;

    GMutex web_lock;
    GHashTable *pending_web_replies;
    GAsyncQueue *web_tasks;
    GThread *web_thread;
    atomic_bool web_move_pending;
    uint32_t next_web_call_id;
    sd_event_source *busfd_events;

    bool is_live;
};

struct pending_web_reply {
    WebKitScriptMessageReply *reply;
    JSCContext *context;
    GMainContext *main_context;
};

struct web_reply_task {
    struct pending_web_reply *pending;
    char *result_json;
    char *error;
};

enum web_task_type {
    kWebTaskStop,
    kWebTaskNavigation,
    kWebTaskLoadUrl,
    kWebTaskLoadBytes,
    kWebTaskRunJavaScript,
};

enum web_pointer_task_kind {
    kWebPointerTaskNone,
    kWebPointerTaskMove,
};

struct web_task {
    enum web_task_type type;
    enum web_pointer_task_kind pointer_kind;
    GstEvent *navigation_event;
    GBytes *bytes;
    char *string;
};

static gboolean complete_web_reply(void *userdata);

static void destroy_web_task(void *userdata) {
    struct web_task *task = userdata;
    if (task == NULL) return;
    if (task->navigation_event != NULL) gst_event_unref(task->navigation_event);
    if (task->bytes != NULL) g_bytes_unref(task->bytes);
    free(task->string);
    free(task);
}

static void *run_web_tasks(void *userdata) {
    struct gstplayer *player = userdata;
    struct web_task *task;

    for (;;) {
        task = g_async_queue_pop(player->web_tasks);
        if (task->type == kWebTaskStop) {
            destroy_web_task(task);
            break;
        }

        switch (task->type) {
            case kWebTaskNavigation:
                {
                    GstNavigationEventType navigation_type = gst_navigation_event_get_type(task->navigation_event);
                    if (!gst_element_send_event(player->pipeline, task->navigation_event)) {
                        LOG_ERROR("Could not send navigation event (type %d) upstream through the GStreamer pipeline.\n", (int) navigation_type);
                    }
                }
                // gst_element_send_event consumes the event even on failure.
                task->navigation_event = NULL;
                if (task->pointer_kind == kWebPointerTaskMove) {
                    player->web_move_pending = false;
                }
                break;
            case kWebTaskLoadUrl:
                g_object_set(G_OBJECT(player->websrc), "location", task->string, NULL);
                break;
            case kWebTaskLoadBytes:
                g_signal_emit_by_name(player->websrc, "load-bytes", task->bytes);
                break;
            case kWebTaskRunJavaScript:
                g_signal_emit_by_name(player->websrc, "run-javascript", task->string);
                break;
            case kWebTaskStop: UNREACHABLE();
        }
        destroy_web_task(task);
    }
    return NULL;
}

static int enqueue_web_task(struct gstplayer *player, struct web_task *task) {
    ASSERT_NOT_NULL(player);
    ASSERT_NOT_NULL(task);
    if (player->websrc == NULL || player->web_thread == NULL) {
        destroy_web_task(task);
        return EOPNOTSUPP;
    }
    g_async_queue_push(player->web_tasks, task);
    return 0;
}

static const char kWebBridgeScript[] =
    "(() => {"
    "if (window.flutter_inappwebview && window.flutter_inappwebview.__telosnexWpeTexture) return;"
    "Object.defineProperty(window, 'flutter_inappwebview', {"
    "configurable: true, value: {"
    "__telosnexWpeTexture: true,"
    "callHandler(handlerName, ...args) {"
    "return window.webkit.messageHandlers.telosnex.postMessage("
    "JSON.stringify({handlerName, args}));"
    "}"
    "}"
    "});"
    "})();";

static void destroy_web_event(void *value) {
    struct gstplayer_web_event *event = value;
    if (event == NULL) return;
    free(event->url);
    free(event->message);
    free(event);
}

static void destroy_pending_web_reply(void *value) {
    struct pending_web_reply *pending = value;
    struct web_reply_task *task;
    if (pending == NULL) return;
    task = calloc(1, sizeof(*task));
    if (task == NULL) return;
    task->pending = pending;
    task->error = strdup("The flutter-pi WPE view was disposed.");
    if (task->error == NULL) {
        free(task);
        return;
    }
    g_main_context_invoke(pending->main_context, complete_web_reply, task);
}

static void notify_web_event(
    struct gstplayer *player,
    enum gstplayer_web_event_type type,
    uint32_t call_id,
    const char *url,
    const char *message
) {
    struct gstplayer_web_event *event = calloc(1, sizeof(*event));
    if (event == NULL) return;
    event->type = type;
    event->call_id = call_id;
    event->url = url == NULL ? NULL : strdup(url);
    event->message = message == NULL ? NULL : strdup(message);
    if ((url != NULL && event->url == NULL) || (message != NULL && event->message == NULL)) {
        destroy_web_event(event);
        return;
    }
    notifier_notify(&player->web_event_notifier, event);
}

static void on_web_load_changed(WebKitWebView *webview, WebKitLoadEvent load_event, void *userdata) {
    struct gstplayer *player = userdata;
    const char *url = webkit_web_view_get_uri(webview);
    if (load_event == WEBKIT_LOAD_STARTED) {
        notify_web_event(player, kGstplayerWebLoadStart, 0, url, NULL);
    } else if (load_event == WEBKIT_LOAD_FINISHED) {
        notify_web_event(player, kGstplayerWebLoadStop, 0, url, NULL);
    }
}

static gboolean on_web_load_failed(
    WebKitWebView *webview,
    WebKitLoadEvent load_event,
    const char *failing_url,
    GError *error,
    void *userdata
) {
    struct gstplayer *player = userdata;
    (void) webview;
    (void) load_event;
    if (error != NULL && g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) return FALSE;
    notify_web_event(
        player,
        kGstplayerWebLoadError,
        0,
        failing_url,
        error == NULL ? "Unknown WPE load error" : error->message
    );
    return FALSE;
}

static void on_web_javascript_message(WebKitUserContentManager *manager, JSCValue *value, void *userdata) {
    struct gstplayer *player = userdata;
    char *message;
    (void) manager;
    if (value == NULL) return;
    message = jsc_value_to_string(value);
    if (message == NULL) return;
    notify_web_event(player, kGstplayerWebJavaScriptMessage, 0, NULL, message);
    g_free(message);
}

static gboolean on_web_handler_call(
    WebKitUserContentManager *manager,
    JSCValue *value,
    WebKitScriptMessageReply *reply,
    void *userdata
) {
    struct gstplayer *player = userdata;
    struct pending_web_reply *pending;
    GMainContext *main_context;
    JSCContext *context;
    uint32_t call_id;
    char *message;
    (void) manager;

    if (value == NULL || reply == NULL) return FALSE;
    message = jsc_value_to_string(value);
    if (message == NULL) return FALSE;
    context = jsc_value_get_context(value);
    main_context = g_main_context_get_thread_default();
    if (context == NULL || main_context == NULL) {
        g_free(message);
        return FALSE;
    }

    pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        g_free(message);
        return FALSE;
    }
    pending->reply = webkit_script_message_reply_ref(reply);
    pending->context = JSC_CONTEXT(g_object_ref(context));
    pending->main_context = g_main_context_ref(main_context);

    g_mutex_lock(&player->web_lock);
    call_id = player->next_web_call_id++;
    if (call_id == 0) call_id = player->next_web_call_id++;
    g_hash_table_insert(player->pending_web_replies, GUINT_TO_POINTER(call_id), pending);
    g_mutex_unlock(&player->web_lock);

    notify_web_event(player, kGstplayerWebHandlerCall, call_id, NULL, message);
    g_free(message);
    return TRUE;
}

static void on_configure_web_view(GstElement *websrc, GObject *object, void *userdata) {
    struct gstplayer *player = userdata;
    WebKitUserContentManager *manager;
    WebKitUserScript *script;
    WebKitWebView *webview = WEBKIT_WEB_VIEW(object);
    (void) websrc;

    manager = webkit_web_view_get_user_content_manager(webview);
    if (manager == NULL) {
        LOG_ERROR("Could not get the WPE WebKit user content manager.\n");
        return;
    }

    g_signal_connect(webview, "load-changed", G_CALLBACK(on_web_load_changed), player);
    g_signal_connect(webview, "load-failed", G_CALLBACK(on_web_load_failed), player);
    g_signal_connect(
        manager,
        "script-message-with-reply-received::telosnex",
        G_CALLBACK(on_web_handler_call),
        player
    );
    g_signal_connect(
        manager,
        "script-message-received::telosnexEvents",
        G_CALLBACK(on_web_javascript_message),
        player
    );

    if (!webkit_user_content_manager_register_script_message_handler_with_reply(manager, "telosnex", NULL) ||
        !webkit_user_content_manager_register_script_message_handler(manager, "telosnexEvents", NULL)) {
        LOG_ERROR("Could not register the WPE WebKit JavaScript bridge.\n");
        return;
    }

    script = webkit_user_script_new(
        kWebBridgeScript,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL,
        NULL
    );
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
}

static gboolean complete_web_reply(void *userdata) {
    struct web_reply_task *task = userdata;
    JSCValue *value;

    if (task->error != NULL) {
        webkit_script_message_reply_return_error_message(task->pending->reply, task->error);
    } else {
        value = jsc_value_new_from_json(
            task->pending->context,
            task->result_json == NULL ? "null" : task->result_json
        );
        if (value == NULL) value = jsc_value_new_null(task->pending->context);
        webkit_script_message_reply_return_value(task->pending->reply, value);
        g_object_unref(value);
    }

    webkit_script_message_reply_unref(task->pending->reply);
    g_object_unref(task->pending->context);
    g_main_context_unref(task->pending->main_context);
    free(task->pending);
    free(task->result_json);
    free(task->error);
    free(task);
    return G_SOURCE_REMOVE;
}

#define MAX_N_PLANES 4
#define MAX_N_EGL_DMABUF_IMAGE_ATTRIBUTES 6 + 6 * MAX_N_PLANES + 1

UNUSED static inline void lock(struct gstplayer *player) {
    pthread_mutex_lock(&player->lock);
}

UNUSED static inline void unlock(struct gstplayer *player) {
    pthread_mutex_unlock(&player->lock);
}

UNUSED static inline void trace_instant(struct gstplayer *player, const char *name) {
    return flutterpi_trace_event_instant(player->flutterpi, name);
}

UNUSED static inline void trace_begin(struct gstplayer *player, const char *name) {
    return flutterpi_trace_event_begin(player->flutterpi, name);
}

UNUSED static inline void trace_end(struct gstplayer *player, const char *name) {
    return flutterpi_trace_event_end(player->flutterpi, name);
}

static int maybe_send_info(struct gstplayer *player) {
    struct video_info *duped;

    if (player->info.has_resolution && player->info.has_fps && player->info.has_duration && player->info.has_seeking_info) {
        // we didn't send the info yet but we have complete video info now.
        // send it!
        duped = memdup(&(player->info.info), sizeof(player->info.info));
        if (duped == NULL) {
            return ENOMEM;
        }

        notifier_notify(&player->video_info_notifier, duped);
    }
    return 0;
}

static void fetch_duration(struct gstplayer *player) {
    gboolean ok;
    int64_t duration;

    ok = gst_element_query_duration(player->pipeline, GST_FORMAT_TIME, &duration);
    if (ok == FALSE) {
        if (player->is_live) {
            player->info.info.duration_ms = INT64_MAX;
            player->info.has_duration = true;
            return;
        } else {
            LOG_ERROR("Could not fetch duration. (gst_element_query_duration)\n");
            return;
        }
    }

    player->info.info.duration_ms = GST_TIME_AS_MSECONDS(duration);
    player->info.has_duration = true;
}

static void fetch_seeking(struct gstplayer *player) {
    GstQuery *seeking_query;
    gboolean ok, seekable;
    int64_t seek_begin, seek_end;

    seeking_query = gst_query_new_seeking(GST_FORMAT_TIME);
    ok = gst_element_query(player->pipeline, seeking_query);
    if (ok == FALSE) {
        if (player->is_live) {
            player->info.info.can_seek = false;
            player->info.info.seek_begin_ms = 0;
            player->info.info.seek_end_ms = 0;
            player->info.has_seeking_info = true;
            return;
        } else {
            LOG_DEBUG("Could not query seeking info. (gst_element_query)\n");
            return;
        }
    }

    gst_query_parse_seeking(seeking_query, NULL, &seekable, &seek_begin, &seek_end);

    gst_query_unref(seeking_query);

    player->info.info.can_seek = seekable;
    player->info.info.seek_begin_ms = GST_TIME_AS_MSECONDS(seek_begin);
    player->info.info.seek_end_ms = GST_TIME_AS_MSECONDS(seek_end);
    player->info.has_seeking_info = true;
}

static void update_buffering_state(struct gstplayer *player) {
    struct buffering_state *state;
    GstBufferingMode mode;
    GstQuery *query;
    gboolean ok, busy;
    int64_t start, stop, buffering_left;
    int n_ranges, percent, avg_in, avg_out;

    query = gst_query_new_buffering(GST_FORMAT_TIME);
    ok = gst_element_query(player->pipeline, query);
    if (ok == FALSE) {
        LOG_ERROR("Could not query buffering state. (gst_element_query)\n");
        goto fail_unref_query;
    }

    gst_query_parse_buffering_percent(query, &busy, &percent);
    gst_query_parse_buffering_stats(query, &mode, &avg_in, &avg_out, &buffering_left);

    n_ranges = (int) gst_query_get_n_buffering_ranges(query);

    state = malloc(sizeof(*state) + n_ranges * sizeof(struct buffering_range));
    if (state == NULL) {
        goto fail_unref_query;
    }

    for (int i = 0; i < n_ranges; i++) {
        ok = gst_query_parse_nth_buffering_range(query, (unsigned int) i, &start, &stop);
        if (ok == FALSE) {
            LOG_ERROR("Could not parse %dth buffering range from buffering state. (gst_query_parse_nth_buffering_range)\n", i);
            goto fail_free_state;
        }

        state->ranges[i].start_ms = GST_TIME_AS_MSECONDS(start);
        state->ranges[i].stop_ms = GST_TIME_AS_MSECONDS(stop);
    }

    gst_query_unref(query);

    state->percent = percent;
    state->mode =
        (mode == GST_BUFFERING_STREAM    ? BUFFERING_MODE_STREAM :
         mode == GST_BUFFERING_DOWNLOAD  ? BUFFERING_MODE_DOWNLOAD :
         mode == GST_BUFFERING_TIMESHIFT ? BUFFERING_MODE_TIMESHIFT :
         mode == GST_BUFFERING_LIVE      ? BUFFERING_MODE_LIVE :
                                           (assert(0), BUFFERING_MODE_STREAM));
    state->avg_in = avg_in;
    state->avg_out = avg_out;
    state->time_left_ms = buffering_left;
    state->n_ranges = n_ranges;

    notifier_notify(&player->buffering_state_notifier, state);
    return;

fail_free_state:
    free(state);

fail_unref_query:
    gst_query_unref(query);
}

static int init(struct gstplayer *player, bool force_sw_decoders);

static void maybe_deinit(struct gstplayer *player);

static int apply_playback_state(struct gstplayer *player) {
    GstStateChangeReturn ok;
    GstState desired_state, current_state, pending_state;
    double desired_rate;
    int64_t position;

    desired_state = player->playpause_state == kPlaying ? GST_STATE_PLAYING : GST_STATE_PAUSED; /* use GST_STATE_PAUSED if we're stepping */

    /// Use 1.0 if we're stepping, otherwise use the stored playback rate for the current direction.
    if (player->playpause_state == kStepping) {
        desired_rate = player->direction == kForward ? 1.0 : -1.0;
    } else {
        desired_rate = player->direction == kForward ? player->playback_rate_forward : player->playback_rate_backward;
    }

    if (player->current_playback_rate != desired_rate || player->has_desired_position) {
        if (player->has_desired_position) {
            position = player->desired_position_ms * GST_MSECOND;
        } else {
            ok = gst_element_query_position(GST_ELEMENT(player->pipeline), GST_FORMAT_TIME, &position);
            if (ok == FALSE) {
                LOG_ERROR("Could not get the current playback position to apply the playback speed.\n");
                return EIO;
            }
        }

        if (player->direction == kForward) {
            LOG_DEBUG(
                "gst_element_seek(..., rate: %f, start: %" GST_TIME_FORMAT ", end: %" GST_TIME_FORMAT ", ...)\n",
                desired_rate,
                GST_TIME_ARGS(position),
                GST_TIME_ARGS(GST_CLOCK_TIME_NONE)
            );
            ok = gst_element_seek(
                GST_ELEMENT(player->pipeline),
                desired_rate,
                GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH |
                    (player->do_fast_seeking ? GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST : GST_SEEK_FLAG_ACCURATE),
                GST_SEEK_TYPE_SET,
                position,
                GST_SEEK_TYPE_SET,
                GST_CLOCK_TIME_NONE
            );
            if (ok == FALSE) {
                LOG_ERROR(
                    "Could not set the new playback speed / playback position (speed: %f, pos: %" GST_TIME_FORMAT ").\n",
                    desired_rate,
                    GST_TIME_ARGS(position)
                );
                return EIO;
            }
        } else {
            LOG_DEBUG(
                "gst_element_seek(..., rate: %f, start: %" GST_TIME_FORMAT ", end: %" GST_TIME_FORMAT ", ...)\n",
                desired_rate,
                GST_TIME_ARGS(0),
                GST_TIME_ARGS(position)
            );
            ok = gst_element_seek(
                GST_ELEMENT(player->pipeline),
                desired_rate,
                GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH |
                    (player->do_fast_seeking ? GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST : GST_SEEK_FLAG_ACCURATE),
                GST_SEEK_TYPE_SET,
                0,
                GST_SEEK_TYPE_SET,
                position
            );

            if (ok == FALSE) {
                LOG_ERROR(
                    "Could not set the new playback speed / playback position (speed: %f, pos: %" GST_TIME_FORMAT ").\n",
                    desired_rate,
                    GST_TIME_ARGS(position)
                );
                return EIO;
            }
        }

        player->current_playback_rate = desired_rate;
        player->fallback_position_ms = GST_TIME_AS_MSECONDS(position);
        player->has_desired_position = false;
    }

    DEBUG_TRACE_BEGIN(player, "gst_element_get_state");
    ok = gst_element_get_state(player->pipeline, &current_state, &pending_state, 0);
    DEBUG_TRACE_END(player, "gst_element_get_state");

    if (ok == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR(
            "last gstreamer pipeline state change failed. gst_element_get_state(element name: %s): GST_STATE_CHANGE_FAILURE\n",
            GST_ELEMENT_NAME(player->pipeline)
        );
        DEBUG_TRACE_END(player, "apply_playback_state");
        return EIO;
    }

    if (pending_state == GST_STATE_VOID_PENDING) {
        if (current_state == desired_state) {
            // we're already in the desired state, and we're also not changing it
            // no need to do anything.
            LOG_DEBUG(
                "apply_playback_state(playing: %s): already in desired state and none pending\n",
                PLAYPAUSE_STATE_AS_STRING(player->playpause_state)
            );
            DEBUG_TRACE_END(player, "apply_playback_state");
            return 0;
        }

        LOG_DEBUG(
            "apply_playback_state(playing: %s): setting state to %s\n",
            PLAYPAUSE_STATE_AS_STRING(player->playpause_state),
            gst_element_state_get_name(desired_state)
        );

        DEBUG_TRACE_BEGIN(player, "gst_element_set_state");
        ok = gst_element_set_state(player->pipeline, desired_state);
        DEBUG_TRACE_END(player, "gst_element_set_state");

        if (ok == GST_STATE_CHANGE_FAILURE) {
            LOG_GST_SET_STATE_ERROR(player->pipeline);
            DEBUG_TRACE_END(player, "apply_playback_state");
            return EIO;
        }
    } else if (pending_state != desired_state) {
        // queue to be executed when pending async state change completes
        /// TODO: Implement properly

        LOG_DEBUG(
            "apply_playback_state(playing: %s): async state change in progress, setting state to %s\n",
            PLAYPAUSE_STATE_AS_STRING(player->playpause_state),
            gst_element_state_get_name(desired_state)
        );

        DEBUG_TRACE_BEGIN(player, "gst_element_set_state");
        ok = gst_element_set_state(player->pipeline, desired_state);
        DEBUG_TRACE_END(player, "gst_element_set_state");

        if (ok == GST_STATE_CHANGE_FAILURE) {
            LOG_GST_SET_STATE_ERROR(player->pipeline);
            DEBUG_TRACE_END(player, "apply_playback_state");
            return EIO;
        }
    }

    DEBUG_TRACE_END(player, "apply_playback_state");
    return 0;
}

static void on_bus_message(struct gstplayer *player, GstMessage *msg) {
    GstState old, current, pending, requested;
    GError *error;
    gchar *debug_info;

    DEBUG_TRACE_BEGIN(player, "on_bus_message");
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(msg, &error, &debug_info);

            LOG_ERROR(
                "gstreamer error: code: %d, domain: %s, msg: %s (debug info: %s)\n",
                error->code,
                g_quark_to_string(error->domain),
                error->message,
                debug_info
            );
            g_clear_error(&error);
            g_free(debug_info);
            break;

        case GST_MESSAGE_WARNING:
            gst_message_parse_warning(msg, &error, &debug_info);
            LOG_ERROR("gstreamer warning: %s (debug info: %s)\n", error->message, debug_info);
            g_clear_error(&error);
            g_free(debug_info);
            break;

        case GST_MESSAGE_INFO:
            gst_message_parse_info(msg, &error, &debug_info);
            LOG_DEBUG("gstreamer info: %s (debug info: %s)\n", error->message, debug_info);
            g_clear_error(&error);
            g_free(debug_info);
            break;

        case GST_MESSAGE_BUFFERING: {
            GstBufferingMode mode;
            int64_t buffering_left;
            int percent, avg_in, avg_out;

            gst_message_parse_buffering(msg, &percent);
            gst_message_parse_buffering_stats(msg, &mode, &avg_in, &avg_out, &buffering_left);

            LOG_DEBUG(
                "buffering, src: %s, percent: %d, mode: %s, avg in: %d B/s, avg out: %d B/s, %" GST_TIME_FORMAT "\n",
                GST_MESSAGE_SRC_NAME(msg),
                percent,
                mode == GST_BUFFERING_STREAM    ? "stream" :
                mode == GST_BUFFERING_DOWNLOAD  ? "download" :
                mode == GST_BUFFERING_TIMESHIFT ? "timeshift" :
                mode == GST_BUFFERING_LIVE      ? "live" :
                                                  "?",
                avg_in,
                avg_out,
                GST_TIME_ARGS(buffering_left * GST_MSECOND)
            );

            /// TODO: GST_MESSAGE_BUFFERING is only emitted when we actually need to wait on some buffering till we can resume the playback.
            /// However, the info we send to the callback also contains information on the buffered video ranges.
            /// That information is constantly changing, but we only notify the player about it when we actively wait for the buffer to be filled.
            DEBUG_TRACE_BEGIN(player, "update_buffering_state");
            update_buffering_state(player);
            DEBUG_TRACE_END(player, "update_buffering_state");

            break;
        };

        case GST_MESSAGE_STATE_CHANGED:
            gst_message_parse_state_changed(msg, &old, &current, &pending);
            LOG_DEBUG(
                "state-changed: src: %s, old: %s, current: %s, pending: %s\n",
                GST_MESSAGE_SRC_NAME(msg),
                gst_element_state_get_name(old),
                gst_element_state_get_name(current),
                gst_element_state_get_name(pending)
            );

            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(player->pipeline)) {
                if (!player->info.has_duration && (current == GST_STATE_PAUSED || current == GST_STATE_PLAYING)) {
                    // it's our pipeline that changed to either playing / paused, and we don't have info about our video duration yet.
                    // get that info now.
                    // technically we can already fetch the duration when the decodebin changed to PAUSED state.
                    DEBUG_TRACE_BEGIN(player, "fetch video info");
                    fetch_duration(player);
                    fetch_seeking(player);
                    maybe_send_info(player);
                    DEBUG_TRACE_END(player, "fetch video info");
                }
            }
            break;

        case GST_MESSAGE_ASYNC_DONE: break;

        case GST_MESSAGE_LATENCY:
            LOG_DEBUG("gstreamer: redistributing latency\n");
            DEBUG_TRACE_BEGIN(player, "gst_bin_recalculate_latency");
            gst_bin_recalculate_latency(GST_BIN(player->pipeline));
            DEBUG_TRACE_END(player, "gst_bin_recalculate_latency");
            break;

        case GST_MESSAGE_EOS: LOG_DEBUG("end of stream, src: %s\n", GST_MESSAGE_SRC_NAME(msg)); break;

        case GST_MESSAGE_REQUEST_STATE:
            gst_message_parse_request_state(msg, &requested);
            LOG_DEBUG(
                "gstreamer state change to %s was requested by %s\n",
                gst_element_state_get_name(requested),
                GST_MESSAGE_SRC_NAME(msg)
            );
            DEBUG_TRACE_BEGIN(player, "gst_element_set_state");
            gst_element_set_state(GST_ELEMENT(player->pipeline), requested);
            DEBUG_TRACE_END(player, "gst_element_set_state");
            break;

        case GST_MESSAGE_APPLICATION:
            if (player->looping && gst_message_has_name(msg, "appsink-eos")) {
                // we have an appsink end of stream event
                // and we should be looping, so seek back to start
                LOG_DEBUG("appsink eos, seeking back to segment start (flushing)\n");
                DEBUG_TRACE_BEGIN(player, "gst_element_seek");
                gst_element_seek(
                    GST_ELEMENT(player->pipeline),
                    player->current_playback_rate,
                    GST_FORMAT_TIME,
                    GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                    GST_SEEK_TYPE_SET,
                    0,
                    GST_SEEK_TYPE_SET,
                    GST_CLOCK_TIME_NONE
                );
                DEBUG_TRACE_END(player, "gst_element_seek");

                apply_playback_state(player);
            }
            break;

        default: LOG_DEBUG("gstreamer message: %s, src: %s\n", GST_MESSAGE_TYPE_NAME(msg), GST_MESSAGE_SRC_NAME(msg)); break;
    }
    DEBUG_TRACE_END(player, "on_bus_message");
    return;
}

static int on_bus_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
    struct gstplayer *player;
    GstMessage *msg;

    (void) s;
    (void) fd;
    (void) revents;

    player = userdata;

    DEBUG_TRACE_BEGIN(player, "on_bus_fd_ready");

    msg = gst_bus_pop(player->bus);
    if (msg != NULL) {
        on_bus_message(player, msg);
        gst_message_unref(msg);
    }

    DEBUG_TRACE_END(player, "on_bus_fd_ready");

    return 0;
}

static GstPadProbeReturn on_query_appsink(GstPad *pad, GstPadProbeInfo *info, void *userdata) {
    GstQuery *query;

    (void) pad;
    (void) userdata;

    query = gst_pad_probe_info_get_query(info);
    if (query == NULL) {
        LOG_DEBUG("Couldn't get query from pad probe info.\n");
        return GST_PAD_PROBE_OK;
    }

    if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) {
        return GST_PAD_PROBE_OK;
    }

    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

    return GST_PAD_PROBE_HANDLED;
}

static void on_element_added(GstBin *bin, GstElement *element, void *userdata) {
    GstElementFactory *factory;
    const char *factory_name;

    (void) userdata;
    (void) bin;

    factory = gst_element_get_factory(element);
    factory_name = gst_plugin_feature_get_name(factory);

    if (g_str_has_prefix(factory_name, "v4l2video") && g_str_has_suffix(factory_name, "dec")) {
        gst_util_set_object_arg(G_OBJECT(element), "capture-io-mode", "dmabuf");
        fprintf(stderr, "[gstreamer video player] found gstreamer V4L2 video decoder element with name \"%s\"\n", GST_OBJECT_NAME(element));
    }
}

static GstPadProbeReturn on_probe_pad(GstPad *pad, GstPadProbeInfo *info, void *userdata) {
    struct gstplayer *player;
    GstEvent *event;
    GstCaps *caps;
    gboolean ok;

    (void) pad;

    player = userdata;
    event = GST_PAD_PROBE_INFO_EVENT(info);

    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
        return GST_PAD_PROBE_OK;
    }

    gst_event_parse_caps(event, &caps);
    if (caps == NULL) {
        LOG_ERROR("gstreamer: caps event without caps\n");
        return GST_PAD_PROBE_OK;
    }

    ok = gst_video_info_from_caps(&player->gst_info, caps);
    if (!ok) {
        LOG_ERROR("gstreamer: caps event with invalid video caps\n");
        return GST_PAD_PROBE_OK;
    }

    player->has_gst_info = true;

    LOG_DEBUG(
        "on_probe_pad, fps: %f, res: % 4d x % 4d, format: %s\n",
        (double) GST_VIDEO_INFO_FPS_N(&player->gst_info) / GST_VIDEO_INFO_FPS_D(&player->gst_info),
        GST_VIDEO_INFO_WIDTH(&player->gst_info),
        GST_VIDEO_INFO_HEIGHT(&player->gst_info),
        gst_video_format_to_string(player->gst_info.finfo->format)
    );

    player->info.info.width = GST_VIDEO_INFO_WIDTH(&player->gst_info);
    player->info.info.height = GST_VIDEO_INFO_HEIGHT(&player->gst_info);
    player->info.info.fps = (double) GST_VIDEO_INFO_FPS_N(&player->gst_info) / GST_VIDEO_INFO_FPS_D(&player->gst_info);
    player->info.has_resolution = true;
    player->info.has_fps = true;
    maybe_send_info(player);

    return GST_PAD_PROBE_OK;
}

static void on_destroy_texture_frame(const struct texture_frame *texture_frame, void *userdata) {
    struct video_frame *frame;

    (void) texture_frame;

    ASSERT_NOT_NULL(texture_frame);
    ASSERT_NOT_NULL(userdata);

    frame = userdata;

    frame_destroy(frame);
}

static void on_appsink_eos(GstAppSink *appsink, void *userdata) {
    gboolean ok;

    ASSERT_NOT_NULL(appsink);
    ASSERT_NOT_NULL(userdata);

    (void) userdata;

    LOG_DEBUG("on_appsink_eos()\n");

    // this method is called from the streaming thread.
    // we shouldn't access the player directly here, it could change while we use it.
    // post a message to the gstreamer bus instead, will be handled by
    // @ref on_bus_message.
    ok = gst_element_post_message(
        GST_ELEMENT(appsink),
        gst_message_new_application(GST_OBJECT(appsink), gst_structure_new_empty("appsink-eos"))
    );
    if (ok == FALSE) {
        LOG_ERROR("Could not post appsink end-of-stream event to the message bus.\n");
    }
}

static GstFlowReturn on_appsink_new_preroll(GstAppSink *appsink, void *userdata) {
    struct video_frame *frame;
    struct gstplayer *player;
    GstSample *sample;

    ASSERT_NOT_NULL(appsink);
    ASSERT_NOT_NULL(userdata);

    player = userdata;

    // Plane path: nothing to preroll, and frame_interface may be NULL.
    if (((struct gstplayer *) userdata)->dmabuf_surface != NULL) {
        return GST_FLOW_OK;
    }

    sample = gst_app_sink_try_pull_preroll(appsink, 0);
    if (sample == NULL) {
        return GST_FLOW_FLUSHING;
    }

    /// TODO: Attempt to upload using gst_gl_upload here
    frame = frame_new(player->frame_interface, sample, player->has_gst_info ? &player->gst_info : NULL);

    gst_sample_unref(sample);

    if (frame != NULL) {
        texture_push_frame(
            player->texture,
            &(struct texture_frame){
                .gl = *frame_get_gl_frame(frame),
                .destroy = on_destroy_texture_frame,
                .userdata = frame,
            }
        );
    }

    return GST_FLOW_OK;
}

static GstFlowReturn on_appsink_new_sample(GstAppSink *appsink, void *userdata) {
    struct video_frame *frame;
    struct gstplayer *player;
    GstSample *sample;

    ASSERT_NOT_NULL(appsink);
    ASSERT_NOT_NULL(userdata);

    player = userdata;

    /// TODO: Attempt to upload using gst_gl_upload here
    sample = gst_app_sink_try_pull_sample(appsink, 0);
    if (sample == NULL) {
        return GST_FLOW_FLUSHING;
    }

    if (player->dmabuf_surface != NULL) {
        struct dmabuf dmabuf;
        int ok;

        ok = frame_dup_sample_as_scanout_dmabuf(flutterpi_get_gbm_device(player->flutterpi), sample, &dmabuf);
        if (ok == 0) {
            ok = dmabuf_surface_push_dmabuf(player->dmabuf_surface, &dmabuf, on_release_plane_dmabuf);
            if (ok != 0) {
                close(dmabuf.fds[0]);
            }
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }


    frame = frame_new(player->frame_interface, sample, player->has_gst_info ? &player->gst_info : NULL);

    gst_sample_unref(sample);

    if (frame != NULL) {
        texture_push_frame(
            player->texture,
            &(struct texture_frame){
                .gl = *frame_get_gl_frame(frame),
                .destroy = on_destroy_texture_frame,
                .userdata = frame,
            }
        );
    }

    return GST_FLOW_OK;
}

static void on_appsink_cbs_destroy(void *userdata) {
    struct gstplayer *player;

    LOG_DEBUG("on_appsink_cbs_destroy()\n");
    ASSERT_NOT_NULL(userdata);

    player = userdata;

    (void) player;
}

void on_source_setup(GstElement *bin, GstElement *source, gpointer userdata) {
    (void) bin;

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "extra-headers") != NULL) {
        g_object_set(source, "extra-headers", (GstStructure *) userdata, NULL);
    } else {
        LOG_ERROR("Failed to set custom HTTP headers because gstreamer source element has no 'extra-headers' property.\n");
    }
}

static int init(struct gstplayer *player, bool force_sw_decoders) {
    GstStateChangeReturn state_change_return;
    sd_event_source *busfd_event_source;
    GstElement *pipeline, *sink, *src, *websrc;
    GstBus *bus;
    GstPad *pad;
    GPollFD fd;
    GError *error = NULL;
    int ok;

    static const char *default_pipeline_descr = "uridecodebin name=\"src\" ! video/x-raw ! appsink sync=true name=\"sink\"";

    const char *pipeline_descr;
    if (player->pipeline_description != NULL) {
        pipeline_descr = player->pipeline_description;
    } else {
        pipeline_descr = default_pipeline_descr;
    }

    pipeline = gst_parse_launch(pipeline_descr, &error);
    if (pipeline == NULL) {
        LOG_ERROR("Could create GStreamer pipeline from description: %s (pipeline: `%s`)\n", error->message, pipeline_descr);
        return error->code;
    }

    sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (sink == NULL) {
        LOG_ERROR("Couldn't find appsink in pipeline bin.\n");
        ok = EINVAL;
        goto fail_unref_pipeline;
    }

    pad = gst_element_get_static_pad(sink, "sink");
    if (pad == NULL) {
        LOG_ERROR("Couldn't get static pad \"sink\" from video sink.\n");
        ok = EINVAL;
        goto fail_unref_sink;
    }

    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, on_query_appsink, player, NULL);

    src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    websrc = gst_bin_get_by_name(GST_BIN(pipeline), "websrc");
    if (websrc != NULL) {
        if (g_signal_lookup("configure-web-view", G_OBJECT_TYPE(websrc)) == 0) {
            LOG_ERROR("The element named \"websrc\" is not a configurable WPE video source.\n");
            gst_object_unref(websrc);
            websrc = NULL;
        } else {
            g_signal_connect(websrc, "configure-web-view", G_CALLBACK(on_configure_web_view), player);
        }
    }

    if (player->video_uri != NULL) {
        if (src != NULL) {
            g_object_set(G_OBJECT(src), "uri", player->video_uri, NULL);
        } else {
            LOG_ERROR("Couldn't find \"src\" element to configure Video URI.\n");
        }
    }

    if (force_sw_decoders) {
        if (src != NULL) {
            g_object_set(G_OBJECT(src), "force-sw-decoders", force_sw_decoders, NULL);
        } else {
            LOG_ERROR("Couldn't find \"src\" element to force sw decoding.\n");
        }
    }

    if (player->headers != NULL && gst_structure_n_fields(player->headers) > 0) {
        if (src != NULL && g_signal_lookup("source-setup", G_OBJECT_TYPE(src)) != 0) {
            g_signal_connect(G_OBJECT(src), "source-setup", G_CALLBACK(on_source_setup), player->headers);
        } else {
            LOG_ERROR("Couldn't configure HTTP headers on the GStreamer source element.\n");
        }
    }

    if (player->pipeline_description == NULL) {
        gst_base_sink_set_max_lateness(GST_BASE_SINK(sink), 20 * GST_MSECOND);
        gst_base_sink_set_qos_enabled(GST_BASE_SINK(sink), TRUE);
        gst_base_sink_set_sync(GST_BASE_SINK(sink), TRUE);
        gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 2);
        gst_app_sink_set_drop(GST_APP_SINK(sink), FALSE);
    }
    // Custom pipelines own their sink timing and backpressure policy.
    gst_app_sink_set_emit_signals(GST_APP_SINK(sink), TRUE);

    // configure our caps
    // we only accept video formats that we can actually upload to EGL
    GstCaps *caps = gst_caps_new_empty();
    if (player->frame_interface == NULL) {
        // Plane path: no EGL import, frames are copied into an ARGB8888
        // scanout BO, so accept the packed 32-bit formats WPE produces.
        gst_caps_append(caps, gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", NULL));
        gst_caps_append(caps, gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRx", NULL));
    } else {
        for_each_format_in_frame_interface(i, format, player->frame_interface) {
            GstVideoFormat gst_format = gst_video_format_from_drm_format(format->format);
            if (gst_format == GST_VIDEO_FORMAT_UNKNOWN) {
                continue;
            }

            gst_caps_append(caps, gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, gst_video_format_to_string(gst_format), NULL));
        }
    }
    gst_app_sink_set_caps(GST_APP_SINK(sink), caps);
    gst_caps_unref(caps);

    gst_app_sink_set_callbacks(
        GST_APP_SINK(sink),
        &(GstAppSinkCallbacks
        ){ .eos = on_appsink_eos, .new_preroll = on_appsink_new_preroll, .new_sample = on_appsink_new_sample, ._gst_reserved = { 0 } },
        player,
        on_appsink_cbs_destroy
    );

    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, on_probe_pad, player, NULL);

    /// FIXME: Make this work for custom pipelines as well.
    if (src != NULL) {
        g_signal_connect(src, "element-added", G_CALLBACK(on_element_added), player);
    } else {
        LOG_DEBUG("Couldn't find \"src\" element to setup v4l2 'capture-io-mode' to 'dmabuf'.\n");
    }

    if (src != NULL) {
        gst_object_unref(src);
        src = NULL;
    }

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));

    gst_bus_get_pollfd(bus, &fd);

    flutterpi_sd_event_add_io(&busfd_event_source, fd.fd, EPOLLIN, on_bus_fd_ready, player);

    LOG_DEBUG("Setting state to paused...\n");
    state_change_return = gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PAUSED);
    if (state_change_return == GST_STATE_CHANGE_NO_PREROLL) {
        LOG_DEBUG("Is Live!\n");
        player->is_live = true;
    } else {
        LOG_DEBUG("Not live!\n");
        player->is_live = false;
    }

    player->sink = sink;
    player->websrc = websrc;
    /// FIXME: Not sure we need this here. pipeline is floating after gst_parse_launch, which
    /// means we should take a reference, but the examples don't increase the refcount.
    player->pipeline = pipeline;  //gst_object_ref(pipeline);
    player->bus = bus;
    player->busfd_events = busfd_event_source;
    if (websrc != NULL) {
        player->web_thread = g_thread_new("flutterpi-wpe", run_web_tasks, player);
    }

    gst_object_unref(pad);
    return 0;

fail_unref_sink:
    gst_object_unref(sink);

fail_unref_pipeline:
    gst_object_unref(pipeline);

    return ok;
}

static void maybe_deinit(struct gstplayer *player) {
    if (player->web_thread != NULL) {
        struct web_task *stop = g_new0(struct web_task, 1);
        stop->type = kWebTaskStop;
        g_async_queue_push(player->web_tasks, stop);
        g_thread_join(player->web_thread);
        player->web_thread = NULL;
    }
    if (player->busfd_events != NULL) {
        sd_event_source_unrefp(&player->busfd_events);
    }
    if (player->sink != NULL) {
        gst_object_unref(GST_OBJECT(player->sink));
        player->sink = NULL;
    }
    if (player->bus != NULL) {
        gst_object_unref(GST_OBJECT(player->bus));
        player->bus = NULL;
    }
    if (player->pipeline != NULL) {
        gst_element_set_state(GST_ELEMENT(player->pipeline), GST_STATE_READY);
        gst_element_set_state(GST_ELEMENT(player->pipeline), GST_STATE_NULL);
        gst_object_unref(GST_OBJECT(player->pipeline));
        player->pipeline = NULL;
    }
    if (player->websrc != NULL) {
        gst_object_unref(GST_OBJECT(player->websrc));
        player->websrc = NULL;
    }
}

DEFINE_LOCK_OPS(gstplayer, lock)

static struct gstplayer *gstplayer_new(struct flutterpi *flutterpi, const char *uri, const char *pipeline_descr, void *userdata) {
    struct frame_interface *frame_interface;
    struct gstplayer *player;
    struct texture *texture;
    GstStructure *gst_headers;
    int64_t texture_id;
    char *uri_owned, *pipeline_descr_owned;
    int ok;

    ASSERT_NOT_NULL(flutterpi);
    assert((uri != NULL) != (pipeline_descr != NULL));

    player = malloc(sizeof *player);
    if (player == NULL)
        return NULL;

    texture = flutterpi_create_texture(flutterpi);
    if (texture == NULL)
        goto fail_free_player;

    // The plane path scans dmabufs out via KMS and never touches GL, so a missing
    // frame interface is only fatal for the texture path. Under Vulkan there is no
    // gl_renderer at all, and bailing here is what made the webview disappear.
    frame_interface = frame_interface_new(flutterpi_get_gl_renderer(flutterpi));
    if (frame_interface == NULL && getenv("FLUTTERPI_WEBVIEW_ON_PLANE") == NULL)
        goto fail_destroy_texture;

    texture_id = texture_get_id(texture);

    if (uri != NULL) {
        uri_owned = strdup(uri);
        if (uri_owned == NULL)
            goto fail_destroy_frame_interface;
    } else {
        uri_owned = NULL;
    }

    if (pipeline_descr != NULL) {
        pipeline_descr_owned = strdup(pipeline_descr);
        if (pipeline_descr_owned == NULL)
            goto fail_destroy_frame_interface;
    } else {
        pipeline_descr_owned = NULL;
    }

    gst_headers = gst_structure_new_empty("http-headers");

    ok = pthread_mutex_init(&player->lock, NULL);
    if (ok != 0)
        goto fail_free_gst_headers;

    ok = value_notifier_init(&player->video_info_notifier, NULL, free /* free(NULL) is a no-op, I checked */);
    if (ok != 0)
        goto fail_destroy_mutex;

    ok = value_notifier_init(&player->buffering_state_notifier, NULL, free);
    if (ok != 0)
        goto fail_deinit_video_info_notifier;

    ok = change_notifier_init(&player->error_notifier);
    if (ok != 0)
        goto fail_deinit_buffering_state_notifier;

    ok = value_notifier_init(&player->web_event_notifier, NULL, destroy_web_event);
    if (ok != 0)
        goto fail_deinit_error_notifier;

    g_mutex_init(&player->web_lock);
    player->pending_web_replies = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, destroy_pending_web_reply);
    if (player->pending_web_replies == NULL)
        goto fail_deinit_web_resources;
    player->web_tasks = g_async_queue_new_full(destroy_web_task);
    if (player->web_tasks == NULL)
        goto fail_unref_pending_web_replies;

    player->flutterpi = flutterpi;
    player->userdata = userdata;
    player->video_uri = uri_owned;
    player->pipeline_description = pipeline_descr_owned;
    player->headers = gst_headers;
    player->playback_rate_forward = 1.0;
    player->playback_rate_backward = 1.0;
    player->looping = false;
    player->playpause_state = kPaused;
    player->direction = kForward;
    player->current_playback_rate = 1.0;
    player->fallback_position_ms = 0;
    player->has_desired_position = false;
    player->desired_position_ms = 0;
    player->has_sent_info = false;
    player->info.has_resolution = false;
    player->info.has_fps = false;
    player->info.has_duration = false;
    player->info.has_seeking_info = false;
    player->has_gst_info = false;
    memset(&player->gst_info, 0, sizeof(player->gst_info));
    player->texture = texture;
    player->texture_id = texture_id;
    player->frame_interface = frame_interface;
    player->pipeline = NULL;
    player->sink = NULL;
    player->websrc = NULL;
    player->bus = NULL;
    player->web_thread = NULL;
    player->web_move_pending = false;
    player->next_web_call_id = 1;
    player->busfd_events = NULL;
    player->is_live = false;
    // Opt-in: present frames on a DRM overlay plane instead of uploading them as a
    // texture. Renderer-agnostic, so this is what survives under Vulkan.
    player->dmabuf_surface = NULL;
    player->platform_view_id = 0;
    if (getenv("FLUTTERPI_WEBVIEW_ON_PLANE") != NULL) {
        player->dmabuf_surface = dmabuf_surface_new(
            flutterpi_get_tracer(player->flutterpi),
            flutterpi_get_texture_registry(player->flutterpi)
        );
        if (player->dmabuf_surface != NULL) {
            // The compositor casts this id straight back to a surface pointer in
        // release builds (compositor_ng.c), so it MUST be the surface pointer.
        player->platform_view_id = (int64_t) (intptr_t) CAST_SURFACE_UNCHECKED(player->dmabuf_surface);
            compositor_set_platform_view(
                flutterpi_get_compositor(player->flutterpi),
                player->platform_view_id,
                CAST_SURFACE_UNCHECKED(player->dmabuf_surface)
            );
        }
    }

    return player;

fail_unref_pending_web_replies:
    g_hash_table_unref(player->pending_web_replies);

fail_deinit_web_resources:
    g_mutex_clear(&player->web_lock);
    notifier_deinit(&player->web_event_notifier);

fail_deinit_error_notifier:
    notifier_deinit(&player->error_notifier);

fail_deinit_buffering_state_notifier:
    notifier_deinit(&player->buffering_state_notifier);

fail_deinit_video_info_notifier:
    notifier_deinit(&player->video_info_notifier);

fail_destroy_mutex:
    pthread_mutex_destroy(&player->lock);

fail_free_gst_headers:
    gst_structure_free(gst_headers);
    free(uri_owned);

fail_destroy_frame_interface:
    if (frame_interface != NULL) {
        frame_interface_unref(frame_interface);
    }

fail_destroy_texture:
    texture_destroy(texture);

fail_free_player:
    free(player);

    return NULL;
}

struct gstplayer *gstplayer_new_from_asset(struct flutterpi *flutterpi, const char *asset_path, const char *package_name, void *userdata) {
    struct gstplayer *player;
    char *uri;
    int ok;

    (void) package_name;

    ok = asprintf(&uri, "file://%s/%s", flutterpi_get_asset_bundle_path(flutterpi), asset_path);
    if (ok < 0) {
        return NULL;
    }

    player = gstplayer_new(flutterpi, uri, NULL, userdata);

    free(uri);

    return player;
}

struct gstplayer *gstplayer_new_from_network(struct flutterpi *flutterpi, const char *uri, enum format_hint format_hint, void *userdata) {
    (void) format_hint;
    return gstplayer_new(flutterpi, uri, NULL, userdata);
}

struct gstplayer *gstplayer_new_from_file(struct flutterpi *flutterpi, const char *uri, void *userdata) {
    return gstplayer_new(flutterpi, uri, NULL, userdata);
}

struct gstplayer *gstplayer_new_from_content_uri(struct flutterpi *flutterpi, const char *uri, void *userdata) {
    return gstplayer_new(flutterpi, uri, NULL, userdata);
}

struct gstplayer *gstplayer_new_from_pipeline(struct flutterpi *flutterpi, const char *pipeline, void *userdata) {
    return gstplayer_new(flutterpi, NULL, pipeline, userdata);
}

void gstplayer_destroy(struct gstplayer *player) {
    LOG_DEBUG("gstplayer_destroy(%p)\n", player);
    maybe_deinit(player);
    notifier_deinit(&player->video_info_notifier);
    notifier_deinit(&player->buffering_state_notifier);
    notifier_deinit(&player->error_notifier);
    notifier_deinit(&player->web_event_notifier);
    g_hash_table_unref(player->pending_web_replies);
    g_async_queue_unref(player->web_tasks);
    g_mutex_clear(&player->web_lock);
    pthread_mutex_destroy(&player->lock);
    if (player->headers != NULL) {
        gst_structure_free(player->headers);
    }
    if (player->video_uri != NULL) {
        free(player->video_uri);
    }
    if (player->pipeline_description != NULL) {
        free(player->pipeline_description);
    }
    if (player->frame_interface != NULL) {
        frame_interface_unref(player->frame_interface);
    }
    texture_destroy(player->texture);
    free(player);
}

int64_t gstplayer_get_texture_id(struct gstplayer *player) {
    return player->texture_id;
}

int64_t gstplayer_get_platform_view_id(struct gstplayer *player) {
    ASSERT_NOT_NULL(player);
    return player->platform_view_id;
}

void gstplayer_put_http_header(struct gstplayer *player, const char *key, const char *value) {
    GValue gvalue = G_VALUE_INIT;
    g_value_set_string(&gvalue, value);
    gst_structure_take_value(player->headers, key, &gvalue);
}

void gstplayer_set_userdata_locked(struct gstplayer *player, void *userdata) {
    player->userdata = userdata;
}

void *gstplayer_get_userdata_locked(struct gstplayer *player) {
    return player->userdata;
}

int gstplayer_initialize(struct gstplayer *player) {
    return init(player, false);
}

int gstplayer_play(struct gstplayer *player) {
    LOG_DEBUG("gstplayer_play()\n");
    player->playpause_state = kPlaying;
    player->direction = kForward;
    return apply_playback_state(player);
}

int gstplayer_pause(struct gstplayer *player) {
    LOG_DEBUG("gstplayer_pause()\n");
    player->playpause_state = kPaused;
    player->direction = kForward;
    return apply_playback_state(player);
}

int gstplayer_set_looping(struct gstplayer *player, bool looping) {
    LOG_DEBUG("gstplayer_set_looping(%s)\n", looping ? "true" : "false");
    player->looping = looping;
    return 0;
}

int gstplayer_set_volume(struct gstplayer *player, double volume) {
    (void) player;
    (void) volume;
    LOG_DEBUG("gstplayer_set_volume(%f)\n", volume);
    /// TODO: Implement
    return 0;
}

int64_t gstplayer_get_position(struct gstplayer *player) {
    GstState current, pending;
    gboolean ok;
    int64_t position;

    GstStateChangeReturn statechange = gst_element_get_state(GST_ELEMENT(player->pipeline), &current, &pending, 0);
    if (statechange == GST_STATE_CHANGE_FAILURE) {
        LOG_GST_GET_STATE_ERROR(player->pipeline);
        return -1;
    }

    if (statechange == GST_STATE_CHANGE_ASYNC) {
        // we don't have position data yet.
        // report the latest known (or the desired) position.
        return player->fallback_position_ms;
    }

    DEBUG_TRACE_BEGIN(player, "gstplayer_get_position");
    DEBUG_TRACE_BEGIN(player, "gst_element_query_position");
    ok = gst_element_query_position(player->pipeline, GST_FORMAT_TIME, &position);
    DEBUG_TRACE_END(player, "gst_element_query_position");

    if (ok == FALSE) {
        LOG_ERROR("Could not query gstreamer position. (gst_element_query_position)\n");
        return 0;
    }

    DEBUG_TRACE_END(player, "gstplayer_get_position");
    return GST_TIME_AS_MSECONDS(position);
}

int gstplayer_seek_to(struct gstplayer *player, int64_t position, bool nearest_keyframe) {
    LOG_DEBUG("gstplayer_seek_to(%" PRId64 ")\n", position);
    player->has_desired_position = true;
    player->desired_position_ms = position;
    player->do_fast_seeking = nearest_keyframe;
    return apply_playback_state(player);
}

int gstplayer_set_playback_speed(struct gstplayer *player, double playback_speed) {
    LOG_DEBUG("gstplayer_set_playback_speed(%f)\n", playback_speed);
    ASSERT_MSG(playback_speed > 0, "playback speed must be > 0.");
    player->playback_rate_forward = playback_speed;
    return apply_playback_state(player);
}

int gstplayer_step_forward(struct gstplayer *player) {
    gboolean gst_ok;
    int ok;

    ASSERT_NOT_NULL(player);

    player->playpause_state = kStepping;
    player->direction = kForward;
    ok = apply_playback_state(player);
    if (ok != 0) {
        return ok;
    }

    gst_ok = gst_element_send_event(player->pipeline, gst_event_new_step(GST_FORMAT_BUFFERS, 1, 1, TRUE, FALSE));
    if (gst_ok == FALSE) {
        LOG_ERROR("Could not send frame-step event to pipeline. (gst_element_send_event)\n");
        return EIO;
    }
    return 0;
}

int gstplayer_send_navigation_event(struct gstplayer *player, GstEvent *event) {
    enum web_pointer_task_kind pointer_kind = kWebPointerTaskNone;
    struct web_task *task;
    int ok;

    ASSERT_NOT_NULL(player);
    ASSERT_NOT_NULL(event);

    switch (gst_navigation_event_get_type(event)) {
        case GST_NAVIGATION_EVENT_MOUSE_MOVE:
            pointer_kind = kWebPointerTaskMove;
            if (atomic_exchange(&player->web_move_pending, true)) {
                gst_event_unref(event);
                return 0;
            }
            break;
        default: break;
    }

    task = calloc(1, sizeof(*task));
    if (task == NULL) {
        if (pointer_kind == kWebPointerTaskMove) player->web_move_pending = false;
        gst_event_unref(event);
        return ENOMEM;
    }
    task->type = kWebTaskNavigation;
    task->pointer_kind = pointer_kind;
    task->navigation_event = event;
    ok = enqueue_web_task(player, task);
    if (ok != 0 && pointer_kind == kWebPointerTaskMove) {
        player->web_move_pending = false;
    }
    return ok;
}

int gstplayer_web_load_url(struct gstplayer *player, const char *url) {
    struct web_task *task;
    ASSERT_NOT_NULL(player);
    ASSERT_NOT_NULL(url);

    task = calloc(1, sizeof(*task));
    if (task == NULL) return ENOMEM;
    task->type = kWebTaskLoadUrl;
    task->string = strdup(url);
    if (task->string == NULL) {
        destroy_web_task(task);
        return ENOMEM;
    }
    return enqueue_web_task(player, task);
}

int gstplayer_web_load_bytes(struct gstplayer *player, const uint8_t *data, size_t size) {
    struct web_task *task;
    ASSERT_NOT_NULL(player);

    task = calloc(1, sizeof(*task));
    if (task == NULL) return ENOMEM;
    task->type = kWebTaskLoadBytes;
    task->bytes = g_bytes_new(data, size);
    if (task->bytes == NULL) {
        destroy_web_task(task);
        return ENOMEM;
    }
    return enqueue_web_task(player, task);
}

int gstplayer_web_run_javascript(struct gstplayer *player, const char *source) {
    struct web_task *task;
    ASSERT_NOT_NULL(player);
    ASSERT_NOT_NULL(source);

    task = calloc(1, sizeof(*task));
    if (task == NULL) return ENOMEM;
    task->type = kWebTaskRunJavaScript;
    task->string = strdup(source);
    if (task->string == NULL) {
        destroy_web_task(task);
        return ENOMEM;
    }
    return enqueue_web_task(player, task);
}

int gstplayer_web_respond_to_handler(
    struct gstplayer *player,
    uint32_t call_id,
    const char *result_json,
    const char *error
) {
    struct pending_web_reply *pending;
    struct web_reply_task *task;

    ASSERT_NOT_NULL(player);
    task = calloc(1, sizeof(*task));
    if (task == NULL) return ENOMEM;
    task->result_json = result_json == NULL ? NULL : strdup(result_json);
    task->error = error == NULL ? NULL : strdup(error);
    if ((result_json != NULL && task->result_json == NULL) || (error != NULL && task->error == NULL)) {
        free(task->result_json);
        free(task->error);
        free(task);
        return ENOMEM;
    }

    g_mutex_lock(&player->web_lock);
    pending = g_hash_table_lookup(player->pending_web_replies, GUINT_TO_POINTER(call_id));
    if (pending != NULL) g_hash_table_steal(player->pending_web_replies, GUINT_TO_POINTER(call_id));
    g_mutex_unlock(&player->web_lock);
    if (pending == NULL) {
        free(task->result_json);
        free(task->error);
        free(task);
        return ENOENT;
    }

    task->pending = pending;
    g_main_context_invoke(pending->main_context, complete_web_reply, task);
    return 0;
}

int gstplayer_step_backward(struct gstplayer *player) {
    gboolean gst_ok;
    int ok;

    ASSERT_NOT_NULL(player);

    player->playpause_state = kStepping;
    player->direction = kBackward;
    ok = apply_playback_state(player);
    if (ok != 0) {
        return ok;
    }

    gst_ok = gst_element_send_event(player->pipeline, gst_event_new_step(GST_FORMAT_BUFFERS, 1, 1, TRUE, FALSE));
    if (gst_ok == FALSE) {
        LOG_ERROR("Could not send frame-step event to pipeline. (gst_element_send_event)\n");
        return EIO;
    }

    return 0;
}

struct notifier *gstplayer_get_web_event_notifier(struct gstplayer *player) {
    return &player->web_event_notifier;
}

struct notifier *gstplayer_get_video_info_notifier(struct gstplayer *player) {
    return &player->video_info_notifier;
}

struct notifier *gstplayer_get_buffering_state_notifier(struct gstplayer *player) {
    return &player->buffering_state_notifier;
}

struct notifier *gstplayer_get_error_notifier(struct gstplayer *player) {
    return &player->error_notifier;
}
