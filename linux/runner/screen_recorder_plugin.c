// screen_recorder_plugin.c  (Linux)
// Wayland: xdg-desktop-portal + PipeWire screen capture
// X11 fallback: XShmGetImage region capture
// Audio: PulseAudio mic capture
// Mux: GStreamer → WebM (VP8+Opus)

#include "screen_recorder_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <sys/shm.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <pulse/simple.h>
#include <pulse/error.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define CHANNEL_NAME "com.screenrecorder/recorder"
#define FPS          30
#define FRAME_NS     (1000000000LL / FPS)

#define AUDIO_SAMPLE_RATE    44100
#define AUDIO_CHANNELS       1
#define AUDIO_CHUNK_FRAMES   1024   // ~23ms per chunk
#define AUDIO_BYTES_PER_FRAME (AUDIO_CHANNELS * sizeof(int16_t))
#define AUDIO_CHUNK_BYTES    (AUDIO_CHUNK_FRAMES * AUDIO_BYTES_PER_FRAME)

#define PORTAL_BUS_NAME      "org.freedesktop.portal.Desktop"
#define PORTAL_OBJ_PATH      "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST_IF "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST_IF    "org.freedesktop.portal.Request"

// ─────────────────────────────────────────────────────────────────────────────
// RecorderState
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    // region
    int x, y, w, h;
    int screen_w, screen_h;  // full monitor dimensions (for PipeWire crop)
    char output_path[2048];
    bool use_pipewire;  // true on Wayland

    // X11 (only used when !use_pipewire)
    Display*        display;
    XShmSegmentInfo shm_info;
    XImage*         ximage;
    bool            has_shm;

    // PipeWire portal (only used when use_pipewire)
    int pw_fd;
    uint32_t pw_node_id;
    char* portal_session_handle;

    // PulseAudio
    pa_simple* pa;

    // GStreamer
    GstElement* pipeline;
    GstElement* video_src;   // appsrc (X11 mode) or NULL (PipeWire mode)
    GstElement* audio_src;

    // threads
    pthread_t   video_thread;  // only used in X11 mode
    pthread_t   audio_thread;
    atomic_bool running;
    atomic_llong start_ns;
} RecorderState;

static RecorderState* g_state = NULL;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool is_wayland(void) {
    const char* session = getenv("XDG_SESSION_TYPE");
    if (session && strcmp(session, "wayland") == 0) return true;
    if (getenv("WAYLAND_DISPLAY")) return true;
    return false;
}

// Check all runtime dependencies, returns a newly-allocated string describing
// missing deps (caller must g_free), or NULL if everything is OK.
static char* check_runtime_deps(void) {
    gst_init(NULL, NULL);

    GString* missing = g_string_new(NULL);

    // Required GStreamer elements
    static const char* required_elements[] = {
        "videoconvert", "vp8enc", "opusenc", "matroskamux",
        "filesink", "queue", "appsrc", "audioconvert",
        "audioresample", "videocrop", NULL
    };

    for (int i = 0; required_elements[i]; i++) {
        GstElementFactory* f = gst_element_factory_find(required_elements[i]);
        if (!f) {
            g_string_append_printf(missing, "  - GStreamer element '%s' not found\n",
                                   required_elements[i]);
        } else {
            gst_object_unref(f);
        }
    }

    // PipeWire source (needed on Wayland)
    if (is_wayland()) {
        GstElementFactory* pw = gst_element_factory_find("pipewiresrc");
        if (!pw) {
            g_string_append(missing, "  - GStreamer element 'pipewiresrc' not found (needed for Wayland)\n");
        } else {
            gst_object_unref(pw);
        }
    }

    // PulseAudio check — try to connect briefly
    pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 1 };
    int pa_err = 0;
    pa_simple* pa_test = pa_simple_new(NULL, "DepCheck", PA_STREAM_RECORD,
                                        NULL, "test", &ss, NULL, NULL, &pa_err);
    if (!pa_test) {
        g_string_append_printf(missing, "  - PulseAudio not available: %s\n",
                               pa_strerror(pa_err));
    } else {
        pa_simple_free(pa_test);
    }

    if (missing->len == 0) {
        g_string_free(missing, TRUE);
        return NULL;
    }

    // Add install hint
    g_string_prepend(missing, "Missing dependencies:\n");
    g_string_append(missing,
        "\nInstall with:\n"
        "  Ubuntu/Debian: sudo apt install gstreamer1.0-plugins-good "
        "gstreamer1.0-plugins-base gstreamer1.0-pipewire pulseaudio-utils\n"
        "  Fedora: sudo dnf install gstreamer1-plugins-good "
        "gstreamer1-plugins-base gstreamer1-pipewire pulseaudio-utils\n");

    return g_string_free(missing, FALSE);
}

// ─────────────────────────────────────────────────────────────────────────────
// GStreamer bus error helper
// ─────────────────────────────────────────────────────────────────────────────

static bool check_bus_errors(GstElement* pipeline) {
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* msg;
    bool had_error = false;

    while ((msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
        GError* gerr = NULL;
        gchar* debug = NULL;

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(msg, &gerr, &debug);
            fprintf(stderr, "[recorder] GST ERROR from %s: %s\n",
                    GST_OBJECT_NAME(msg->src),
                    gerr ? gerr->message : "unknown");
            if (debug) fprintf(stderr, "[recorder]   debug: %s\n", debug);
            had_error = true;
        } else {
            gst_message_parse_warning(msg, &gerr, &debug);
            fprintf(stderr, "[recorder] GST WARNING from %s: %s\n",
                    GST_OBJECT_NAME(msg->src),
                    gerr ? gerr->message : "unknown");
            if (debug) fprintf(stderr, "[recorder]   debug: %s\n", debug);
        }

        if (gerr) g_error_free(gerr);
        g_free(debug);
        gst_message_unref(msg);
    }

    gst_object_unref(bus);
    return had_error;
}

// ─────────────────────────────────────────────────────────────────────────────
// Portal ScreenCast (Wayland)
// Uses org.freedesktop.portal.ScreenCast D-Bus API to get a PipeWire stream.
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    GMainLoop* loop;
    uint32_t response;
    GVariant* results;
} PortalResponse;

