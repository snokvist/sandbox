// SPDX-License-Identifier: MIT
// png_loop_appsrc → pngdec → x265enc → rtph265pay → udpsink
// Continuous loop without EOS / segment restart.
//gcc -std=c11 -O2 -g main.c -o png_loop_appsrc \
//  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gio-2.0 glib-2.0)

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <glib.h>
#include <gio/gio.h>     // GMappedFile
#include <string.h>      // memcpy

typedef struct {
  GstElement *pipeline;
  GstAppSrc  *appsrc;
  GPtrArray  *frames;        // GBytes* (PNG contents)
  guint       idx;
  guint       n_frames;
  guint64     pts;           // running PTS in ns
  guint64     frame_duration;
  gboolean    playing;
} Ctx;

static gboolean push_one(Ctx *ctx) {
  if (!ctx->playing || ctx->n_frames == 0) return G_SOURCE_CONTINUE;

  const guint i = ctx->idx % ctx->n_frames;
  GBytes *bytes = (GBytes *) g_ptr_array_index(ctx->frames, i);

  gsize size = 0;
  const guint8 *data = g_bytes_get_data(bytes, &size);

  GstBuffer *buf = gst_buffer_new_allocate(NULL, size, NULL);
  GstMapInfo map;
  gst_buffer_map(buf, &map, GST_MAP_WRITE);
  memcpy(map.data, data, size);
  gst_buffer_unmap(buf, &map);

  GST_BUFFER_PTS(buf)      = ctx->pts;
  GST_BUFFER_DTS(buf)      = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buf) = ctx->frame_duration;

  ctx->pts += ctx->frame_duration;
  ctx->idx++;

  GstFlowReturn ret = gst_app_src_push_buffer(ctx->appsrc, buf);
  if (ret != GST_FLOW_OK) {
    g_printerr("push buffer failed: %s\n", gst_flow_get_name(ret));
  }
  return G_SOURCE_CONTINUE;
}

static void need_data(GstAppSrc *src, guint length, gpointer user_data) {
  (void)src; (void)length;
  push_one((Ctx*)user_data);
}

// Optional time-driven pusher (keeps cadence if downstream is greedy)
static gboolean timeout_push(gpointer user_data) {
  return push_one((Ctx*)user_data);
}

static gboolean load_pngs(Ctx *ctx, const gchar *pattern, guint start_idx, guint stop_idx) {
  ctx->frames = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);

  for (guint i = start_idx; i <= stop_idx; i++) {
    gchar *path = g_strdup_printf(pattern, i);
    GError *err = NULL;
    GMappedFile *mf = g_mapped_file_new(path, FALSE, &err);
    if (!mf) {
      g_printerr("Failed to read %s: %s\n", path, err ? err->message : "unknown");
      if (err) g_error_free(err);
      g_free(path);
      continue;
    }
    GBytes *bytes = g_mapped_file_get_bytes(mf);
    g_ptr_array_add(ctx->frames, g_bytes_ref(bytes));
    g_mapped_file_unref(mf);
    g_free(path);
  }

  ctx->n_frames = ctx->frames->len;
  if (ctx->n_frames == 0) {
    g_printerr("No frames loaded. Aborting.\n");
    return FALSE;
  }
  g_print("Loaded %u PNG frames.\n", ctx->n_frames);
  return TRUE;
}

