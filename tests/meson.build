tests = [
  ['sender.c']
]

gstcheck_dep = dependency('gstreamer-check-1.0', required : true, method : 'pkg-config')
gstrtp_dep = dependency('gstreamer-rtp-1.0', required : true, method : 'pkg-config')
fsmod = import('fs')
test_defines = [
  '-UG_DISABLE_ASSERT',
  '-UG_DISABLE_CAST_CHECKS',
  '-DGST_CHECK_TEST_ENVIRONMENT_BEACON="GST_STATE_IGNORE_ELEMENTS"',
  '-DTESTFILE="' + fsmod.as_posix(meson.current_source_dir()) + '/meson.build"',
  '-DGST_DISABLE_DEPRECATED',
]

foreach t : tests
  fname = t[0]
  test_name = fname.split('.')[0].underscorify()
  exe = executable(test_name, fname,
      dependencies : [roc_plugin_dep, gstcheck_dep, gstrtp_dep], 
  )
  test(test_name, exe, timeout : 60)
endforeach
