#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>

GST_START_TEST (test_simple_sin)
{
  const gsize nbuffers = 44100/1024;
  GstHarness * h = gst_harness_new_parse(" audioconvert ! " 
    "audio/x-raw,format=F32LE,rate=44100,channels=2 ! "
    "rocsend");
  gchar * src_ll = g_strdup_printf ( "audiotestsrc "
                             "wave=sine freq=440 num-buffers=%lu "
                             "samplesperbuffer=1024", nbuffers);
  gst_harness_add_src_parse (h, src_ll, FALSE);
  g_free(src_ll);
  for (gsize i = 0; i < nbuffers-1; i++) {
    GstFlowReturn r = gst_harness_push_from_src (h);
    fail_unless (r == GST_FLOW_OK);
  }
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