static void on_portal_response(GDBusConnection* conn,
                                const gchar* sender,
                                const gchar* object_path,
                                const gchar* interface_name,
                                const gchar* signal_name,
                                GVariant* parameters,
                                gpointer user_data) {
    PortalResponse* pr = (PortalResponse*)user_data;
    g_variant_get(parameters, "(u@a{sv})", &pr->response, &pr->results);
    g_main_loop_quit(pr->loop);
}

// Wait for Response signal on a portal request.
// Returns the results variant (caller must unref), or NULL on error.
static GVariant* portal_wait_response(GDBusConnection* bus,
                                       const char* request_path,
                                       int timeout_sec) {
    PortalResponse pr = { .loop = g_main_loop_new(NULL, FALSE), .response = 99, .results = NULL };

    guint sig_id = g_dbus_connection_signal_subscribe(
        bus, PORTAL_BUS_NAME, PORTAL_REQUEST_IF, "Response",
        request_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        on_portal_response, &pr, NULL);

    // Timeout to avoid hanging forever
    guint timeout_id = g_timeout_add_seconds(timeout_sec, (GSourceFunc)g_main_loop_quit, pr.loop);

    g_main_loop_run(pr.loop);

    g_source_remove(timeout_id);
    g_dbus_connection_signal_unsubscribe(bus, sig_id);
    g_main_loop_unref(pr.loop);

    if (pr.response != 0) {
        fprintf(stderr, "[recorder] Portal response code: %u\n", pr.response);
        if (pr.results) g_variant_unref(pr.results);
        return NULL;
    }

    return pr.results;
}

// Build the portal request token and object path
static char* portal_make_request_path(GDBusConnection* bus, const char* token) {
    const gchar* unique = g_dbus_connection_get_unique_name(bus);
    // Convert ":1.234" → "1_234"
    char sender_clean[128];
    int j = 0;
    for (int i = 0; unique[i]; i++) {
        if (unique[i] == ':') continue;
        sender_clean[j++] = (unique[i] == '.') ? '_' : unique[i];
    }
    sender_clean[j] = '\0';

    char* path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                                  sender_clean, token);
    return path;
}

