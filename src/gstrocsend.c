// #include "gst/gstbuffer.h"
// #include "gst/gstinfo.h"
// #include "gst/gstmemory.h"
// #include "gst/gstpad.h"
#include "common.h"
#include "glib.h"
#include "glibconfig.h"
#include "gst/gstbuffer.h"
#include "gst/gstclock.h"
#include "gst/gstinfo.h"
#include "gst/gstpad.h"
#include <gst/audio/audio.h>
#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <roc/config.h>
#include <roc/context.h>
#include <roc/packet.h>
#include <roc/sender_encoder.h>

GST_DEBUG_CATEGORY_EXTERN(roc_toolkit_debug);
GST_DEBUG_CATEGORY_STATIC(gst_rocsend_debug);
#define GST_CAT_DEFAULT gst_rocsend_debug
#define DEFAULT_MTU 1492

#define GST_TYPE_ROCSEND (gst_rocsend_get_type())
G_DECLARE_FINAL_TYPE(GstRocSend, gst_rocsend, GST, ROCSEND, GstElement)

struct _GstRocSend {
  GstElement parent;

  /* ROC components */
  roc_context *context;
  roc_sender_encoder *encoder;
  roc_sender_config encoder_config;

  /* GStreamer pads */
  GstPad *sinkpad; /* audio/x-raw input */
  GstPad *srcpad;  /* RTP audio packets output */

  /* State */
  gboolean encoder_activated;
  GstCaps *negotiated_caps;

  /* ROC sender configuration properties */
  guint packet_encoding;
  guint64 packet_length;
  guint packet_interleaving;
  guint sample_rate;

  GstClockTime last_pts;
  GstClockTime last_dts;

  guint32 prev_timestamp;
  gboolean prev_timestamp_valid;
};

G_DEFINE_TYPE(GstRocSend, gst_rocsend, GST_TYPE_ELEMENT)

enum {
  PROP_0,
  PROP_PACKET_ENCODING,
  PROP_PACKET_LENGTH,
  PROP_PACKET_INTERLEAVING,
};

