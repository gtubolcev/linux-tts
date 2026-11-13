project('linux-tts', 'c',
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

# libspeechd-dev has no .pc file for the module library: the header is
# <speech-dispatcher/spd_module_main.h>, spd_audio.h is shimmed per module
speechd_module = cc.find_library('speechd_module', has_headers: ['speech-dispatcher/spd_module_main.h'])
speechd_inc = include_directories('/usr/include/speech-dispatcher')
shim_inc = include_directories('kokoro/shim')
common_inc = include_directories('common')

module_dir = get_option('prefix') / 'lib' / 'speech-dispatcher-modules'
conf_dir = get_option('sysconfdir') / 'speech-dispatcher' / 'modules'
module_deps = [speechd_module, glib, math, threads]

onnx_ok = json_glib.found() and onnxruntime.found()

# ---------------------------------------------------------------- vosk
if get_option('vosk').require(onnx_ok, error_message: 'sd_vosk needs json-glib and onnxruntime').allowed()
  executable('sd_vosk', 'vosk/vosk.c', 'vosk/sd_vosk.c',
    include_directories: [speechd_inc, shim_inc],
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
  executable('sd_kokoro', 'kokoro/ru-g2p.c', 'common/ru-stress.c', 'kokoro/kokoro.c', 'kokoro/sd_kokoro.c',
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps + [json_glib, onnxruntime, espeak],
    install: true, install_dir: module_dir)
  install_data('kokoro/kokoro.conf', install_dir: conf_dir)
  # ru_dict from the espeak-ng master rules, which honour the stress mark
  # U+0301; the rest of espeak-ng-data is linked to the system's at install
  espeak_data = espeak.get_variable(pkgconfig: 'prefix', default_value: '/usr') / 'lib' / 'x86_64-linux-gnu' / 'espeak-ng-data'
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
  executable('sd_teratts', 'teratts/teratts.c', 'common/num-words.c', 'common/ru-stress.c', 'teratts/sd_teratts.c',
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps + [json_glib, onnxruntime],
    install: true, install_dir: module_dir)
  install_data('teratts/teratts.conf', install_dir: conf_dir)
endif

# -------------------------------------------------------------- sherpa
sherpa = cc.find_library('sherpa-onnx-c-api', has_headers: ['sherpa-onnx/c-api/c-api.h'], required: false)
if get_option('sherpa').require(sherpa.found(), error_message: 'sd_sherpa needs the sherpa-onnx C API').allowed()
  executable('sd_sherpa', 'sherpa/sd_sherpa.c',
    include_directories: [speechd_inc, shim_inc],
    dependencies: module_deps + [sherpa],
    install: true, install_dir: module_dir)
  install_data('sherpa/sherpa.conf', install_dir: conf_dir)
endif

# -------------------------------------------------------------- silero
# the module only; silero-server needs the user's Silero model (CC BY-NC-SA)
if get_option('silero').allowed()
  executable('sd_silero', 'silero/sd_silero.c',
    include_directories: [speechd_inc, shim_inc],
    dependencies: module_deps,
    install: true, install_dir: module_dir)
  install_data('silero/silero.conf', install_dir: conf_dir)
  install_data('silero/silero-server', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
endif
