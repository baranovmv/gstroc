#include "gst/check/internal-check.h"
#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/rtp/gstrtpbuffer.h>

GST_START_TEST (test_simple_sin)
{
  const gsize buff_sz = 441;
  const gsize nbuffers = 44100/buff_sz;

  GstHarness * h = gst_harness_new_parse(" audioconvert ! " 
    "audio/x-raw,format=F32LE,rate=44100,channels=2 ! "
    "rocsend");
  gchar * src_ll = g_strdup_printf ( "audiotestsrc "
                             "wave=sine freq=440 num-buffers=%lu "
                             "samplesperbuffer=%lu", nbuffers, buff_sz);
  gst_harness_add_src_parse (h, src_ll, FALSE);
  g_free(src_ll);
  gsize out_nbuffs = 0;
  
  guint16 seq = 0;
  gboolean got_seq = FALSE;
  GstClockTime ts = GST_CLOCK_TIME_NONE; 
  for (gsize i = 0; i < nbuffers-1; i++) {
    GstFlowReturn r = gst_harness_push_from_src (h);
    fail_unless (r == GST_FLOW_OK);
    GstBuffer * buff = NULL;
    while (buff = gst_harness_try_pull(h)) {
      out_nbuffs++;
      GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
      if (!gst_rtp_buffer_map (buff, GST_MAP_READ, &rtp))
        g_assert_not_reached ();
      GST_DEBUG ("Got outgoing packet #%lu: %" GST_PTR_FORMAT ", seqnum: %u", out_nbuffs, buff,
                 gst_rtp_buffer_get_seq (&rtp));
      if (!got_seq) {
        seq = gst_rtp_buffer_get_seq (&rtp);
        got_seq = TRUE;
        ts = GST_BUFFER_TIMESTAMP (buff);
      } else {
        const guint16 cur_seq = gst_rtp_buffer_get_seq (&rtp);
        fail_unless_equals_int (seq + 1, cur_seq);
        seq = cur_seq;
        const GstClockTimeDiff tdiff = GST_CLOCK_DIFF (ts, GST_BUFFER_TIMESTAMP (buff));
        GST_DEBUG("tdiff: %"GST_STIME_FORMAT, GST_STIME_ARGS (tdiff));
        ts = GST_BUFFER_TIMESTAMP (buff);
        fail_unless (tdiff > 0);
        fail_unless (tdiff <= 10 * GST_MSECOND);
      }
      
      gst_rtp_buffer_unmap (&rtp);
    }
  }
  GST_DEBUG ("Sent %lu, Received %d, got outside %lu", nbuffers-1, gst_harness_buffers_received (h), out_nbuffs);
  gst_harness_teardown (h);
}
GST_END_TEST;

static Suite *
sender_suite (void)
{
  Suite *s = suite_create ("sender");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_simple_sin);

  return s;
}

GST_CHECK_MAIN (sender);
