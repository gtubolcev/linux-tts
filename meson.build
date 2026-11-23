project('speech-dispatcher-modules', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
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
# the data of the C normalizer rn (cmorphy3-ru.bin, en-ipa.tsv)
norm_data = get_option('prefix') / get_option('datadir') / 'speech-dispatcher-modules' / 'textnorm'
add_project_arguments('-DTTS_NORM_DATA="@0@"'.format(norm_data), language: 'c')
# zyphra-textnorm for the other languages, loaded with dlopen when present
zt_lib = get_option('zt_lib')
if zt_lib == ''
  zt_lib = get_option('prefix') / get_option('libdir') / 'linux-tts' / 'libzyphra_textnorm.so.0'
endif
add_project_arguments('-DTTS_NORM_ZT_LIB="@0@"'.format(zt_lib), language: 'c')
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

# rates above ModelMaxSpeed: the audio is sped up with sonic; without it the
# model does the whole speed
sonic = cc.find_library('sonic', required: get_option('sonic'), has_headers: ['sonic.h'])
if sonic.found()
  sonic = declare_dependency(dependencies: sonic, compile_args: '-DHAVE_SONIC')
endif
module_deps = [speechd_module, glib, math, threads, sonic, cc.find_library('dl', required: false)]

# text normalization: rn, the C port of ru-normalizr (textnorm/), in the
# modules; else ru-normalizr through a resident Python helper; the modules pass
# text through unchanged when neither works
pcre2 = dependency('libpcre2-32', required: get_option('textnorm'))
rn_dep = dependency('', required: false)
if pcre2.found()
  rn_lib = static_library('rn',
    files('textnorm/cmorphy3/cmorphy3.c', 'textnorm/num2words/n2w-ru.c',
      'textnorm/rn/rn-abbr.c', 'textnorm/rn/rn-abbrx.c', 'textnorm/rn/rn-address.c',
      'textnorm/rn/rn-api.c', 'textnorm/rn/rn-caps.c', 'textnorm/rn/rn-dates.c',
      'textnorm/rn/rn-dic.c', 'textnorm/rn/rn-final.c', 'textnorm/rn/rn-ident.c',
      'textnorm/rn/rn-lat.c', 'textnorm/rn/rn-math.c', 'textnorm/rn/rn-morph.c',
      'textnorm/rn/rn-mx.c', 'textnorm/rn/rn-ncard.c', 'textnorm/rn/rn-ncase.c',
      'textnorm/rn/rn-ndec.c', 'textnorm/rn/rn-nord.c', 'textnorm/rn/rn-norm.c',
      'textnorm/rn/rn-npost.c', 'textnorm/rn/rn-nrng.c', 'textnorm/rn/rn-nrun.c',
      'textnorm/rn/rn-nsuf.c', 'textnorm/rn/rn-nsym.c', 'textnorm/rn/rn-num.c',
      'textnorm/rn/rn-numbering.c', 'textnorm/rn/rn-nx.c', 'textnorm/rn/rn-pat.c',
      'textnorm/rn/rn-ph.c', 'textnorm/rn/rn-phone.c', 'textnorm/rn/rn-phones.c',
      'textnorm/rn/rn-pre.c', 'textnorm/rn/rn-protect.c', 'textnorm/rn/rn-pystr.c',
      'textnorm/rn/rn-re.c', 'textnorm/rn/rn-refs.c', 'textnorm/rn/rn-roman.c',
      'textnorm/rn/rn-so.c', 'textnorm/rn/rn-stage.c', 'textnorm/rn/rn-tech.c',
      'textnorm/rn/rn-tables.c', 'textnorm/rn/rn-unitform.c',
      'textnorm/rn/rn-units.c', 'textnorm/rn/rn-urls.c', 'textnorm/rn/rn-years.c'),
    c_args: ['-Wno-comment', '-Wno-sign-compare', '-Wno-missing-field-initializers'],
    dependencies: [pcre2, threads],
    pic: true)
  rn_dep = declare_dependency(link_with: rn_lib, compile_args: '-DHAVE_RN',
    include_directories: include_directories('textnorm/rn'), dependencies: [pcre2, threads])
endif
module_deps += rn_dep
norm_src = files('common/tts-norm.c', 'common/user-stress.c', 'common/tts-copy.c', 'common/tts-config.c')
# the stress stage after it (Vosk dictionary; homographs later)
stress_src = files('common/tts-stress.c', 'common/ru-stress.c')
install_data('common/tts-normalize.py', rename: 'tts-normalize',
  install_dir: module_dir, install_mode: 'rwxr-xr-x')
test('tts-config', executable('tts-config-test', 'common/tts-config-test.c', 'common/tts-config.c',
  include_directories: common_inc, dependencies: glib))
test('user-stress', executable('user-stress-test', 'common/user-stress-test.c',
  'common/user-stress.c', include_directories: common_inc, dependencies: glib))
tts_norm_test = executable('tts-norm-test', 'common/tts-norm-test.c', norm_src, stress_src,
  include_directories: common_inc, dependencies: [glib, rn_dep, cc.find_library('dl', required: false)])
test('norm-helper', find_program('tests/norm-helper.py'),
  args: [tts_norm_test, files('common/tts-normalize.py')], timeout: 120)
if rn_dep.found()
  # needs the data in $RN_DATA (meson test --setup or env); skipped without it
  test('norm-rn', find_program('tests/norm-rn.py'), args: [tts_norm_test], timeout: 300)
endif

# the same text stages as a private library for other engines' modules
# (the PPA's sd_rhvoice loads it with dlopen)
linux_tts_text = shared_library('linux-tts-text', 'common/linux-tts-text.c', norm_src, stress_src,
  include_directories: common_inc,
  dependencies: [glib, rn_dep, cc.find_library('dl', required: false)],
  gnu_symbol_visibility: 'hidden', soversion: '0',
  link_args: '-Wl,--version-script=' + meson.current_source_dir() / 'common/linux-tts-text.map',
  link_depends: 'common/linux-tts-text.map',
  install: true, install_dir: get_option('libdir') / 'linux-tts')
test('linux-tts-text', executable('linux-tts-text-test', 'common/linux-tts-text-test.c',
  include_directories: common_inc, link_with: linux_tts_text, dependencies: glib))

# the common module frame (threads, protocol, text) and its tests with a
# fake engine
module_src = files('common/tts-module.c', 'common/tts-keys.c') + norm_src + stress_src
sd_dummy = executable('sd_dummy', 'tests/sd_dummy.c', module_src,
  include_directories: [speechd_inc, shim_inc, common_inc],
  dependencies: module_deps)
test('module-protocol', find_program('tests/module-protocol.py'), args: [sd_dummy],
  timeout: 180)

onnx_ok = json_glib.found() and onnxruntime.found()

# ---------------------------------------------------------------- vosk
if get_option('vosk').require(onnx_ok, error_message: 'sd_vosk needs json-glib and onnxruntime').allowed()
  executable('sd_vosk', 'vosk/vosk.c', 'vosk/sd_vosk.c', module_src,
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
  vulkan_ep_args = ['-DVULKAN_EP_LIBRARY="@0@"'.format(get_option('prefix') / get_option('libdir')
                    / 'linux-tts' / 'libonnxruntime_ep_vulkan.so')]
  executable('sd_kokoro', 'kokoro/espeak-g2p.c', 'kokoro/en-g2p.c', 'kokoro/en-tagger.c', 'kokoro/ru-g2p.c', 'kokoro/kokoro.c', 'kokoro/sd_kokoro.c', module_src,
    c_args: vulkan_ep_args,
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps + [json_glib, onnxruntime, espeak],
    install: true, install_dir: module_dir)
  install_data('kokoro/kokoro.conf', install_dir: conf_dir)
  # try the front end and the model by hand (see kokoro-test.c)
  executable('kokoro-test', 'kokoro/espeak-g2p.c', 'kokoro/en-g2p.c', 'kokoro/en-tagger.c', 'kokoro/ru-g2p.c', 'common/ru-stress.c', 'kokoro/kokoro.c', 'kokoro/kokoro-test.c',
    include_directories: [common_inc],
    dependencies: [glib, json_glib, onnxruntime, espeak, math])
  test('en-g2p', executable('en-g2p-test', 'kokoro/en-g2p.c', 'kokoro/en-tagger.c', 'kokoro/en-g2p-test.c',
    dependencies: [glib, math]),
    args: [meson.current_source_dir() / 'kokoro' / 'misaki'])
  test('en-tagger', executable('en-tagger-test', 'kokoro/en-tagger.c', 'kokoro/en-tagger-test.c',
    dependencies: [glib, math]),
    args: [meson.current_source_dir() / 'kokoro' / 'misaki'])
  # the same module on the graphics card: a link and its own settings
  if get_option('kokoro_vulkan').allowed()
    install_symlink('sd_kokoro-vulkan-rs', pointing_to: 'sd_kokoro', install_dir: module_dir)
    custom_target('kokoro-vulkan-rs.conf',
      input: 'kokoro/kokoro.conf', output: 'kokoro-vulkan-rs.conf',
      command: [find_program('sh'), files('kokoro/vulkan-conf.sh'), '@INPUT@', '@OUTPUT@'],
      install: true, install_dir: conf_dir)
  endif
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
  # misaki's English dictionaries (kokoro/misaki-lexicon.py) and the tagger's
  # data (kokoro/en-tagger-export.py)
  install_data('kokoro/misaki/us-gold.tsv', 'kokoro/misaki/us-silver.tsv',
    'kokoro/misaki/gb-gold.tsv', 'kokoro/misaki/gb-silver.tsv',
    'kokoro/misaki/en-tagger.bin', 'kokoro/misaki/en-tagger-norm.tsv',
    'kokoro/misaki/en-tagger-rules.tsv', 'kokoro/misaki/en-tagger-symbols.tsv',
    'kokoro/misaki/en-tagger-tokenizer.txt',
    install_dir: get_option('datadir') / 'speech-dispatcher-kokoro' / 'misaki')
endif

# ------------------------------------------------------------- teratts
if get_option('teratts').require(onnx_ok, error_message: 'sd_teratts needs json-glib and onnxruntime').allowed()
  executable('sd_teratts', 'teratts/teratts.c', 'teratts/tera-text.c', 'common/num-words.c', 'teratts/sd_teratts.c', module_src,
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
  executable('sd_sherpa-onnx', 'sherpa/sd_sherpa.c', module_src,
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
  executable('sd_silero', 'silero/sd_silero.c', module_src,
    include_directories: [speechd_inc, shim_inc, common_inc],
    dependencies: module_deps,
    install: true, install_dir: module_dir)
  install_data('silero/silero.conf', install_dir: conf_dir)
  install_data('silero/silero-server', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
  install_data('silero/silero_onnx.sha256',
               install_dir: get_option('datadir') / 'speech-dispatcher-silero')
endif

# ------------------------------------------------------------ audiocpp
# libaudiocpp (audio.cpp, Apache-2.0): upstream has no pkg-config file;
# the private build of audiocpp-tts has one (libaudiocpp-tts-dev, under
# /usr/lib/<triplet>/linux-tts/pkgconfig), and sd_audiocpp then gets its
# libdir as RUNPATH. Otherwise audiocpp_dir is its prefix (include/audiocpp.h,
# lib/libaudiocpp.so) or its source and build tree, and audiocpp_rpath
# the RUNPATH if the library is not in the default paths.
audiocpp_ok = false
acpp_rpath = get_option('audiocpp_rpath')
if not get_option('audiocpp').disabled()
  acpp_root = get_option('audiocpp_dir')
  if acpp_root == ''
    acpp_dep = dependency('audiocpp', required: false)
    audiocpp_ok = acpp_dep.found()
    if audiocpp_ok and acpp_rpath == ''
      acpp_rpath = acpp_dep.get_variable(pkgconfig: 'libdir')
    endif
  endif
  if not audiocpp_ok
    acpp_inc_dirs = acpp_root != '' ? [acpp_root / 'include'] : []
    acpp_lib_dirs = acpp_root != '' ? [acpp_root / 'lib', acpp_root / 'bin'] : []
    acpp_lib = cc.find_library('audiocpp', dirs: acpp_lib_dirs, required: false)
    acpp_args = []
    foreach d : acpp_inc_dirs
      acpp_args += '-I' + d
    endforeach
    audiocpp_ok = acpp_lib.found() and cc.has_header('audiocpp.h', args: acpp_args)
    if audiocpp_ok
      acpp_dep = declare_dependency(dependencies: acpp_lib, compile_args: acpp_args)
    endif
  endif
endif
if get_option('audiocpp').require(audiocpp_ok,
    error_message: 'sd_audiocpp needs libaudiocpp (libaudiocpp-tts-dev, or set -Daudiocpp_dir)').allowed()
  # Kokoro's phonemes from sd_kokoro's front end (phonemizer=kokoro)
  acpp_src = ['audiocpp/sd_audiocpp.c']
  acpp_deps = [acpp_dep]
  acpp_cargs = []
  if espeak.found()
    acpp_src += ['kokoro/espeak-g2p.c', 'kokoro/en-g2p.c', 'kokoro/en-tagger.c',
                 'kokoro/ru-g2p.c']
    acpp_deps += [espeak]
    acpp_cargs += ['-DAUDIOCPP_KOKORO']
  endif
  # TeraTTS's text front end (phonemizer=teratts)
  if json_glib.found()
    acpp_src += ['teratts/tera-text.c', 'common/num-words.c']
    acpp_deps += [json_glib]
    acpp_cargs += ['-DAUDIOCPP_TERATTS']
  endif
  executable('sd_audiocpp', acpp_src, module_src,
    include_directories: [speechd_inc, shim_inc, common_inc, include_directories('kokoro', 'teratts')],
    c_args: acpp_cargs,
    dependencies: module_deps + acpp_deps,
    install_rpath: acpp_rpath,
    install: true, install_dir: module_dir)
  install_data('audiocpp/audiocpp.conf', install_dir: conf_dir)
  # the same module on the graphics card: a link and its own settings
  install_symlink('sd_audiocpp-vulkan-rs', pointing_to: 'sd_audiocpp', install_dir: module_dir)
  custom_target('audiocpp-vulkan-rs.conf',
    input: 'audiocpp/audiocpp.conf', output: 'audiocpp-vulkan-rs.conf',
    command: [find_program('sh'), files('audiocpp/vulkan-conf.sh'), '@INPUT@', '@OUTPUT@'],
    install: true, install_dir: conf_dir)
  # the installer of the models the packages may not ship (the settings app
  # runs it): the catalogue, the converters and the model files
  acpp_share = get_option('datadir') / 'speech-dispatcher-audiocpp'
  install_data('audiocpp/audiocpp-models.py', 'audiocpp/kokoro-convert.py',
    'audiocpp/tera-convert.py', install_dir: acpp_share, install_mode: 'rwxr-xr-x')
  install_data('audiocpp/models/models.json', 'audiocpp/models/kokoro_tts.json',
    'audiocpp/models/kokoro-ru.ini', 'audiocpp/models/supertonic3.ini',
    'audiocpp/models/teratts.ini', 'audiocpp/models/teratts-model_spec.json',
    install_dir: acpp_share / 'models')
endif
