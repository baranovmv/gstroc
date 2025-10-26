project('gst_rocsend', 'c',
  version : '1.0',
  meson_version : '>=0.56.0'
)



roc_dep = dependency('roc', required : true, static : false, method : 'pkg-config')
gstreamer_dep = dependency('gstreamer-1.0', required : true, method : 'pkg-config')
gstbase_dep = dependency('gstreamer-base-1.0', required : true, method : 'pkg-config')
gstaudio_dep = dependency('gstreamer-audio-1.0', required : true, method : 'pkg-config')
gstrtp_dep = dependency('gstreamer-rtp-1.0', required : true, method : 'pkg-config')

srcs = files('src/gstrocsend.c', 'src/common.c')
shared_library('gstrocsend',
  srcs, dependencies : [gstreamer_dep, gstbase_dep, gstaudio_dep, gstrtp_dep, roc_dep],
  install : true,
  install_dir : join_paths(get_option('libdir'), 'gstreamer-1.0')
)
