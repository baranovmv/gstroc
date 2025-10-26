#include "common.h"
#include "gst/gstinfo.h"

GST_DEBUG_CATEGORY(roc_toolkit_debug);
#define GST_CAT_DEFAULT gst_rocsend_debug

static inline roc_log_level gst_roc_log_level_gst_2_roc (const GstDebugLevel gst_log_level)
{
	if (gst_log_level == GST_LEVEL_NONE)
    return ROC_LOG_NONE;
	else if (gst_log_level == GST_LEVEL_ERROR)
		return ROC_LOG_ERROR;
	else if (gst_log_level == GST_LEVEL_WARNING)
		return ROC_LOG_ERROR;
	else if (gst_log_level == GST_LEVEL_INFO || gst_log_level == GST_LEVEL_FIXME)
		return ROC_LOG_INFO;
	else if (gst_log_level == GST_LEVEL_DEBUG || gst_log_level == GST_LEVEL_LOG)
		return ROC_LOG_DEBUG;
	else if (gst_log_level == GST_LEVEL_TRACE)
		return ROC_LOG_TRACE;
  else
    	return ROC_LOG_NONE;
}

static inline GstDebugLevel gst_roc_log_level_roc_2_gst (const roc_log_level roc_log_level)
{
  if (roc_log_level == ROC_LOG_NONE)
    return GST_LEVEL_NONE;
  else if (roc_log_level == ROC_LOG_ERROR)
    return GST_LEVEL_ERROR;
  else if (roc_log_level == ROC_LOG_INFO)
    return GST_LEVEL_INFO;
  else if (roc_log_level == ROC_LOG_DEBUG)
    return GST_LEVEL_DEBUG;
  else if (roc_log_level == ROC_LOG_TRACE)
    return GST_LEVEL_TRACE;
  else
    return GST_LEVEL_NONE ;
}

void gst_roc_log_handler(const roc_log_message* message, void* argument)
{
  const GstDebugLevel log_level = gst_roc_log_level_roc_2_gst (message->level);
  // if (G_UNLIKELY ((log_level) <= GST_LEVEL_MAX && (log_level) <= _gst_debug_min)) {
    gst_debug_log_literal ((roc_toolkit_debug), (log_level), message->file, message->module, message->line,
        NULL, message->text);	
  // }
}
