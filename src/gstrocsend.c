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

/* Configuration state collected before encoder initialization */
typedef struct {
  /* From caps negotiation */
  gboolean caps_negotiated;
  gint rate;
  gint channels;
  roc_channel_layout channel_layout;
  gint tracks;
  roc_format format;
  roc_subformat subformat;

  /* From pad requests */
  gboolean rtcp_src_requested;
  gboolean rtcp_sink_requested;
} GstRocSendConfig;

struct _GstRocSend {
  GstElement parent;

  /* ROC components */
  roc_context *context;
  roc_sender_encoder *encoder;
  roc_sender_config encoder_config;

  /* GStreamer pads */
  GstPad *sinkpad; /* audio/x-raw input */
  GstPad *srcpad;  /* RTP audio packets output */

  /* RTCP pads */
  GstPad *rtcp_src_pad;  /* RTCP packets output (request pad) */
  GstPad *rtcp_sink_pad; /* RTCP feedback input (request pad) */

  /* Configuration state for deferred initialization */
  GstRocSendConfig config_state;

  /* State */
  gboolean encoder_activated;
  gboolean rtcp_interface_activated;
  GstCaps *negotiated_caps;

  /* ROC sender configuration properties */
  guint packet_encoding;
  guint64 packet_length;

  GstClockTime last_pts;
  GstClockTime last_dts;

  guint32 prev_timestamp;
  gboolean prev_timestamp_valid;
};

G_DEFINE_TYPE(GstRocSend, gst_rocsend, GST_TYPE_ELEMENT)

static GstStaticPadTemplate rtcp_src_factory =
    GST_STATIC_PAD_TEMPLATE("rtcp_src_%u", GST_PAD_SRC, GST_PAD_REQUEST,
                            GST_STATIC_CAPS("application/x-rtcp"));

static GstStaticPadTemplate rtcp_sink_factory =
    GST_STATIC_PAD_TEMPLATE("rtcp_sink_%u", GST_PAD_SINK, GST_PAD_REQUEST,
                            GST_STATIC_CAPS("application/x-rtcp"));

enum {
  PROP_0,
  PROP_PACKET_ENCODING,
  PROP_PACKET_LENGTH,
};