static void gst_rocsend_set_property(GObject *object, guint prop_id,
                                     const GValue *value, GParamSpec *pspec) {
  GstRocSend *self = GST_ROCSEND(object);
  switch (prop_id) {
  case PROP_PACKET_ENCODING:
    self->packet_encoding = g_value_get_uint(value);
    break;
  case PROP_PACKET_LENGTH:
    self->packet_length = g_value_get_uint64(value);
    break;
  case PROP_PACKET_INTERLEAVING:
    self->packet_interleaving = g_value_get_uint(value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void gst_rocsend_finalize(GObject *object) {
  GstRocSend *self = GST_ROCSEND(object);

  if (self->encoder) {
    roc_sender_encoder_close(self->encoder);
    self->encoder = NULL;
  }

  if (self->context) {
    roc_context_close(self->context);
    self->context = NULL;
  }

  if (self->negotiated_caps) {
    gst_caps_unref(self->negotiated_caps);
    self->negotiated_caps = NULL;
  }

  G_OBJECT_CLASS(gst_rocsend_parent_class)->finalize(object);
}

static void gst_rocsend_get_property(GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *pspec) {
  GstRocSend *self = GST_ROCSEND(object);
  switch (prop_id) {
  case PROP_PACKET_ENCODING:
    g_value_set_uint(value, self->packet_encoding);
    break;
  case PROP_PACKET_LENGTH:
    g_value_set_uint64(value, self->packet_length);
    break;
  case PROP_PACKET_INTERLEAVING:
    g_value_set_uint(value, self->packet_interleaving);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static gboolean gst_rocsend_sink_event(GstPad *pad, GstObject *parent,
                                       GstEvent *event) {
  GstRocSend *self = GST_ROCSEND(parent);
  gboolean res = FALSE;
  GST_LOG_OBJECT(self, "Received event: %s",
                 gst_event_type_get_name(GST_EVENT_TYPE(event)));
  switch (GST_EVENT_TYPE(event)) {
  case GST_EVENT_CAPS: {
    GstCaps *caps;
    gst_event_parse_caps(event, &caps);
    GST_LOG_OBJECT(self, "Processing CAPS event");
    if (self->negotiated_caps)
      gst_caps_unref(self->negotiated_caps);
    self->negotiated_caps = gst_caps_copy(caps);

    // Parse caps to get audio format
    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *format = gst_structure_get_string(s, "format");
    gint rate = 0, channels = 0;
    gst_structure_get_int(s, "rate", &rate);
    gst_structure_get_int(s, "channels", &channels);

    // Initialize encoder config following ROC best practices
    memset(&self->encoder_config, 0, sizeof(self->encoder_config));

    // Frame encoding
    self->encoder_config.frame_encoding.rate = rate;
    self->sample_rate = rate;
    if (channels == 1)
      self->encoder_config.frame_encoding.channels = ROC_CHANNEL_LAYOUT_MONO;
    else if (channels == 2)
      self->encoder_config.frame_encoding.channels = ROC_CHANNEL_LAYOUT_STEREO;
    else {
      self->encoder_config.frame_encoding.channels =
          ROC_CHANNEL_LAYOUT_MULTITRACK;
      self->encoder_config.frame_encoding.tracks = channels;
    }

    if (g_strcmp0(format, "F32LE") == 0)
      self->encoder_config.frame_encoding.format = ROC_FORMAT_PCM_FLOAT32;
    else {
      GST_ERROR_OBJECT(
          self, "Unsupported format for ROC encoder: %s (only F32LE supported)",
          format);
      gst_event_unref(event);
      return FALSE;
    }

    GST_DEBUG_OBJECT(self,
                     "Configuring sender, channels: %d, format: %s, rate: %d",
                     channels, format, rate);
    // Apply user-configured properties
    self->encoder_config.packet_encoding = self->packet_encoding;
    self->encoder_config.packet_length = self->packet_length;
    self->encoder_config.packet_interleaving = self->packet_interleaving;
    self->encoder_config.fec_encoding = ROC_FEC_ENCODING_DISABLE;
    self->encoder_config.clock_source = ROC_CLOCK_SOURCE_EXTERNAL;

    // Initialize ROC encoder immediately after caps negotiation
    if (!self->context) {
      GST_LOG_OBJECT(self, "Opening ROC context");
      roc_context_config context_config;
      memset(&context_config, 0, sizeof(context_config));
      if (roc_context_open(&context_config, &self->context) != 0) {
        GST_ERROR_OBJECT(self, "Failed to open ROC context");
        gst_event_unref(event);
        return FALSE;
      }
    }

    // Close existing encoder if any
    if (self->encoder) {
      GST_LOG_OBJECT(self, "Closing existing encoder before re-creation");
      roc_sender_encoder_close(self->encoder);
      self->encoder = NULL;
      self->encoder_activated = FALSE;
    }

    // Create encoder
    GST_LOG_OBJECT(self, "Opening ROC sender encoder");
    if (roc_sender_encoder_open(self->context, &self->encoder_config,
                                &self->encoder) != 0) {
      GST_ERROR_OBJECT(self, "Failed to open ROC sender encoder");
      gst_event_unref(event);
      return FALSE;
    }

    // Activate RTP audio source interface
    GST_LOG_OBJECT(self, "Activating audio source interface with RTP");
    if (roc_sender_encoder_activate(self->encoder, ROC_INTERFACE_AUDIO_SOURCE,
                                    ROC_PROTO_RTP) != 0) {
      GST_ERROR_OBJECT(self, "Failed to activate audio source interface");
      roc_sender_encoder_close(self->encoder);
      self->encoder = NULL;
      gst_event_unref(event);
      return FALSE;
    }

    self->encoder_activated = TRUE;
    GST_INFO_OBJECT(self, "ROC encoder initialized and activated");

    // Create and set caps on source pad
    GstCaps *src_caps = gst_caps_new_simple(
        "application/x-rtp", "media", G_TYPE_STRING, "audio", "clock-rate",
        G_TYPE_INT, rate, "encoding-name", G_TYPE_STRING, "F32LE", NULL);

    gboolean caps_set = gst_pad_set_caps(self->srcpad, src_caps);
    gst_caps_unref(src_caps);

    if (!caps_set) {
      GST_ERROR_OBJECT(self, "Failed to set caps on source pad");
      gst_event_unref(event);
      return FALSE;
    }

    gst_event_unref(event);
    gst_event_unref(event);
    return TRUE;
  }
  case GST_EVENT_EOS:
    GST_INFO_OBJECT(self, "Received EOS event");
    res = gst_pad_push_event(self->srcpad, event);
    break;
  default:
    GST_LOG_OBJECT(self, "Passing event to default handler");
    res = gst_pad_push_event(self->srcpad, event);
    break;
  }
  return res;
}

static GstFlowReturn gst_rocsend_chain(GstPad *pad, GstObject *parent,
                                       GstBuffer *buf) {
  GstRocSend *self = GST_ROCSEND(parent);
  GstMapInfo info;
  GstFlowReturn ret = GST_FLOW_OK;
  const GstClockTime pts = GST_BUFFER_PTS(buf);
  const GstClockTime dts = GST_BUFFER_DTS(buf);
  (void)pad; /* unused */

  GST_LOG_OBJECT(self, "Received buffer %" GST_PTR_FORMAT, buf);
  if (!self->encoder_activated) {
    GST_LOG_OBJECT(self, "Encoder not activated, dropping buffer");
    gst_buffer_unref(buf);
    return GST_FLOW_OK;
  }

  if (!gst_buffer_map(buf, &info, GST_MAP_READ)) {
    GST_ERROR_OBJECT(self, "Failed to map input buffer for reading");
    gst_buffer_unref(buf);
    return GST_FLOW_ERROR;
  }

  roc_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.samples = info.data;
  frame.samples_size = info.size;

  if (roc_sender_encoder_push_frame(self->encoder, &frame) != 0) {
    GST_ERROR_OBJECT(self, "Failed to push frame to ROC encoder");
    gst_buffer_unmap(buf, &info);
    gst_buffer_unref(buf);
    return GST_FLOW_ERROR;
  }

  gst_buffer_unmap(buf, &info);
  gst_buffer_unref(buf);

  /* Pop RTP packets from encoder */
  gboolean more_packets;
  gsize out_pkt_i = 0;
  do {
    /* Allocate buffer for RTP packet (max size 2048 bytes) */
    GstBuffer *outbuf = gst_buffer_new_allocate(NULL, 2048, NULL);
    if (!gst_buffer_map(outbuf, &info, GST_MAP_WRITE)) {
      GST_ERROR_OBJECT(self, "Failed to map output buffer");
      gst_buffer_unref(outbuf);
      return GST_FLOW_ERROR;
    }

    roc_packet packet;
    packet.bytes = info.data;
    packet.bytes_size = info.size;

    more_packets =
        (roc_sender_encoder_pop_packet(
             self->encoder, ROC_INTERFACE_AUDIO_SOURCE, &packet) == 0);

    if (more_packets) {
      /* Set PTS and DTS to egress buffer based on the input buffer, samplerate
       * and RTP timestamp */
      GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
      if (gst_rtp_buffer_map(outbuf, GST_MAP_READ, &rtp)) {
        const guint32 timestamp = gst_rtp_buffer_get_timestamp(&rtp);
        const GstClockTime ts_delta =
            self->prev_timestamp_valid
                ? gst_util_uint64_scale_int((timestamp - self->prev_timestamp),
                                            GST_SECOND, self->sample_rate)
                : 0;
        self->prev_timestamp = timestamp;
        self->prev_timestamp_valid = TRUE;
        GST_LOG_OBJECT(self, "timestamp: %u,\tdelta: %" GST_TIME_FORMAT,
                       timestamp, GST_TIME_ARGS(ts_delta));
        if (G_UNLIKELY(!GST_CLOCK_TIME_IS_VALID(self->last_pts) &&
                       GST_CLOCK_TIME_IS_VALID(pts))) {
          self->last_pts = pts;
          GST_LOG_OBJECT(self, "initialize internal pts: %" GST_TIME_FORMAT,
                         GST_TIME_ARGS(pts));
        }
        if (G_UNLIKELY(!GST_CLOCK_TIME_IS_VALID(self->last_dts) &&
                       GST_CLOCK_TIME_IS_VALID(dts))) {
          self->last_dts = dts;
          GST_LOG_OBJECT(self, "initialize internal dts: %" GST_TIME_FORMAT,
                         GST_TIME_ARGS(dts));
        }
        GST_BUFFER_DURATION(outbuf) = ts_delta;
        GST_BUFFER_PTS(outbuf) = self->last_pts;
        if (out_pkt_i == 0 && GST_CLOCK_TIME_IS_VALID(self->last_pts) &&
            GST_CLOCK_TIME_IS_VALID(pts) && pts < self->last_pts) {
          GST_WARNING_OBJECT(self,
                             "PTS (%" GST_TIME_FORMAT
                             ") after repacketization by roc become "
                             "ahead of input stream ts (%" GST_TIME_FORMAT ")",
                             GST_TIME_ARGS(self->last_pts), GST_TIME_ARGS(pts));
          self->last_pts = GST_BUFFER_PTS(outbuf) = pts;
        }
        GST_BUFFER_DTS(outbuf) = self->last_dts;
        if (out_pkt_i == 0 && GST_CLOCK_TIME_IS_VALID(self->last_dts) &&
            GST_CLOCK_TIME_IS_VALID(dts) && dts < self->last_dts) {
          GST_WARNING_OBJECT(self,
                             "DTS (%" GST_TIME_FORMAT
                             ") after repacketization by roc become "
                             "ahead of input stream ts (%" GST_TIME_FORMAT ")",
                             GST_TIME_ARGS(self->last_dts), GST_TIME_ARGS(dts));
          self->last_dts = GST_BUFFER_DTS(outbuf) = dts;
        }
        out_pkt_i += 1;
        if (G_LIKELY(GST_CLOCK_TIME_IS_VALID(self->last_pts))) {
          self->last_pts += ts_delta;
        }
        if (G_LIKELY(GST_CLOCK_TIME_IS_VALID(self->last_dts))) {
          self->last_dts += ts_delta;
        }
        gst_rtp_buffer_unmap(&rtp);
      }

      gst_buffer_unmap(outbuf, &info);
      gst_buffer_resize(outbuf, 0, packet.bytes_size);

      GST_LOG("Pushing buffer %" GST_PTR_FORMAT, outbuf);
      ret = gst_pad_push(self->srcpad, outbuf);
      if (ret != GST_FLOW_OK) {
        GST_ERROR_OBJECT(self, "Failed to push RTP packet: %s",
                         gst_flow_get_name(ret));
        return ret;
      }
    } else {
      gst_buffer_unmap(outbuf, &info);
      gst_buffer_unref(outbuf);
    }
  } while (more_packets);

  GST_LOG_OBJECT(self, "Finished processing buffer");
  return GST_FLOW_OK;
}

static GstStateChangeReturn
gst_rocsend_change_state(GstElement *element, GstStateChange transition) {
  const char *transition_name = "UNKNOWN";
  switch (transition) {
  case GST_STATE_CHANGE_NULL_TO_READY:
    transition_name = "NULL_TO_READY";
    break;
  case GST_STATE_CHANGE_READY_TO_PAUSED:
    transition_name = "READY_TO_PAUSED";
    break;
  case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    transition_name = "PAUSED_TO_PLAYING";
    break;
  case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
    transition_name = "PLAYING_TO_PAUSED";
    break;
  case GST_STATE_CHANGE_PAUSED_TO_READY:
    transition_name = "PAUSED_TO_READY";
    break;
  case GST_STATE_CHANGE_READY_TO_NULL:
    transition_name = "READY_TO_NULL";
    break;
  case GST_STATE_CHANGE_NULL_TO_NULL:
    transition_name = "NULL_TO_NULL";
    break;
  case GST_STATE_CHANGE_READY_TO_READY:
    transition_name = "READY_TO_READY";
    break;
  case GST_STATE_CHANGE_PAUSED_TO_PAUSED:
    transition_name = "PAUSED_TO_PAUSED";
    break;
  case GST_STATE_CHANGE_PLAYING_TO_PLAYING:
    transition_name = "PLAYING_TO_PLAYING";
    break;
  }
  GST_LOG_OBJECT(element, "change_state called, transition: %d (%s)",
                 transition, transition_name);
  GstRocSend *self = GST_ROCSEND(element);
  GstStateChangeReturn ret = GST_ELEMENT_CLASS(gst_rocsend_parent_class)
                                 ->change_state(element, transition);
  switch (transition) {
  case GST_STATE_CHANGE_PAUSED_TO_READY:
    GST_LOG_OBJECT(self, "Transition: PAUSED_TO_READY (%d)", transition);
    // Clean up encoder when going back to READY
    if (self->encoder) {
      roc_sender_encoder_close(self->encoder);
      self->encoder = NULL;
      self->encoder_activated = FALSE;
      GST_INFO_OBJECT(self, "ROC encoder closed");
    }
    break;
  default:
    GST_LOG_OBJECT(self, "Transition: %d (%s)", transition, transition_name);
    break;
  }
  return ret;
}

// RTP/RTCP output

static void gst_rocsend_class_init(GstRocSendClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->set_property = gst_rocsend_set_property;
  gobject_class->get_property = gst_rocsend_get_property;
  gobject_class->finalize = gst_rocsend_finalize;

  roc_log_set_handler(gst_roc_log_handler, NULL);
  roc_log_set_level(ROC_LOG_TRACE);

  g_object_class_install_property(
      gobject_class, PROP_PACKET_ENCODING,
      g_param_spec_uint("packet-encoding", "Packet Encoding",
                        "ROC packet encoding (0=auto)", 0, G_MAXUINT, 0,
                        G_PARAM_READWRITE));
  g_object_class_install_property(
      gobject_class, PROP_PACKET_LENGTH,
      g_param_spec_uint64("packet-length", "Packet Length",
                          "Packet length in nanoseconds (0=default)", 0,
                          G_MAXUINT64, 0, G_PARAM_READWRITE));
  g_object_class_install_property(
      gobject_class, PROP_PACKET_INTERLEAVING,
      g_param_spec_uint("packet-interleaving", "Packet Interleaving",
                        "Enable packet interleaving (0=disabled)", 0, 1, 0,
                        G_PARAM_READWRITE));

  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(
      element_class, "ROC Sender", "Filter/Audio",
      "Sends raw audio as RTP using ROC", "Your Name <your@email.com>");

  // Define pad templates

  // Sink pad: audio input (always)
  static GstStaticPadTemplate sink_template =
      GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                              GST_STATIC_CAPS("audio/x-raw, "
                                              "format = (string) F32LE, "
                                              "rate = (int) [ 1, MAX ], "
                                              "layout = (string) interleaved, "
                                              "channels = (int) [ 1, MAX ]"));
  gst_element_class_add_pad_template(
      element_class, gst_static_pad_template_get(&sink_template));

  // Source pad: RTP audio packets (always)
  static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
      "src", GST_PAD_SRC, GST_PAD_ALWAYS,
      GST_STATIC_CAPS("application/x-rtp,media=(string)audio"));
  gst_element_class_add_pad_template(
      element_class, gst_static_pad_template_get(&src_template));

  element_class->change_state = GST_DEBUG_FUNCPTR(gst_rocsend_change_state);
}

static void gst_rocsend_init(GstRocSend *self) {
  // Create audio sink pad (always)
  self->sinkpad = gst_pad_new_from_template(
      gst_element_class_get_pad_template(
          GST_ELEMENT_CLASS(G_OBJECT_GET_CLASS(self)), "sink"),
      "sink");
  gst_pad_set_chain_function(self->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_rocsend_chain));
  gst_pad_set_event_function(self->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_rocsend_sink_event));
  gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);

  // Create RTP source pad (always)
  self->srcpad = gst_pad_new_from_template(
      gst_element_class_get_pad_template(
          GST_ELEMENT_CLASS(G_OBJECT_GET_CLASS(self)), "src"),
      "src");
  gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

  // Initialize ROC components
  self->context = NULL;
  self->encoder = NULL;
  self->encoder_activated = FALSE;

  // Initialize state
  self->negotiated_caps = NULL;

  // Initialize ROC configuration properties with defaults
  self->packet_encoding = ROC_PACKET_ENCODING_AVP_L16_STEREO;
  self->packet_length = 0;
  self->packet_interleaving = 0;

  self->last_dts = self->last_pts = GST_CLOCK_TIME_NONE;

  // Initialize encoder config with zeros (ROC best practice)
  memset(&self->encoder_config, 0, sizeof(self->encoder_config));
  self->prev_timestamp = 0;
  self->prev_timestamp_valid = FALSE;
}

static gboolean plugin_init(GstPlugin *plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_rocsend_debug, "rocsend", 0, "ROC Sender");
  GST_DEBUG_CATEGORY_INIT(roc_toolkit_debug, "roctoolkit", 0, "ROC Toolkit");

  return gst_element_register(plugin, "rocsend", GST_RANK_NONE,
                              GST_TYPE_ROCSEND);
}

#ifndef PACKAGE
#define PACKAGE "gst_rocsend"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, rocsend,
                  "ROC Sender Plugin", plugin_init, "1.0", "LGPL", PACKAGE,
                  "https://your.project.url")
