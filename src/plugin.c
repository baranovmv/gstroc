#include <gst/gst.h>

static gboolean plugin_init(GstPlugin *plugin);

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rocsend,
    "ROC Sender Plugin",
    plugin_init,
    "1.0",
    "LGPL",
    "gst_rocsend",
    "https://your.project.url"
)
