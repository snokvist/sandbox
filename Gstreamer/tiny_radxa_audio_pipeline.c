#include <gst/gst.h>
#include <string.h>

typedef struct {
  guint16 port;
  guint plane_id;
  guint latency_ms;
  GstElement *pipeline, *rtpbin;
  GstElement *v_queue, *v_depay, *v_parse, *v_dec, *v_postq, *v_sink;
  GstElement *a_queue, *a_depay, *a_dec, *a_conv, *a_res, *a_sink;
} App;

static GstCaps* on_request_pt_map (GstElement *rtpbin, guint session, guint pt, gpointer user_data) {
  (void)session; (void)user_data;
  if (pt == 97) {
    return gst_caps_new_simple ("application/x-rtp",
                                "media", G_TYPE_STRING, "video",
                                "encoding-name", G_TYPE_STRING, "H265",
                                "clock-rate", G_TYPE_INT, 90000,
                                NULL);
  } else if (pt == 98) {
    return gst_caps_new_simple ("application/x-rtp",
                                "media", G_TYPE_STRING, "audio",
                                "encoding-name", G_TYPE_STRING, "OPUS",
                                "clock-rate", G_TYPE_INT, 48000,
                                NULL);
  }
  g_printerr ("[pt-map] Unknown PT %u\n", pt);
  return NULL;
}

static void on_pad_added (GstElement *rtpbin, GstPad *newpad, gpointer user_data) {
  App *app = (App*)user_data;
  GstCaps *caps = gst_pad_get_current_caps (newpad);
  const GstStructure *s = caps ? gst_caps_get_structure (caps, 0) : NULL;
  const gchar *media = s ? gst_structure_get_string (s, "media") : NULL;

  if (media && g_str_equal (media, "video")) {
    GstPad *sinkpad = gst_element_get_static_pad (app->v_queue, "sink");
    if (gst_pad_is_linked (sinkpad) == FALSE) {
      if (gst_pad_link (newpad, sinkpad) == GST_PAD_LINK_OK)
        g_print ("[link] video pad linked\n");
      else
        g_printerr ("[link] failed to link video pad\n");
    }
    gst_object_unref (sinkpad);
  } else if (media && g_str_equal (media, "audio")) {
    GstPad *sinkpad = gst_element_get_static_pad (app->a_queue, "sink");
    if (gst_pad_is_linked (sinkpad) == FALSE) {
      if (gst_pad_link (newpad, sinkpad) == GST_PAD_LINK_OK)
        g_print ("[link] audio pad linked\n");
      else
        g_printerr ("[link] failed to link audio pad\n");
    }
    gst_object_unref (sinkpad);
  } else {
    g_printerr ("[link] unknown media on new pad\n");
  }
  if (caps) gst_caps_unref (caps);
}

static gboolean bus_cb (GstBus *bus, GstMessage *msg, gpointer user_data) {
  (void)bus; (void)user_data;
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err=NULL; gchar *dbg=NULL;
      gst_message_parse_error (msg, &err, &dbg);
      g_printerr ("ERROR: %s\nDebug: %s\n", err->message, dbg?dbg:"none");
      g_clear_error (&err); g_free (dbg);
      g_main_loop_quit ((GMainLoop*)user_data);
      break;
    }
    case GST_MESSAGE_EOS:
      g_print ("EOS\n"); g_main_loop_quit ((GMainLoop*)user_data); break;
    default: break;
  }
  return TRUE;
}