static bool setup_screencast_portal(RecorderState* s, char* err_out) {
    static int portal_counter = 0;
    portal_counter++;

    GError* error = NULL;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) {
        snprintf(err_out, 512, "D-Bus session bus failed: %s",
                 error ? error->message : "unknown");
        if (error) g_error_free(error);
        return false;
    }

    fprintf(stderr, "[recorder] Portal: starting ScreenCast session...\n");

    // Generate unique tokens per session to avoid portal conflicts
    char tok1[64], tok2[64], tok3[64], sess_tok[64];
    snprintf(tok1, sizeof(tok1), "screc%da", portal_counter);
    snprintf(tok2, sizeof(tok2), "screc%db", portal_counter);
    snprintf(tok3, sizeof(tok3), "screc%dc", portal_counter);
    snprintf(sess_tok, sizeof(sess_tok), "screcsess%d", portal_counter);

    // 1. CreateSession
    char* req_path1 = portal_make_request_path(bus, tok1);
    GVariantBuilder opts1;
    g_variant_builder_init(&opts1, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts1, "{sv}", "handle_token", g_variant_new_string(tok1));
    g_variant_builder_add(&opts1, "{sv}", "session_handle_token", g_variant_new_string(sess_tok));

    GVariant* ret1 = g_dbus_connection_call_sync(
        bus, PORTAL_BUS_NAME, PORTAL_OBJ_PATH, PORTAL_SCREENCAST_IF,
        "CreateSession",
        g_variant_new("(a{sv})", &opts1),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

    if (!ret1) {
        snprintf(err_out, 512, "Portal CreateSession failed: %s",
                 error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_free(req_path1);
        g_object_unref(bus);
        return false;
    }
    g_variant_unref(ret1);

    GVariant* res1 = portal_wait_response(bus, req_path1, 10);
    g_free(req_path1);
    if (!res1) {
        strcpy(err_out, "Portal CreateSession: no response or user cancelled");
        g_object_unref(bus);
        return false;
    }

    GVariant* session_var = g_variant_lookup_value(res1, "session_handle", G_VARIANT_TYPE_STRING);
    if (!session_var) {
        strcpy(err_out, "Portal CreateSession: no session_handle in response");
        g_variant_unref(res1);
        g_object_unref(bus);
        return false;
    }
    const char* session_handle = g_variant_get_string(session_var, NULL);
    s->portal_session_handle = g_strdup(session_handle);
    fprintf(stderr, "[recorder] Portal session: %s\n", s->portal_session_handle);
    g_variant_unref(session_var);
    g_variant_unref(res1);

    // 2. SelectSources (type=1=MONITOR, allow multiple=false)
    char* req_path2 = portal_make_request_path(bus, tok2);
    GVariantBuilder opts2;
    g_variant_builder_init(&opts2, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts2, "{sv}", "handle_token", g_variant_new_string(tok2));
    g_variant_builder_add(&opts2, "{sv}", "types", g_variant_new_uint32(1));  // MONITOR
    g_variant_builder_add(&opts2, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts2, "{sv}", "types",       g_variant_new_uint32(1));   // MONITOR
    g_variant_builder_add(&opts2, "{sv}", "multiple",    g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts2, "{sv}", "cursor_mode", g_variant_new_uint32(2));   // ← ADD: embedded cursor

    GVariant* ret2 = g_dbus_connection_call_sync(
        bus, PORTAL_BUS_NAME, PORTAL_OBJ_PATH, PORTAL_SCREENCAST_IF,
        "SelectSources",
        g_variant_new("(oa{sv})", s->portal_session_handle, &opts2),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

    if (!ret2) {
        snprintf(err_out, 512, "Portal SelectSources failed: %s",
                 error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_free(req_path2);
        g_object_unref(bus);
        return false;
    }
    g_variant_unref(ret2);

    GVariant* res2 = portal_wait_response(bus, req_path2, 10);
    g_free(req_path2);
    if (!res2) {
        strcpy(err_out, "Portal SelectSources: no response or cancelled");
        g_object_unref(bus);
        return false;
    }
    g_variant_unref(res2);

    // 3. Start — this shows the GNOME screen picker dialog
    char* req_path3 = portal_make_request_path(bus, tok3);
    GVariantBuilder opts3;
    g_variant_builder_init(&opts3, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts3, "{sv}", "handle_token", g_variant_new_string(tok3));

    GVariant* ret3 = g_dbus_connection_call_sync(
        bus, PORTAL_BUS_NAME, PORTAL_OBJ_PATH, PORTAL_SCREENCAST_IF,
        "Start",
        g_variant_new("(osa{sv})", s->portal_session_handle, "", &opts3),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

    if (!ret3) {
        snprintf(err_out, 512, "Portal Start failed: %s",
                 error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_free(req_path3);
        g_object_unref(bus);
        return false;
    }
    g_variant_unref(ret3);

    // Wait for user to pick a screen (longer timeout — user interaction)
    GVariant* res3 = portal_wait_response(bus, req_path3, 60);
    g_free(req_path3);
    if (!res3) {
        strcpy(err_out, "Portal Start: user cancelled or timeout");
        g_object_unref(bus);
        return false;
    }

    // Extract streams array → node_id
    GVariant* streams_var = g_variant_lookup_value(res3, "streams", NULL);
    if (!streams_var) {
        strcpy(err_out, "Portal Start: no streams in response");
        g_variant_unref(res3);
        g_object_unref(bus);
        return false;
    }

    // streams is a(ua{sv})
    GVariantIter iter;
    g_variant_iter_init(&iter, streams_var);
    GVariant* stream_entry = g_variant_iter_next_value(&iter);
    if (!stream_entry) {
        strcpy(err_out, "Portal Start: empty streams array");
        g_variant_unref(streams_var);
        g_variant_unref(res3);
        g_object_unref(bus);
        return false;
    }

    g_variant_get_child(stream_entry, 0, "u", &s->pw_node_id);
    fprintf(stderr, "[recorder] Portal PipeWire node_id: %u\n", s->pw_node_id);
    g_variant_unref(stream_entry);
    g_variant_unref(streams_var);
    g_variant_unref(res3);

    // 4. OpenPipeWireRemote — returns a file descriptor
    GUnixFDList* fd_list = NULL;
    GVariantBuilder opts4;
    g_variant_builder_init(&opts4, G_VARIANT_TYPE("a{sv}"));

    GVariant* ret4 = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, PORTAL_BUS_NAME, PORTAL_OBJ_PATH, PORTAL_SCREENCAST_IF,
        "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", s->portal_session_handle, &opts4),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 5000,
        NULL, &fd_list, NULL, &error);

    if (!ret4) {
        snprintf(err_out, 512, "Portal OpenPipeWireRemote failed: %s",
                 error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_object_unref(bus);
        return false;
    }

    gint32 fd_idx = 0;
    g_variant_get(ret4, "(h)", &fd_idx);
    s->pw_fd = g_unix_fd_list_get(fd_list, fd_idx, &error);
    g_variant_unref(ret4);
    g_object_unref(fd_list);

    if (s->pw_fd < 0) {
        snprintf(err_out, 512, "Portal: failed to get PipeWire fd: %s",
                 error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_object_unref(bus);
        return false;
    }

    fprintf(stderr, "[recorder] Portal PipeWire fd: %d, node: %u\n",
            s->pw_fd, s->pw_node_id);

    g_object_unref(bus);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// GStreamer pipeline
// ─────────────────────────────────────────────────────────────────────────────

static bool build_pipeline_pipewire(RecorderState* s, char* err_out) {
    gst_init(NULL, NULL);

    // PipeWire source → videoconvert → videocrop → vp8enc → mux → filesink
    // Audio: appsrc → audioconvert → audioresample → opusenc → mux
    // videocrop properties set programmatically (inline parsing unreliable)
    int crop_top = s->y;
    int crop_left = s->x;
    int crop_bottom = s->screen_h - (s->y + s->h);
    int crop_right = s->screen_w - (s->x + s->w);
    if (crop_top < 0) crop_top = 0;
    if (crop_left < 0) crop_left = 0;
    if (crop_bottom < 0) crop_bottom = 0;
    if (crop_right < 0) crop_right = 0;

    fprintf(stderr, "[recorder] Crop: top=%d left=%d right=%d bottom=%d (screen %dx%d, region %d,%d %dx%d)\n",
            crop_top, crop_left, crop_right, crop_bottom,
            s->screen_w, s->screen_h, s->x, s->y, s->w, s->h);

    char pipeline_str[4096];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "pipewiresrc fd=%d path=%u do-timestamp=true keepalive-time=1000 "
        "! videoconvert "
        "! video/x-raw,format=I420 "
        "! videocrop name=crop "
        "! vp8enc deadline=1 cpu-used=8 target-bitrate=2000000 keyframe-max-dist=30 "
        "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 "
        "! matroskamux name=mux streamable=true "
        "! filesink location=\"%s\" sync=false "
        "appsrc name=asrc "
        "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 "
        "! audioconvert "
        "! audioresample "
        "! opusenc "
        "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 "
        "! mux. ",
        s->pw_fd, s->pw_node_id, s->output_path);

    fprintf(stderr, "[recorder] Pipeline (PipeWire): %s\n", pipeline_str);

    GError* gerr = NULL;
    s->pipeline = gst_parse_launch(pipeline_str, &gerr);
    if (!s->pipeline) {
        snprintf(err_out, 512, "GStreamer pipeline failed: %s",
                 gerr ? gerr->message : "unknown");
        if (gerr) g_error_free(gerr);
        return false;
    }
    if (gerr) {
        fprintf(stderr, "[recorder] Pipeline parse warning: %s\n", gerr->message);
        g_error_free(gerr);
    }

    // Set videocrop properties programmatically
    GstElement* crop = gst_bin_get_by_name(GST_BIN(s->pipeline), "crop");
    if (crop) {
        g_object_set(crop,
            "top",    crop_top,
            "bottom", crop_bottom,
            "left",   crop_left,
            "right",  crop_right,
            NULL);
        fprintf(stderr, "[recorder] videocrop set: top=%d bottom=%d left=%d right=%d\n",
                crop_top, crop_bottom, crop_left, crop_right);
        gst_object_unref(crop);
    } else {
        fprintf(stderr, "[recorder] WARNING: videocrop element not found in pipeline!\n");
    }

    s->video_src = NULL;  // No video appsrc — pipewiresrc handles video
    s->audio_src = gst_bin_get_by_name(GST_BIN(s->pipeline), "asrc");

    if (!s->audio_src) {
        snprintf(err_out, 512, "Failed to find audio appsrc in pipeline");
        gst_object_unref(s->pipeline);
        s->pipeline = NULL;
        return false;
    }

    // Configure audio appsrc
    GstCaps* acaps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "S16LE",
        "layout",   G_TYPE_STRING, "interleaved",
        "channels", G_TYPE_INT,    AUDIO_CHANNELS,
        "rate",     G_TYPE_INT,    AUDIO_SAMPLE_RATE,
        NULL);
    g_object_set(s->audio_src,
        "caps",         acaps,
        "format",       GST_FORMAT_TIME,
        "is-live",      TRUE,
        "do-timestamp", FALSE,
        "block",        FALSE,
        "max-bytes",    (guint64)0,
        "stream-type",  0,
        NULL);
    gst_caps_unref(acaps);

    fprintf(stderr, "[recorder] Pipeline built (PipeWire) → %s\n", s->output_path);

    GstStateChangeReturn ret = gst_element_set_state(s->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        strcpy(err_out, "Failed to set pipeline to PLAYING");
        check_bus_errors(s->pipeline);
        if (s->audio_src) gst_object_unref(s->audio_src);
        gst_object_unref(s->pipeline);
        s->pipeline = NULL;
        return false;
    }

    fprintf(stderr, "[recorder] Pipeline set to PLAYING (ret=%d)\n", ret);
    return true;
}

static bool build_pipeline_x11(RecorderState* s, char* err_out) {
    gst_init(NULL, NULL);

    char pipeline_str[4096];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "appsrc name=vsrc "
        "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 "
        "! videoconvert "
        "! vp8enc deadline=1 cpu-used=8 target-bitrate=2000000 keyframe-max-dist=30 "
        "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 "
        "! matroskamux name=mux streamable=true "
        "! filesink location=\"%s\" sync=false "
        "appsrc name=asrc "
        "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 "
        "! audioconvert "
        "! audioresample "
        "! opusenc "
        "! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 "
        "! mux. ",
        s->output_path);

    fprintf(stderr, "[recorder] Pipeline (X11): %s\n", pipeline_str);

    GError* gerr = NULL;
    s->pipeline = gst_parse_launch(pipeline_str, &gerr);
    if (!s->pipeline) {
        snprintf(err_out, 512, "GStreamer pipeline failed: %s",
                 gerr ? gerr->message : "unknown");
        if (gerr) g_error_free(gerr);
        return false;
    }
    if (gerr) {
        fprintf(stderr, "[recorder] Pipeline parse warning: %s\n", gerr->message);
        g_error_free(gerr);
    }

    s->video_src = gst_bin_get_by_name(GST_BIN(s->pipeline), "vsrc");
    s->audio_src = gst_bin_get_by_name(GST_BIN(s->pipeline), "asrc");

    if (!s->video_src || !s->audio_src) {
        snprintf(err_out, 512, "Failed to find appsrc elements in pipeline");
        gst_object_unref(s->pipeline);
        s->pipeline = NULL;
        return false;
    }

    // Configure video appsrc
    GstCaps* vcaps = gst_caps_new_simple("video/x-raw",
        "format",    G_TYPE_STRING,  "BGRx",
        "width",     G_TYPE_INT,     s->w,
        "height",    G_TYPE_INT,     s->h,
        "framerate", GST_TYPE_FRACTION, FPS, 1,
        NULL);
    g_object_set(s->video_src,
        "caps",         vcaps,
        "format",       GST_FORMAT_TIME,
        "is-live",      TRUE,
        "do-timestamp", FALSE,
        "block",        FALSE,
        "max-bytes",    (guint64)0,
        "stream-type",  0,
        NULL);
    gst_caps_unref(vcaps);

    // Configure audio appsrc
    GstCaps* acaps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "S16LE",
        "layout",   G_TYPE_STRING, "interleaved",
        "channels", G_TYPE_INT,    AUDIO_CHANNELS,
        "rate",     G_TYPE_INT,    AUDIO_SAMPLE_RATE,
        NULL);
    g_object_set(s->audio_src,
        "caps",         acaps,
        "format",       GST_FORMAT_TIME,
        "is-live",      TRUE,
        "do-timestamp", FALSE,
        "block",        FALSE,
        "max-bytes",    (guint64)0,
        "stream-type",  0,
        NULL);
    gst_caps_unref(acaps);

    fprintf(stderr, "[recorder] Pipeline built (X11): %dx%d @ %d fps → %s\n",
            s->w, s->h, FPS, s->output_path);

    GstStateChangeReturn ret = gst_element_set_state(s->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        strcpy(err_out, "Failed to set pipeline to PLAYING");
        check_bus_errors(s->pipeline);
        gst_object_unref(s->video_src);
        gst_object_unref(s->audio_src);
        gst_object_unref(s->pipeline);
        s->pipeline = NULL;
        return false;
    }

    fprintf(stderr, "[recorder] Pipeline set to PLAYING (ret=%d)\n", ret);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// X11 capture init (only for native X11 sessions)
// ─────────────────────────────────────────────────────────────────────────────

static bool init_x11_capture(RecorderState* s, char* err_out) {
    s->display = XOpenDisplay(NULL);
    if (!s->display) {
        strcpy(err_out, "XOpenDisplay failed — no DISPLAY?");
        return false;
    }

    int screen = DefaultScreen(s->display);
    s->has_shm = false;

    // Try XShm for fast zero-copy capture (works on native X11, not on XWayland)
    int shm_major, shm_minor;
    Bool shm_pixmaps;
    if (XShmQueryVersion(s->display, &shm_major, &shm_minor, &shm_pixmaps)) {
        s->ximage = XShmCreateImage(
            s->display,
            DefaultVisual(s->display, screen),
            DefaultDepth(s->display, screen),
            ZPixmap, NULL, &s->shm_info,
            s->w, s->h);

        if (s->ximage) {
            s->shm_info.shmid = shmget(IPC_PRIVATE,
                (size_t)s->ximage->bytes_per_line * s->ximage->height,
                IPC_CREAT | 0600);
            if (s->shm_info.shmid != -1) {
                s->shm_info.shmaddr = s->ximage->data = (char*)shmat(s->shm_info.shmid, NULL, 0);
                s->shm_info.readOnly = False;
                XShmAttach(s->display, &s->shm_info);
                XSync(s->display, False);
                s->has_shm = true;
                fprintf(stderr, "[recorder] X11: using XShm capture\n");
            } else {
                XDestroyImage(s->ximage);
                s->ximage = NULL;
            }
        }
    }

    if (!s->has_shm) {
        // XGetImage fallback
        s->ximage = NULL;
        fprintf(stderr, "[recorder] X11: using XGetImage fallback\n");
    }

    return true;
}

static void cleanup_x11(RecorderState* s) {
    if (s->display) {
        if (s->has_shm) {
            XShmDetach(s->display, &s->shm_info);
            shmdt(s->shm_info.shmaddr);
            shmctl(s->shm_info.shmid, IPC_RMID, NULL);
        }
        if (s->ximage) XDestroyImage(s->ximage);
        XCloseDisplay(s->display);
    }
}

// Composite the X11 cursor onto the BGRx frame buffer.
// frame_data: pointer to the frame (BGRx, stride = w*4)
// frame_x, frame_y: top-left of the capture region on screen
static void draw_cursor_on_frame(Display* dpy,
                                  uint8_t* frame_data,
                                  int frame_x, int frame_y,
                                  int frame_w, int frame_h) {
    XFixesCursorImage* ci = XFixesGetCursorImage(dpy);
    if (!ci) return;

    // ci->x, ci->y is the cursor position on screen (top-left of hotspot)
    // ci->xhot, ci->yhot is the hotspot offset within the cursor image
    int cx = (int)ci->x - ci->xhot - frame_x;
    int cy = (int)ci->y - ci->yhot - frame_y;
    int cw = (int)ci->width;
    int ch = (int)ci->height;

    for (int row = 0; row < ch; row++) {
        int fy = cy + row;
        if (fy < 0 || fy >= frame_h) continue;

        for (int col = 0; col < cw; col++) {
            int fx = cx + col;
            if (fx < 0 || fx >= frame_w) continue;

            // XFixes pixels are ARGB packed in unsigned long
            uint32_t cpx = (uint32_t)ci->pixels[row * cw + col];
            uint8_t  a   = (cpx >> 24) & 0xFF;
            if (a == 0) continue;

            uint8_t cr = (cpx >> 16) & 0xFF;
            uint8_t cg = (cpx >>  8) & 0xFF;
            uint8_t cb = (cpx >>  0) & 0xFF;

            uint8_t* dst = frame_data + (fy * frame_w + fx) * 4;
            // Frame is BGRx
            dst[0] = (uint8_t)((cb * a + dst[0] * (255 - a)) / 255);  // B
            dst[1] = (uint8_t)((cg * a + dst[1] * (255 - a)) / 255);  // G
            dst[2] = (uint8_t)((cr * a + dst[2] * (255 - a)) / 255);  // R
            // dst[3] is padding, leave it
        }
    }

    XFree(ci);
}

// ─────────────────────────────────────────────────────────────────────────────
// Video thread (X11 mode only — PipeWire mode doesn't need this)
// ─────────────────────────────────────────────────────────────────────────────

static void* video_thread_proc(void* arg) {
    RecorderState* s = (RecorderState*)arg;
    Window root = RootWindow(s->display, DefaultScreen(s->display));

    long long frame_idx = 0;
    const int expected_stride = s->w * 4;

    fprintf(stderr, "[recorder] Video thread started (X11): %dx%d, stride=%d, shm=%d\n",
            s->w, s->h, expected_stride, s->has_shm);

    while (atomic_load(&s->running)) {
        long long frame_start = now_ns();

        if (s->has_shm) {
            XShmGetImage(s->display, root, s->ximage,
                         s->x, s->y, AllPlanes);
        } else {
            if (s->ximage) XDestroyImage(s->ximage);
            s->ximage = XGetImage(s->display, root,
                                  s->x, s->y, s->w, s->h,
                                  AllPlanes, ZPixmap);
            if (!s->ximage) { usleep(10000); continue; }
        }

        gsize byte_size = (gsize)expected_stride * s->h;
        GstBuffer* buf = gst_buffer_new_allocate(NULL, byte_size, NULL);

        GstMapInfo map;
        gst_buffer_map(buf, &map, GST_MAP_WRITE);

        int ximage_stride = s->ximage->bytes_per_line;
        if (ximage_stride == expected_stride) {
            memcpy(map.data, s->ximage->data, byte_size);
        } else {
            for (int row = 0; row < s->h; row++) {
                memcpy(map.data + row * expected_stride,
                       s->ximage->data + row * ximage_stride,
                       expected_stride);
            }
        }

         // ── Draw cursor onto the frame ──────────────────────────────────────
        draw_cursor_on_frame(s->display, map.data, s->x, s->y, s->w, s->h);
        // ───────────────────────────────────────────────────────────────────

        gst_buffer_unmap(buf, &map);


        GstClockTime ts = (GstClockTime)((long long)frame_idx * FRAME_NS);
        GST_BUFFER_PTS(buf)      = ts;
        GST_BUFFER_DTS(buf)      = ts;
        GST_BUFFER_DURATION(buf) = (GstClockTime)FRAME_NS;

        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(s->video_src), buf);
        if (ret != GST_FLOW_OK) {
            fprintf(stderr, "[recorder] Video push error: %s (frame %lld)\n",
                    gst_flow_get_name(ret), frame_idx);
            check_bus_errors(s->pipeline);
            break;
        }

        frame_idx++;

        if (frame_idx % 30 == 0) {
            if (check_bus_errors(s->pipeline)) {
                fprintf(stderr, "[recorder] Pipeline error detected, stopping video\n");
                break;
            }
        }

        long long elapsed = now_ns() - frame_start;
        long long sleep_ns = FRAME_NS - elapsed;
        if (sleep_ns > 0) {
            struct timespec ts2 = {
                .tv_sec  = sleep_ns / 1000000000LL,
                .tv_nsec = sleep_ns % 1000000000LL,
            };
            nanosleep(&ts2, NULL);
        }
    }

    fprintf(stderr, "[recorder] Video thread: sent %lld frames, sending EOS\n", frame_idx);
    gst_app_src_end_of_stream(GST_APP_SRC(s->video_src));
    return NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread (PulseAudio → GStreamer appsrc)
// ─────────────────────────────────────────────────────────────────────────────

static void* audio_thread_proc(void* arg) {
    RecorderState* s = (RecorderState*)arg;

    int16_t buf[AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS];

    fprintf(stderr, "[recorder] Audio thread started: %d Hz, %d ch\n",
            AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);

    const long long chunk_ns =
        (long long)AUDIO_CHUNK_FRAMES * 1000000000LL / AUDIO_SAMPLE_RATE;

    int pa_err = 0;
    long long chunk_count = 0;
    long long audio_pts = 0;
    while (atomic_load(&s->running)) {
        int ret = pa_simple_read(s->pa, buf, AUDIO_CHUNK_BYTES, &pa_err);
        if (ret < 0) {
            fprintf(stderr, "[recorder] PulseAudio read error: %s\n",
                    pa_strerror(pa_err));
            continue;
        }

        GstBuffer* gbuf = gst_buffer_new_allocate(NULL, AUDIO_CHUNK_BYTES, NULL);
        GstMapInfo map;
        gst_buffer_map(gbuf, &map, GST_MAP_WRITE);
        memcpy(map.data, buf, AUDIO_CHUNK_BYTES);
        gst_buffer_unmap(gbuf, &map);

        GST_BUFFER_PTS(gbuf)      = (GstClockTime)audio_pts;
        GST_BUFFER_DTS(gbuf)      = (GstClockTime)audio_pts;
        GST_BUFFER_DURATION(gbuf) = (GstClockTime)chunk_ns;
        audio_pts += chunk_ns;

        GstFlowReturn fr = gst_app_src_push_buffer(GST_APP_SRC(s->audio_src), gbuf);
        if (fr != GST_FLOW_OK) {
            fprintf(stderr, "[recorder] Audio push error: %s (chunk %lld)\n",
                    gst_flow_get_name(fr), chunk_count);
            check_bus_errors(s->pipeline);
            break;
        }
        chunk_count++;
    }

    fprintf(stderr, "[recorder] Audio thread: sent %lld chunks, sending EOS\n", chunk_count);
    gst_app_src_end_of_stream(GST_APP_SRC(s->audio_src));
    return NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public start / stop
// ─────────────────────────────────────────────────────────────────────────────

static bool recorder_start(int x, int y, int w, int h,
                            const char* output_path, char* err_out) {
    if (g_state) { strcpy(err_out, "Already recording"); return false; }

    RecorderState* s = calloc(1, sizeof(RecorderState));
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->pw_fd = -1;

    // Get screen dimensions for PipeWire crop calculation
    Display* tmp_dpy = XOpenDisplay(NULL);
    if (tmp_dpy) {
        int scr = DefaultScreen(tmp_dpy);
        s->screen_w = DisplayWidth(tmp_dpy, scr);
        s->screen_h = DisplayHeight(tmp_dpy, scr);
        XCloseDisplay(tmp_dpy);
    } else {
        s->screen_w = w; s->screen_h = h;  // fallback: no crop
    }
    strncpy(s->output_path, output_path, sizeof(s->output_path) - 1);

    s->use_pipewire = is_wayland();
    fprintf(stderr, "[recorder] Session type: %s\n", s->use_pipewire ? "Wayland (PipeWire)" : "X11");

    if (s->use_pipewire) {
        // Get PipeWire stream from portal
        if (!setup_screencast_portal(s, err_out)) {
            free(s);
            return false;
        }
    } else {
        // X11 capture setup
        if (!init_x11_capture(s, err_out)) {
            free(s);
            return false;
        }
    }

    // PulseAudio
    pa_sample_spec ss = {
        .format   = PA_SAMPLE_S16LE,
        .rate     = AUDIO_SAMPLE_RATE,
        .channels = AUDIO_CHANNELS,
    };
    int pa_err = 0;
    s->pa = pa_simple_new(NULL, "ScreenRecorder", PA_STREAM_RECORD,
                          NULL, "mic", &ss, NULL, NULL, &pa_err);
    if (!s->pa) {
        snprintf(err_out, 512, "PulseAudio connect failed: %s",
                 pa_strerror(pa_err));
        if (!s->use_pipewire) cleanup_x11(s);
        free(s);
        return false;
    }

    // GStreamer pipeline
    bool pipeline_ok;
    if (s->use_pipewire) {
        pipeline_ok = build_pipeline_pipewire(s, err_out);
    } else {
        pipeline_ok = build_pipeline_x11(s, err_out);
    }

    if (!pipeline_ok) {
        pa_simple_free(s->pa);
        if (!s->use_pipewire) cleanup_x11(s);
        free(s);
        return false;
    }

    atomic_store(&s->running, true);
    atomic_store(&s->start_ns, now_ns());
    g_state = s;

    // Video thread only needed for X11 mode
    if (!s->use_pipewire) {
        pthread_create(&s->video_thread, NULL, video_thread_proc, s);
    }
    pthread_create(&s->audio_thread, NULL, audio_thread_proc, s);
    return true;
}

static const char* recorder_stop(void) {
    if (!g_state) return NULL;
    RecorderState* s = g_state;

    atomic_store(&s->running, false);

    if (!s->use_pipewire) {
        pthread_join(s->video_thread, NULL);
    }
    pthread_join(s->audio_thread, NULL);

    fprintf(stderr, "[recorder] Threads joined, waiting for EOS...\n");

    // For PipeWire mode, send EOS on the pipeline to finalize
    if (s->use_pipewire) {
        gst_element_send_event(s->pipeline, gst_event_new_eos());
    }

    GstBus* bus = gst_element_get_bus(s->pipeline);
    GstMessage* msg = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            fprintf(stderr, "[recorder] EOS received — file finalized\n");
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* gerr = NULL;
            gchar* debug = NULL;
            gst_message_parse_error(msg, &gerr, &debug);
            fprintf(stderr, "[recorder] GST ERROR from %s: %s\n",
                    GST_OBJECT_NAME(msg->src),
                    gerr ? gerr->message : "unknown");
            if (debug) fprintf(stderr, "[recorder]   debug: %s\n", debug);
            if (gerr) g_error_free(gerr);
            g_free(debug);
        }
        gst_message_unref(msg);
    } else {
        fprintf(stderr, "[recorder] WARNING: EOS timeout (10s) — file may be incomplete\n");
    }
    gst_object_unref(bus);

    check_bus_errors(s->pipeline);

    if (s->video_src) gst_object_unref(s->video_src);
    if (s->audio_src) gst_object_unref(s->audio_src);

    gst_element_set_state(s->pipeline, GST_STATE_NULL);
    gst_object_unref(s->pipeline);

    pa_simple_free(s->pa);

    if (s->use_pipewire) {
        if (s->pw_fd >= 0) close(s->pw_fd);
        g_free(s->portal_session_handle);
    } else {
        cleanup_x11(s);
    }

    static char path_copy[2048];
    strncpy(path_copy, s->output_path, sizeof(path_copy) - 1);

    free(s);
    g_state = NULL;
    return path_copy;
}

// ─────────────────────────────────────────────────────────────────────────────
// Region selector — fullscreen X11 RGBA overlay with Cairo drawing
// (Works via XWayland on Wayland sessions)
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    int x, y, width, height;
    bool valid;
} SelectedRegion;

static void overlay_draw(Display* dpy, Window win, cairo_surface_t* surface,
                         int screen_w, int screen_h,
                         bool dragging, int sx, int sy, int cx, int cy) {
    cairo_t* cr = cairo_create(surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_paint(cr);

    if (dragging) {
        int rx = sx < cx ? sx : cx;
        int ry = sy < cy ? sy : cy;
        int rw = abs(cx - sx);
        int rh = abs(cy - sy);

        if (rw > 0 && rh > 0) {
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_rgba(cr, 0, 0, 0, 0);
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_fill(cr);

            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_set_source_rgba(cr, 0.145, 0.388, 0.922, 1.0);
            cairo_set_line_width(cr, 2.0);
            cairo_rectangle(cr, rx + 1, ry + 1, rw - 2, rh - 2);
            cairo_stroke(cr);

            double handle_len = 14.0;
            double hw = 3.0;
            cairo_set_line_width(cr, hw);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

            cairo_move_to(cr, rx, ry); cairo_line_to(cr, rx + handle_len, ry); cairo_stroke(cr);
            cairo_move_to(cr, rx, ry); cairo_line_to(cr, rx, ry + handle_len); cairo_stroke(cr);
            cairo_move_to(cr, rx + rw, ry); cairo_line_to(cr, rx + rw - handle_len, ry); cairo_stroke(cr);
            cairo_move_to(cr, rx + rw, ry); cairo_line_to(cr, rx + rw, ry + handle_len); cairo_stroke(cr);
            cairo_move_to(cr, rx, ry + rh); cairo_line_to(cr, rx + handle_len, ry + rh); cairo_stroke(cr);
            cairo_move_to(cr, rx, ry + rh); cairo_line_to(cr, rx, ry + rh - handle_len); cairo_stroke(cr);
            cairo_move_to(cr, rx + rw, ry + rh); cairo_line_to(cr, rx + rw - handle_len, ry + rh); cairo_stroke(cr);
            cairo_move_to(cr, rx + rw, ry + rh); cairo_line_to(cr, rx + rw, ry + rh - handle_len); cairo_stroke(cr);

            char label[64];
            snprintf(label, sizeof(label), "%d x %d", rw, rh);

            cairo_set_font_size(cr, 13.0);
            cairo_text_extents_t extents;
            cairo_text_extents(cr, label, &extents);

            double lx = rx;
            double ly = ry - 8;
            double pad_x = 8, pad_y = 4;
            double bg_w = extents.width + 2 * pad_x;
            double bg_h = extents.height + 2 * pad_y;

            if (ly - bg_h < 0) ly = ry + rh + 8 + bg_h;

            cairo_set_source_rgba(cr, 0.145, 0.388, 0.922, 1.0);
            cairo_rectangle(cr, lx, ly - bg_h, bg_w, bg_h);
            cairo_fill(cr);

            cairo_set_source_rgba(cr, 1, 1, 1, 1);
            cairo_move_to(cr, lx + pad_x, ly - pad_y);
            cairo_show_text(cr, label);
        }
    } else {
        const char* hint = "Drag to select recording area  \xC2\xB7  Esc to cancel";
        cairo_set_font_size(cr, 16.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, hint, &ext);

        double bx = (screen_w - ext.width) / 2.0 - 20;
        double by = 32;
        double bw = ext.width + 40;
        double bh = ext.height + 20;

        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_move_to(cr, bx + 20, by + 10 + ext.height);
        cairo_show_text(cr, hint);
    }

    cairo_destroy(cr);
    XFlush(dpy);
}

static SelectedRegion do_select_region(void) {
    SelectedRegion result = {0, 0, 0, 0, false};

    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) return result;

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int screen_w = DisplayWidth(dpy, screen);
    int screen_h = DisplayHeight(dpy, screen);

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        XCloseDisplay(dpy);
        return result;
    }

    Colormap cmap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.colormap = cmap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | KeyPressMask | StructureNotifyMask;

    Window overlay = XCreateWindow(
        dpy, root,
        0, 0, screen_w, screen_h, 0,
        vinfo.depth, InputOutput, vinfo.visual,
        CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask,
        &attrs);

    Atom wm_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom wm_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, overlay, wm_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)&wm_type_dock, 1);

    XMapRaised(dpy, overlay);
    XFlush(dpy);

    Cursor crosshair = XCreateFontCursor(dpy, XC_crosshair);
    XGrabPointer(dpy, overlay, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync,
                 overlay, crosshair, CurrentTime);
    XGrabKeyboard(dpy, overlay, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    cairo_surface_t* surface = cairo_xlib_surface_create(
        dpy, overlay, vinfo.visual, screen_w, screen_h);

    overlay_draw(dpy, overlay, surface, screen_w, screen_h,
                 false, 0, 0, 0, 0);

    bool dragging = false;
    bool done = false;
    int sx = 0, sy = 0, cx = 0, cy = 0;

    while (!done) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            overlay_draw(dpy, overlay, surface, screen_w, screen_h,
                         dragging, sx, sy, cx, cy);
            break;

        case ButtonPress:
            if (ev.xbutton.button == 1) {
                sx = ev.xbutton.x;
                sy = ev.xbutton.y;
                cx = sx;
                cy = sy;
                dragging = true;
            } else if (ev.xbutton.button == 3) {
                done = true;
            }
            break;

        case MotionNotify:
            if (dragging) {
                cx = ev.xmotion.x;
                cy = ev.xmotion.y;
                overlay_draw(dpy, overlay, surface, screen_w, screen_h,
                             true, sx, sy, cx, cy);
            }
            break;

        case ButtonRelease:
            if (ev.xbutton.button == 1 && dragging) {
                cx = ev.xbutton.x;
                cy = ev.xbutton.y;
                dragging = false;
                done = true;

                int rx = sx < cx ? sx : cx;
                int ry = sy < cy ? sy : cy;
                int rw = abs(cx - sx);
                int rh = abs(cy - sy);

                if (rw > 20 && rh > 20) {
                    result.x = rx;
                    result.y = ry;
                    result.width = rw;
                    result.height = rh;
                    result.valid = true;
                }
            }
            break;

        case KeyPress: {
            KeySym key = XLookupKeysym(&ev.xkey, 0);
            if (key == XK_Escape) {
                done = true;
            }
            break;
        }
        }
    }

    cairo_surface_destroy(surface);
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XFreeCursor(dpy, crosshair);
    XDestroyWindow(dpy, overlay);
    XFreeColormap(dpy, cmap);
    XCloseDisplay(dpy);

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Flutter method channel
// ─────────────────────────────────────────────────────────────────────────────