// Plain C bus watch callback (no lambdas)
static gboolean bus_cb(GstBus *bus, GstMessage *m, gpointer user_data) {
  (void)bus; (void)user_data;
  switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_ERROR: {
      GError *err=NULL; gchar *dbg=NULL;
      gst_message_parse_error(m, &err, &dbg);
      g_printerr("ERROR: %s\n", err->message);
      if (dbg) { g_printerr("debug: %s\n", dbg); g_free(dbg); }
      g_clear_error(&err);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError *err=NULL; gchar *dbg=NULL;
      gst_message_parse_warning(m, &err, &dbg);
      g_printerr("WARNING: %s\n", err->message);
      if (dbg) { g_printerr("debug: %s\n", dbg); g_free(dbg); }
      g_clear_error(&err);
      break;
    }
    default: break;
  }
  return TRUE; // keep watching
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);

  const gchar *pattern   = "OpenIPC_intro_v2_%05u.png";
  guint start_idx        = 0;
  guint stop_idx         = 180;
  gdouble fps            = 30.0;
  const gchar *host      = "192.168.2.20";
  gint port              = 5600;

  for (int i = 1; i < argc; i++) {
    if (g_str_has_prefix(argv[i], "--pattern="))      pattern = argv[i] + 10;
    else if (g_str_has_prefix(argv[i], "--start="))   start_idx = (guint)g_ascii_strtoull(argv[i]+8, NULL, 10);
    else if (g_str_has_prefix(argv[i], "--stop="))    stop_idx  = (guint)g_ascii_strtoull(argv[i]+7, NULL, 10);
    else if (g_str_has_prefix(argv[i], "--fps="))     fps = g_ascii_strtod(argv[i]+6, NULL);
    else if (g_str_has_prefix(argv[i], "--host="))    host = argv[i] + 7;
    else if (g_str_has_prefix(argv[i], "--port="))    port = (gint)g_ascii_strtoll(argv[i]+7, NULL, 10);
  }

  Ctx ctx = {0};
  ctx.frame_duration = (guint64)((gdouble)GST_SECOND / fps);
  ctx.playing = TRUE;

  if (!load_pngs(&ctx, pattern, start_idx, stop_idx)) return 1;

  ctx.appsrc = GST_APP_SRC(gst_element_factory_make("appsrc", "src"));
  GstElement *identity1    = gst_element_factory_make("identity", "id1");
  GstElement *pngdec       = gst_element_factory_make("pngdec", "pngdec");
  GstElement *convert      = gst_element_factory_make("videoconvert", "convert");
  GstElement *videorate    = gst_element_factory_make("videorate", "vrate");
  GstElement *capsfilter   = gst_element_factory_make("capsfilter", "to_i420_30");
  GstElement *identity2    = gst_element_factory_make("identity", "id2");
  GstElement *x265enc      = gst_element_factory_make("x265enc", "x265");
  GstElement *h265parse    = gst_element_factory_make("h265parse", "h265parse");
  GstElement *pay          = gst_element_factory_make("rtph265pay", "pay");
  GstElement *udpsink      = gst_element_factory_make("udpsink", "udp");

  if (!ctx.appsrc || !identity1 || !pngdec || !convert || !videorate ||
      !capsfilter || !identity2 || !x265enc || !h265parse || !pay || !udpsink) {
    g_printerr("Failed to create one or more GStreamer elements.\n");
    return 1;
  }

  g_object_set(ctx.appsrc,
               "is-live", TRUE,
               "do-timestamp", TRUE,
               "format", GST_FORMAT_TIME,
               NULL);

  GstCaps *src_caps = gst_caps_new_simple("image/png",
                          "framerate", GST_TYPE_FRACTION, (int)fps, 1,
                          NULL);
  gst_app_src_set_caps(ctx.appsrc, src_caps);
  gst_caps_unref(src_caps);

  g_object_set(identity1, "single-segment", TRUE, NULL);
  g_object_set(identity2, "single-segment", TRUE, NULL);

  GstCaps *raw_caps = gst_caps_new_simple("video/x-raw",
                        "format", G_TYPE_STRING, "I420",
                        "framerate", GST_TYPE_FRACTION, (int)fps, 1,
                        NULL);
  g_object_set(capsfilter, "caps", raw_caps, NULL);
  gst_caps_unref(raw_caps);

  g_object_set(x265enc,
               "speed-preset", 1 /* ultrafast */,
               "tune", 4 /* zerolatency */,
               "key-int-max", 30,
               "option-string", "keyint=30:min-keyint=30:scenecut=0:open-gop=0:bframes=0:rc-lookahead=0",
               NULL);

  g_object_set(h265parse, "config-interval", 0, NULL);
  g_object_set(pay, "pt", 97, "mtu", 1200, "config-interval", 1, NULL);
  g_object_set(udpsink, "host", host, "port", port, "sync", TRUE, "async", FALSE, NULL);

  ctx.pipeline = gst_pipeline_new("png-loop-rtp");
  gst_bin_add_many(GST_BIN(ctx.pipeline),
                   GST_ELEMENT(ctx.appsrc), identity1, pngdec, convert, videorate,
                   capsfilter, identity2, x265enc, h265parse, pay, udpsink, NULL);

  if (!gst_element_link_many(GST_ELEMENT(ctx.appsrc), identity1, pngdec, convert, videorate,
                             capsfilter, identity2, x265enc, h265parse, pay, udpsink, NULL)) {
    g_printerr("Failed to link elements.\n");
    gst_object_unref(ctx.pipeline);
    return 1;
  }

  g_signal_connect(ctx.appsrc, "need-data", G_CALLBACK(need_data), &ctx);

  GstBus *bus = gst_element_get_bus(ctx.pipeline);
  gst_bus_add_watch(bus, (GstBusFunc)bus_cb, NULL);
  gst_object_unref(bus);

  if (gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to set pipeline to PLAYING\n");
    gst_object_unref(ctx.pipeline);
    return 1;
  }

  guint interval_ms = (guint)(1000.0 / fps);
  g_timeout_add(interval_ms, timeout_push, &ctx);

  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_print("Streaming to %s:%d at %.3f fps. Press Ctrl+C to quit.\n", host, port, fps);
  g_main_loop_run(loop);

  ctx.playing = FALSE;
  gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
  gst_object_unref(ctx.pipeline);
  if (ctx.frames) g_ptr_array_unref(ctx.frames);
  g_main_loop_unref(loop);
  return 0;
}
