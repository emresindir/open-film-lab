gtkdep = dependency('gtk4', version: '>=4.10')
librawdep = dependency('libraw', required: true)
tiffdep = dependency('libtiff-4', required: true)
jpegdep = dependency('libjpeg', required: true)
lcmsdep = dependency('lcms2', required: true)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

extra_deps = [m_dep]
if host_machine.system() == 'darwin'
  extra_deps += [
    dependency('appleframeworks', modules: ['AppKit', 'Foundation'])
  ]
endif

inc = include_directories('.', 'core', 'io', 'ui', 'platform')

src_files = [
  'main.c',
  'core/ofl_frame.c',
  'core/ofl_negpipe.c',
  'io/ofl_rawthumb.c',
  'io/ofl_rawdecode.c',
  'io/ofl_tiffdecode.c',
  'io/ofl_export.c',
  'ui/ofl_app.c',
  'platform/ofl_app_icon.c',
]

extra_args = []
if host_machine.system() == 'windows'
  windows = import('windows')
  src_files += windows.compile_resources('platform/open-film-lab.rc')
  extra_args += ['-mwindows']
endif

executable('open-film-lab',
  src_files,
  include_directories: inc,
  dependencies: [gtkdep, librawdep, tiffdep, jpegdep, lcmsdep] + extra_deps,
  link_args: extra_args,
  install: true
)