struct _ScreenRecorderPlugin {
    GObject parent_instance;
};

G_DEFINE_TYPE(ScreenRecorderPlugin, screen_recorder_plugin, g_object_get_type())

static void screen_recorder_plugin_handle_method_call(
    ScreenRecorderPlugin* self,
    FlMethodCall* method_call) {

    g_autoptr(FlMethodResponse) response = NULL;
    const gchar* method = fl_method_call_get_name(method_call);

    if (strcmp(method, "checkPermissions") == 0 ||
        strcmp(method, "requestPermissions") == 0) {
        char* missing = check_runtime_deps();
        if (missing) {
            fprintf(stderr, "[recorder] %s", missing);
            response = FL_METHOD_RESPONSE(
                fl_method_error_response_new("MISSING_DEPS", missing, NULL));
            g_free(missing);
        } else {
            response = FL_METHOD_RESPONSE(
                fl_method_success_response_new(fl_value_new_bool(TRUE)));
        }

    } else if (strcmp(method, "selectRegion") == 0) {
        SelectedRegion region = do_select_region();
        if (region.valid) {
            g_autoptr(FlValue) result_map = fl_value_new_map();
            fl_value_set_string_take(result_map, "x", fl_value_new_int(region.x));
            fl_value_set_string_take(result_map, "y", fl_value_new_int(region.y));
            fl_value_set_string_take(result_map, "width", fl_value_new_int(region.width));
            fl_value_set_string_take(result_map, "height", fl_value_new_int(region.height));
            response = FL_METHOD_RESPONSE(fl_method_success_response_new(result_map));
        } else {
            response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_null()));
        }

    } else if (strcmp(method, "startRecording") == 0) {
        FlValue* args = fl_method_call_get_args(method_call);
        if (!args || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
            response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                "INVALID_ARGS", "Expected map", NULL));
            fl_method_call_respond(method_call, response, NULL);
            return;
        }

        int x = (int)fl_value_get_int(fl_value_lookup_string(args, "x"));
        int y = (int)fl_value_get_int(fl_value_lookup_string(args, "y"));
        int w = (int)fl_value_get_int(fl_value_lookup_string(args, "width"));
        int h = (int)fl_value_get_int(fl_value_lookup_string(args, "height"));
        const gchar* path = fl_value_get_string(fl_value_lookup_string(args, "outputPath"));

        char err[512] = {0};
        if (recorder_start(x, y, w, h, path, err)) {
            response = FL_METHOD_RESPONSE(fl_method_success_response_new(NULL));
        } else {
            response = FL_METHOD_RESPONSE(
                fl_method_error_response_new("START_FAILED", err, NULL));
        }

    } else if (strcmp(method, "stopRecording") == 0) {
        const char* path = recorder_stop();
        if (path) {
            response = FL_METHOD_RESPONSE(
                fl_method_success_response_new(fl_value_new_string(path)));
        } else {
            response = FL_METHOD_RESPONSE(fl_method_success_response_new(NULL));
        }
    } else {
        response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
    }

    fl_method_call_respond(method_call, response, NULL);
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                            gpointer user_data) {
    ScreenRecorderPlugin* plugin = SCREEN_RECORDER_PLUGIN(user_data);
    screen_recorder_plugin_handle_method_call(plugin, method_call);
}

static void screen_recorder_plugin_dispose(GObject* object) {
    G_OBJECT_CLASS(screen_recorder_plugin_parent_class)->dispose(object);
}

static void screen_recorder_plugin_class_init(ScreenRecorderPluginClass* klass) {
    G_OBJECT_CLASS(klass)->dispose = screen_recorder_plugin_dispose;
}

static void screen_recorder_plugin_init(ScreenRecorderPlugin* self) {}

void screen_recorder_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
    ScreenRecorderPlugin* plugin = SCREEN_RECORDER_PLUGIN(
        g_object_new(screen_recorder_plugin_get_type(), NULL));

    g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
    g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
        fl_plugin_registrar_get_messenger(registrar),
        CHANNEL_NAME,
        FL_METHOD_CODEC(codec));

    fl_method_channel_set_method_call_handler(
        channel, method_call_cb,
        g_object_ref(plugin), g_object_unref);

    g_object_unref(plugin);
}