int main (int argc, char **argv) {
  gst_init (&argc, &argv);

  App app = { .port = 5600, .plane_id = 76, .latency_ms = 8 };
  for (int i=1;i<argc;i++) {
    if (!strcmp(argv[i], "--port") && i+1<argc) app.port = (guint16)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--plane") && i+1<argc) app.plane_id = (guint)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--latency") && i+1<argc) app.latency_ms = (guint)atoi(argv[++i]);
  }

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  app.pipeline = gst_pipeline_new ("p");

  // Source (single UDP port, raw RTP)
  GstElement *udpsrc = gst_element_factory_make ("udpsrc", "src");
  g_object_set (udpsrc, "port", app.port, "buffer-size", 262144, NULL);
  // Let rtpbin/ptdemux decide PTs; caps are generic RTP:
  GstCaps *rtp_caps = gst_caps_from_string ("application/x-rtp");
  g_object_set (udpsrc, "caps", rtp_caps, NULL);
  gst_caps_unref (rtp_caps);

  // rtpbin orchestrates jitter + PT demux internally
  app.rtpbin = gst_element_factory_make ("rtpbin", "rtpbin");
  g_object_set (app.rtpbin, "latency", app.latency_ms, NULL);

  // VIDEO branch
  app.v_queue = gst_element_factory_make ("queue", "v_q");
  g_object_set (app.v_queue, "leaky", 1, "max-size-buffers", 96, "max-size-time", 0, "max-size-bytes", 0, NULL);
  app.v_depay = gst_element_factory_make ("rtph265depay", "v_depay");
  app.v_parse = gst_element_factory_make ("h265parse", "v_parse");
  g_object_set (app.v_parse, "config-interval", -1, "disable-passthrough", TRUE, NULL);
  app.v_dec   = gst_element_factory_make ("mppvideodec", "v_dec");
  app.v_postq = gst_element_factory_make ("queue", "v_postq");
  g_object_set (app.v_postq, "leaky", 1, "max-size-buffers", 4, "max-size-time", 0, "max-size-bytes", 0, NULL);
  app.v_sink  = gst_element_factory_make ("kmssink", "v_sink");
  g_object_set (app.v_sink, "plane-id", app.plane_id, "sync", TRUE, NULL);

  // AUDIO branch
  app.a_queue = gst_element_factory_make ("queue", "a_q");
  g_object_set (app.a_queue, "leaky", 1, "max-size-buffers", 96, "max-size-time", 0, "max-size-bytes", 0, NULL);
  app.a_depay = gst_element_factory_make ("rtpopusdepay", "a_depay");
  app.a_dec   = gst_element_factory_make ("opusdec", "a_dec");
  app.a_conv  = gst_element_factory_make ("audioconvert", "a_conv");
  app.a_res   = gst_element_factory_make ("audioresample", "a_res");
  app.a_sink  = gst_element_factory_make ("alsasink", "a_sink");
  g_object_set (app.a_sink, "sync", FALSE, "async", FALSE, NULL);

  gst_bin_add_many (GST_BIN (app.pipeline),
    udpsrc, app.rtpbin,
    app.v_queue, app.v_depay, app.v_parse, app.v_dec, app.v_postq, app.v_sink,
    app.a_queue, app.a_depay, app.a_dec, app.a_conv, app.a_res, app.a_sink,
    NULL);

  // Link udpsrc -> rtpbin.recv_rtp_sink_0
  GstPad *rtp_sink = gst_element_get_request_pad (app.rtpbin, "recv_rtp_sink_0");
  GstPad *udp_src  = gst_element_get_static_pad (udpsrc, "src");
  if (gst_pad_link (udp_src, rtp_sink) != GST_PAD_LINK_OK) {
    g_printerr ("Failed to link udpsrc->rtpbin\n"); return 1;
  }
  gst_object_unref (udp_src);
  gst_object_unref (rtp_sink);

  // Static (downstream) links for decode chains
  if (!gst_element_link_many (app.v_queue, app.v_depay, app.v_parse, app.v_dec, app.v_postq, app.v_sink, NULL)) {
    g_printerr ("Failed to link video chain\n"); return 1;
  }
  if (!gst_element_link_many (app.a_queue, app.a_depay, app.a_dec, app.a_conv, app.a_res, app.a_sink, NULL)) {
    g_printerr ("Failed to link audio chain\n"); return 1;
  }

  // Signals
  g_signal_connect (app.rtpbin, "request-pt-map", G_CALLBACK (on_request_pt_map), &app);
  g_signal_connect (app.rtpbin, "pad-added",      G_CALLBACK (on_pad_added),     &app);

  // Bus
  GstBus *bus = gst_element_get_bus (app.pipeline);
  gst_bus_add_watch (bus, bus_cb, loop);
  gst_object_unref (bus);

  gst_element_set_state (app.pipeline, GST_STATE_PLAYING);
  g_print ("Listening on UDP %u, KMS plane %u, latency %u ms\n", app.port, app.plane_id, app.latency_ms);
  g_main_loop_run (loop);

  gst_element_set_state (app.pipeline, GST_STATE_NULL);
  gst_object_unref (app.pipeline);
  g_main_loop_unref (loop);
  return 0;
}
