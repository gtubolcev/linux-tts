project('speech-dispatcher-modules', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 0.62',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
add_project_arguments('-Wno-unused-parameter', '-Wno-unused-result', language: 'c')

glib = dependency('glib-2.0', version: '>= 2.70')
json_glib = dependency('json-glib-1.0', required: false)
onnxruntime = dependency('libonnxruntime', required: false)
math = cc.find_library('m')
threads = dependency('threads')

module_dir = get_option('prefix') / 'lib' / 'speech-dispatcher-modules'
conf_dir = get_option('sysconfdir') / 'speech-dispatcher' / 'modules'
# the text normalization helper (below); project arguments must come before
# the first target (the speechd_module fallback)
norm_helper = module_dir / 'tts-normalize'
add_project_arguments('-DTTS_NORM_HELPER="@0@"'.format(norm_helper), language: 'c')
# the user's module file is read on top of the system one (common/tts-config.c)
add_project_arguments('-DTTS_SYS_CONF_DIR="@0@"'.format(get_option('prefix') / conf_dir), language: 'c')

# libspeechd-dev has no .pc file for the module library: the header is
# <speech-dispatcher/spd_module_main.h>, spd_audio.h is shimmed per module
speechd_inc = include_directories('/usr/include/speech-dispatcher')
shim_inc = include_directories('kokoro/shim')
common_inc = include_directories('common')
speechd_module = cc.find_library('speechd_module', required: false,
  has_headers: ['speech-dispatcher/spd_module_main.h'])
if not speechd_module.found()
  # speech-dispatcher before 0.12.0 (noble has 0.12.0~rc2) does not install
  # the module library: build the copy of 0.12.1 in speechd-module/
  speechd_module = declare_dependency(
    include_directories: include_directories('speechd-module'),
    link_with: static_library('speechd_module',
      'speechd-module/module_main.c', 'speechd-module/module_readline.c',
      'speechd-module/module_process.c',
      include_directories: [include_directories('speechd-module/speech-dispatcher'),
                            speechd_inc, shim_inc],
      dependencies: threads))
endif

module_deps = [speechd_module, glib, math, threads]

# text normalization (ru-normalizr) through a resident Python helper; the
# modules pass text through unchanged when it or ru_normalizr is missing
norm_src = files('common/tts-norm.c', 'common/tts-copy.c', 'common/tts-config.c')
# the stress stage after it (Vosk dictionary; homographs later)
stress_src = files('common/tts-stress.c', 'common/ru-stress.c')
install_data('common/tts-normalize.py', rename: 'tts-normalize',
  install_dir: module_dir, install_mode: 'rwxr-xr-x')
test('tts-config', executable('tts-config-test', 'common/tts-config-test.c', 'common/tts-config.c',
  include_directories: common_inc, dependencies: glib))
executable('tts-norm-test', 'common/tts-norm-test.c', norm_src, stress_src,
  include_directories: common_inc, dependencies: glib)

onnx_ok = json_glib.found() and onnxruntime.found()

# ---------------------------------------------------------------- vosk
if get_option('vosk').require(onnx_ok, error_message: 'sd_vosk needs json-glib and onnxruntime').allowed()
  executable('sd_vosk', 'vosk/vosk.c', 'vosk/sd_vosk.c', norm_src,
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps + [json_glib, onnxruntime],
    install: true, install_dir: module_dir)
  # sorts the model's dictionary for the model package (vosk-test --sort-dictionary)
  executable('vosk-test', 'vosk/vosk.c', 'vosk/vosk-test.c',
    dependencies: [glib, json_glib, onnxruntime, math])
  install_data('vosk/vosk.conf', install_dir: conf_dir)
endif

# -------------------------------------------------------------- kokoro
espeak = dependency('espeak-ng', required: false)
espeak_prog = find_program('espeak-ng', required: false)
if get_option('kokoro').require(onnx_ok and espeak.found() and espeak_prog.found(),
    error_message: 'sd_kokoro needs json-glib, onnxruntime and espeak-ng').allowed()
  executable('sd_kokoro', 'kokoro/ru-g2p.c', 'common/ru-stress.c', 'kokoro/kokoro.c', 'kokoro/sd_kokoro.c', norm_src,
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps + [json_glib, onnxruntime, espeak],
    install: true, install_dir: module_dir)
  install_data('kokoro/kokoro.conf', install_dir: conf_dir)
  # ru_dict from the espeak-ng master rules, which honour the stress mark
  # U+0301; the rest of espeak-ng-data is linked to the system's at install
  espeak_data = espeak.get_variable(pkgconfig: 'libdir', default_value: get_option('prefix') / get_option('libdir')) / 'espeak-ng-data'
  espeak_data = get_option('espeak_data') != '' ? get_option('espeak_data') : espeak_data
  kokoro_data = get_option('datadir') / 'speech-dispatcher-kokoro' / 'espeak-ng-data'
  custom_target('ru_dict',
    input: files('kokoro/espeak-ru/ru_rules', 'kokoro/espeak-ru/ru_list', 'kokoro/espeak-ru/ru_emoji'),
    output: 'ru_dict',
    command: [find_program('kokoro/compile-ru-dict.sh'), espeak_prog, espeak_data,
              meson.current_source_dir() / 'kokoro' / 'espeak-ru', '@OUTPUT@'],
    install: true, install_dir: kokoro_data)
  meson.add_install_script('kokoro/link-espeak-data.sh', espeak_data, kokoro_data)
endif

# ------------------------------------------------------------- teratts
if get_option('teratts').require(onnx_ok, error_message: 'sd_teratts needs json-glib and onnxruntime').allowed()
  executable('sd_teratts', 'teratts/teratts.c', 'common/num-words.c', 'common/ru-stress.c', 'teratts/sd_teratts.c', norm_src,
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps + [json_glib, onnxruntime],
    install: true, install_dir: module_dir)
  install_data('teratts/teratts.conf', install_dir: conf_dir)
endif

# -------------------------------------------------------------- sherpa
# the private build of sherpa-onnx-tts (its .pc is under
# /usr/lib/<triplet>/linux-tts/pkgconfig, on PKG_CONFIG_PATH in debian/rules);
# a plain library in the default paths for local builds
sherpa = dependency('sherpa-onnx', required: false)
if not sherpa.found()
  sherpa = cc.find_library('sherpa-onnx-c-api', has_headers: ['sherpa-onnx/c-api/c-api.h'], required: false)
endif
# Supertonic, Piper, Pocket TTS and MMS in one module; espeak-ng turns the
# text of Piper voices into phonemes
if get_option('sherpa').require(sherpa.found() and espeak.found(),
    error_message: 'sd_sherpa-onnx needs the sherpa-onnx C API and espeak-ng').allowed()
  # espeak-ng < 1.52 (Ubuntu 24.04) has no espeak_TextToPhonemesWithTerminator
  # the Vulkan plugin sits next to the private libonnxruntime (onnxruntime-tts)
  sherpa_args = ['-DVULKAN_EP_LIBRARY="@0@"'.format(get_option('prefix') / get_option('libdir')
                 / 'linux-tts' / 'libonnxruntime_ep_vulkan.so')]
  if cc.has_function('espeak_TextToPhonemesWithTerminator', dependencies: espeak,
                     prefix: '#include <espeak-ng/speak_lib.h>')
    sherpa_args += '-DHAVE_ESPEAK_TERMINATOR'
  endif
  executable('sd_sherpa-onnx', 'sherpa/sd_sherpa.c', norm_src, stress_src,
    c_args: sherpa_args,
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps + [sherpa, espeak],
    install: true, install_dir: module_dir)
  install_data('sherpa/sherpa-onnx.conf', install_dir: conf_dir)
  # the same module on the graphics card: a link and its own settings
  if get_option('sherpa_vulkan').allowed()
    install_symlink('sd_sherpa-onnx-vulkan-rs', pointing_to: 'sd_sherpa-onnx',
      install_dir: module_dir)
    custom_target('sherpa-onnx-vulkan-rs.conf',
      input: 'sherpa/sherpa-onnx.conf', output: 'sherpa-onnx-vulkan-rs.conf',
      command: [find_program('sh'), files('sherpa/vulkan-conf.sh'), '@INPUT@', '@OUTPUT@'],
      install: true, install_dir: conf_dir)
  endif
endif

# -------------------------------------------------------------- silero
# the module only; silero-server needs the user's Silero model (CC BY-NC-SA)
if get_option('silero').allowed()
  executable('sd_silero', 'silero/sd_silero.c', norm_src, stress_src,
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps,
    install: true, install_dir: module_dir)
  install_data('silero/silero.conf', install_dir: conf_dir)
  install_data('silero/silero-server', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
endif