static gboolean gst_rocsend_initialize_encoder(GstRocSend *self);

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

    // Store configuration for deferred initialization
    self->config_state.rate = rate;
    self->config_state.channels = channels;

    if (channels == 1)
      self->config_state.channel_layout = ROC_CHANNEL_LAYOUT_MONO;
    else if (channels == 2)
      self->config_state.channel_layout = ROC_CHANNEL_LAYOUT_STEREO;
    else {
      self->config_state.channel_layout = ROC_CHANNEL_LAYOUT_MULTITRACK;
      self->config_state.tracks = channels;
    }

    if (g_strcmp0(format, "F32LE") == 0) {
      self->config_state.format = ROC_FORMAT_PCM;
      self->config_state.subformat = ROC_SUBFORMAT_PCM_FLOAT32_LE;
    } else {
      GST_ERROR_OBJECT(
          self, "Unsupported format for ROC encoder: %s (only F32LE supported)",
          format);
      gst_event_unref(event);
      return FALSE;
    }

    self->config_state.caps_negotiated = TRUE;
    GST_INFO_OBJECT(
        self, "Stored caps configuration: channels=%d, format=%s, rate=%d",
        channels, format, rate);

    // Initialize ROC encoder now that we have caps
    if (!self->encoder) {
      GST_INFO_OBJECT(self, "Initializing ROC encoder on CAPS event");
      if (!gst_rocsend_initialize_encoder(self)) {
        GST_ERROR_OBJECT(self, "Failed to initialize ROC encoder");
        gst_event_unref(event);
        return FALSE;
      }
    }

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

  /* Drop buffer if encoder not activated */
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

    if (more_packets && packet.duration > 0) {
      /* Set PTS and DTS to egress buffer based on the input buffer, samplerate
       * and RTP timestamp */
      GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
      if (gst_rtp_buffer_map(outbuf, GST_MAP_READ, &rtp)) {
        const guint32 timestamp = gst_rtp_buffer_get_timestamp(&rtp);
        const GstClockTime ts_delta = (GstClockTime)packet.duration;
        self->prev_timestamp = timestamp;
        self->prev_timestamp_valid = TRUE;
        GST_LOG_OBJECT(self, "timestamp: %u,\tdelta: %" GST_TIME_FORMAT
                       ", internal PTS %" GST_TIME_FORMAT
                       ", internal DTS %" GST_TIME_FORMAT,
                       timestamp, GST_TIME_ARGS(ts_delta), GST_TIME_ARGS (self->last_pts),
                       GST_TIME_ARGS (self->last_dts));
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
          GST_LOG_OBJECT (self, "update internal PTS %" GST_TIME_FORMAT, GST_TIME_ARGS (self->last_pts));
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

  /* Pop RTCP packets from encoder if RTCP source pad exists */
  GST_INFO_OBJECT(self,
                  "RTCP check: rtcp_src_pad=%p, rtcp_interface_activated=%d",
                  self->rtcp_src_pad, self->rtcp_interface_activated);
  if (self->rtcp_src_pad && self->rtcp_interface_activated) {
    GST_TRACE_OBJECT(self, "Attempting to pop RTCP packets");
    do {
      /* Allocate buffer for RTCP packet (max size 2048 bytes) */
      GstBuffer *rtcp_outbuf = gst_buffer_new_allocate(NULL, 2048, NULL);
      if (!gst_buffer_map(rtcp_outbuf, &info, GST_MAP_WRITE)) {
        GST_ERROR_OBJECT(self, "Failed to map RTCP output buffer");
        gst_buffer_unref(rtcp_outbuf);
        return GST_FLOW_ERROR;
      }

      roc_packet rtcp_packet;
      rtcp_packet.bytes = info.data;
      rtcp_packet.bytes_size = info.size;

      more_packets =
          (roc_sender_encoder_pop_packet(
               self->encoder, ROC_INTERFACE_AUDIO_CONTROL, &rtcp_packet) == 0);

      GST_TRACE_OBJECT(self, "RTCP pop_packet returned: %s, size: %zu",
                       more_packets ? "success" : "no packets",
                       rtcp_packet.bytes_size);

      if (more_packets) {
        gst_buffer_unmap(rtcp_outbuf, &info);
        gst_buffer_resize(rtcp_outbuf, 0, rtcp_packet.bytes_size);

        /* Set timestamps for RTCP packets */
        GST_BUFFER_PTS(rtcp_outbuf) = self->last_pts;
        GST_BUFFER_DTS(rtcp_outbuf) = self->last_dts;

        GST_LOG_OBJECT(self, "Pushing RTCP buffer %" GST_PTR_FORMAT,
                       rtcp_outbuf);
        ret = gst_pad_push(self->rtcp_src_pad, rtcp_outbuf);
        if (ret != GST_FLOW_OK) {
          GST_ERROR_OBJECT(self, "Failed to push RTCP packet: %s",
                           gst_flow_get_name(ret));
          return ret;
        }
      } else {
        gst_buffer_unmap(rtcp_outbuf, &info);
        gst_buffer_unref(rtcp_outbuf);
      }
    } while (more_packets);
  }

  GST_LOG_OBJECT(self, "Finished processing buffer");
  return GST_FLOW_OK;
}

/* RTCP sink pad event handler */
static gboolean gst_rocsend_rtcp_sink_event(GstPad *pad, GstObject *parent,
                                            GstEvent *event) {
  GstRocSend *self = GST_ROCSEND(parent);
  gboolean res = FALSE;

  GST_LOG_OBJECT(self, "Received RTCP sink event: %s",
                 gst_event_type_get_name(GST_EVENT_TYPE(event)));

  switch (GST_EVENT_TYPE(event)) {
  case GST_EVENT_CAPS: {
    GstCaps *caps;
    gst_event_parse_caps(event, &caps);
    GST_LOG_OBJECT(self, "RTCP sink pad CAPS event: %" GST_PTR_FORMAT, caps);
    res = TRUE;
    gst_event_unref(event);
    break;
  }
  case GST_EVENT_EOS:
    GST_INFO_OBJECT(self, "Received EOS on RTCP sink pad");
    res = TRUE;
    gst_event_unref(event);
    break;
  default:
    res = gst_pad_event_default(pad, parent, event);
    break;
  }
  return res;
}

/* RTCP sink pad chain handler - receives feedback packets from decoder */
static GstFlowReturn gst_rocsend_rtcp_sink_chain(GstPad *pad, GstObject *parent,
                                                 GstBuffer *buf) {
  GstRocSend *self = GST_ROCSEND(parent);
  guint8 *packet_data = NULL;
  gsize packet_size;
  (void)pad; /* unused */

  GST_LOG_OBJECT(self, "Received RTCP feedback buffer %" GST_PTR_FORMAT, buf);

  if (!self->encoder || !self->rtcp_interface_activated) {
    GST_DEBUG_OBJECT(self, "Encoder not ready or RTCP interface not activated, "
                           "dropping RTCP feedback packet");
    gst_buffer_unref(buf);
    return GST_FLOW_OK;
  }

  packet_size = gst_buffer_get_size(buf);
  if (packet_size == 0) {
    GST_WARNING_OBJECT(self, "Received empty RTCP feedback buffer");
    gst_buffer_unref(buf);
    return GST_FLOW_OK;
  }

  /* Extract and duplicate buffer data into contiguous memory
   * This handles buffers with memory spread across multiple chunks */
  gst_buffer_extract_dup(buf, 0, packet_size, (gpointer *)&packet_data,
                         &packet_size);
  if (!packet_data) {
    GST_ERROR_OBJECT(self, "Failed to extract RTCP feedback packet data");
    gst_buffer_unref(buf);
    return GST_FLOW_ERROR;
  }

  /* Push feedback packet to ROC encoder */
  roc_packet packet;
  packet.bytes = packet_data;
  packet.bytes_size = packet_size;

  if (roc_sender_encoder_push_feedback_packet(
          self->encoder, ROC_INTERFACE_AUDIO_CONTROL, &packet) != 0) {
    GST_WARNING_OBJECT(self,
                       "Failed to push RTCP feedback packet to ROC encoder");
  } else {
    GST_LOG_OBJECT(self,
                   "Successfully pushed RTCP feedback packet to ROC encoder");
  }

  g_free(packet_data);
  gst_buffer_unref(buf);

  return GST_FLOW_OK;
}

static GstPad *gst_rocsend_request_new_pad(GstElement *element,
                                           GstPadTemplate *templ,
                                           const gchar *req_name,
                                           const GstCaps *caps) {
  GstRocSend *self = GST_ROCSEND(element);
  GstPad *newpad = NULL;
  const gchar *templ_name;

  (void)caps; /* unused */

  templ_name = GST_PAD_TEMPLATE_NAME_TEMPLATE(templ);

  GST_DEBUG_OBJECT(self, "Requesting new pad from template: %s (req_name: %s)",
                   templ_name, req_name ? req_name : "NULL");

  if (g_str_equal(templ_name, "rtcp_src_%u")) {
    /* Create RTCP source pad for outgoing control packets */
    if (self->rtcp_src_pad) {
      GST_WARNING_OBJECT(self, "RTCP source pad already exists");
      return NULL;
    }

    newpad = gst_pad_new_from_template(templ, "rtcp_src_0");
    if (!newpad) {
      GST_ERROR_OBJECT(self, "Failed to create RTCP source pad");
      return NULL;
    }

    self->rtcp_src_pad = newpad;
    self->config_state.rtcp_src_requested = TRUE;
    GST_INFO_OBJECT(
        self,
        "Created RTCP source pad (will be activated on state transition)");

  } else if (g_str_equal(templ_name, "rtcp_sink_%u")) {
    /* Create RTCP sink pad for incoming feedback packets */
    if (self->rtcp_sink_pad) {
      GST_WARNING_OBJECT(self, "RTCP sink pad already exists");
      return NULL;
    }

    newpad = gst_pad_new_from_template(templ, "rtcp_sink_0");
    if (!newpad) {
      GST_ERROR_OBJECT(self, "Failed to create RTCP sink pad");
      return NULL;
    }

    gst_pad_set_chain_function(newpad,
                               GST_DEBUG_FUNCPTR(gst_rocsend_rtcp_sink_chain));
    gst_pad_set_event_function(newpad,
                               GST_DEBUG_FUNCPTR(gst_rocsend_rtcp_sink_event));

    self->rtcp_sink_pad = newpad;
    self->config_state.rtcp_sink_requested = TRUE;
    GST_INFO_OBJECT(
        self, "Created RTCP sink pad (will be activated on state transition)");

  } else {
    GST_WARNING_OBJECT(self, "Unknown pad template: %s", templ_name);
    return NULL;
  }

  gst_pad_set_active(newpad, TRUE);
  gst_element_add_pad(element, newpad);

  return newpad;
}

static void gst_rocsend_release_pad(GstElement *element, GstPad *pad) {
  GstRocSend *self = GST_ROCSEND(element);

  GST_DEBUG_OBJECT(self, "Releasing pad: %s", GST_PAD_NAME(pad));

  if (pad == self->rtcp_src_pad) {
    self->rtcp_src_pad = NULL;
    self->config_state.rtcp_src_requested = FALSE;
    GST_INFO_OBJECT(self, "Released RTCP source pad");
  } else if (pad == self->rtcp_sink_pad) {
    self->rtcp_sink_pad = NULL;
    self->config_state.rtcp_sink_requested = FALSE;
    GST_INFO_OBJECT(self, "Released RTCP sink pad");
  }

  gst_pad_set_active(pad, FALSE);
  gst_element_remove_pad(element, pad);
}

/* Initialize ROC encoder with collected configuration */
static gboolean gst_rocsend_initialize_encoder(GstRocSend *self) {
  GST_INFO_OBJECT(self, "Initializing ROC encoder");

  /* Initialize ROC context if not already done */
  if (!self->context) {
    GST_LOG_OBJECT(self, "Opening ROC context");
    roc_context_config context_config;
    memset(&context_config, 0, sizeof(context_config));
    if (roc_context_open(&context_config, &self->context) != 0) {
      GST_ERROR_OBJECT(self, "Failed to open ROC context");
      return FALSE;
    }
  }

  /* Close existing encoder if any */
  if (self->encoder) {
    GST_LOG_OBJECT(self, "Closing existing encoder before re-creation");
    roc_sender_encoder_close(self->encoder);
    self->encoder = NULL;
    self->encoder_activated = FALSE;
    self->rtcp_interface_activated = FALSE;
  }

  /* Build encoder config from stored configuration state */
  memset(&self->encoder_config, 0, sizeof(self->encoder_config));

  /* Frame encoding from caps */
  self->encoder_config.frame_encoding.rate = self->config_state.rate;
  self->encoder_config.frame_encoding.channels =
      self->config_state.channel_layout;
  self->encoder_config.frame_encoding.format = self->config_state.format;
  self->encoder_config.frame_encoding.subformat = self->config_state.subformat;
  if (self->config_state.channel_layout == ROC_CHANNEL_LAYOUT_MULTITRACK) {
    self->encoder_config.frame_encoding.tracks = self->config_state.tracks;
  }

  /* Apply user-configured properties */
  self->encoder_config.packet_encoding = self->packet_encoding;
  self->encoder_config.packet_length = self->packet_length;
  self->encoder_config.fec_encoding = ROC_FEC_ENCODING_DISABLE;
  self->encoder_config.clock_source = ROC_CLOCK_SOURCE_EXTERNAL;

  GST_DEBUG_OBJECT(
      self,
      "Encoder config: channels=%d, format=%d, rate=%d, packet_encoding=%d",
      self->config_state.channels, self->config_state.format,
      self->config_state.rate, self->packet_encoding);

  /* Create encoder */
  GST_LOG_OBJECT(self, "Opening ROC sender encoder");
  if (roc_sender_encoder_open(self->context, &self->encoder_config,
                              &self->encoder) != 0) {
    GST_ERROR_OBJECT(self, "Failed to open ROC sender encoder");
    return FALSE;
  }

  /* Activate RTP audio source interface */
  GST_LOG_OBJECT(self, "Activating audio source interface with RTP");
  if (roc_sender_encoder_activate(self->encoder, ROC_INTERFACE_AUDIO_SOURCE,
                                  ROC_PROTO_RTP) != 0) {
    GST_ERROR_OBJECT(self, "Failed to activate audio source interface");
    roc_sender_encoder_close(self->encoder);
    self->encoder = NULL;
    return FALSE;
  }

  self->encoder_activated = TRUE;
  GST_INFO_OBJECT(self, "Audio source interface activated");

  /* Activate RTCP interface if any RTCP pads were requested */
  if (self->config_state.rtcp_src_requested ||
      self->config_state.rtcp_sink_requested) {
    GST_LOG_OBJECT(self, "Activating RTCP control interface");
    if (roc_sender_encoder_activate(self->encoder, ROC_INTERFACE_AUDIO_CONTROL,
                                    ROC_PROTO_RTCP) != 0) {
      GST_ERROR_OBJECT(self, "Failed to activate RTCP control interface");
      roc_sender_encoder_close(self->encoder);
      self->encoder = NULL;
      self->encoder_activated = FALSE;
      return FALSE;
    }
    self->rtcp_interface_activated = TRUE;
    GST_INFO_OBJECT(self, "RTCP control interface activated");
  }

  GST_INFO_OBJECT(self, "ROC encoder successfully initialized");
  return TRUE;
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

  /* Let parent class handle the state change first */
  GstStateChangeReturn ret = GST_ELEMENT_CLASS(gst_rocsend_parent_class)
                                 ->change_state(element, transition);

  /* If parent state change failed, return immediately */
  if (ret == GST_STATE_CHANGE_FAILURE) {
    return ret;
  }

  /* Handle post-transition actions */
  switch (transition) {
  case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    GST_LOG_OBJECT(self, "Post-transition: PAUSED_TO_PLAYING");
    /* Encoder should already be initialized by CAPS event */
    if (!self->encoder) {
      GST_WARNING_OBJECT(
          self,
          "Encoder not initialized - caps may not have been received yet");
    }
    break;

  case GST_STATE_CHANGE_PAUSED_TO_READY:
    GST_LOG_OBJECT(self,
                   "Post-transition: PAUSED_TO_READY - cleaning up encoder");
    /* Clean up encoder when going back to READY */
    if (self->encoder) {
      roc_sender_encoder_close(self->encoder);
      self->encoder = NULL;
      self->encoder_activated = FALSE;
      self->rtcp_interface_activated = FALSE;
      GST_INFO_OBJECT(self, "ROC encoder closed");
    }
    break;

  default:
    GST_LOG_OBJECT(self, "Post-transition: %d (%s)", transition,
                   transition_name);
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

  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(element_class, "ROC Sender",
                                        "Filter/Audio",
                                        "Sends raw audio as RTP using ROC",
                                        "Misha Baranov <baranov.mv@gmail.com>");

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

  gst_element_class_add_pad_template(
      element_class, gst_static_pad_template_get(&rtcp_src_factory));
  gst_element_class_add_pad_template(
      element_class, gst_static_pad_template_get(&rtcp_sink_factory));
  element_class->request_new_pad = gst_rocsend_request_new_pad;
  element_class->release_pad = gst_rocsend_release_pad;

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

  // Initialize RTCP pads and state
  self->rtcp_src_pad = NULL;
  self->rtcp_sink_pad = NULL;
  self->rtcp_interface_activated = FALSE;

  // Initialize configuration state for deferred initialization
  memset(&self->config_state, 0, sizeof(GstRocSendConfig));
  self->config_state.caps_negotiated = FALSE;
  self->config_state.rtcp_src_requested = FALSE;
  self->config_state.rtcp_sink_requested = FALSE;

  // Initialize state
  self->negotiated_caps = NULL;

  // Initialize ROC configuration properties with defaults
  self->packet_encoding = ROC_PACKET_ENCODING_AVP_L16_STEREO;
  self->packet_length = 0;

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
