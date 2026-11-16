# Firefox: let Speech Dispatcher choose the voice

`speechd-default-voice.patch` is a proposal for Firefox's speechd backend
(`dom/media/webspeech/synth/speechd/`, against mozilla-firefox/firefox
b08a9cbe6b29, 2026-11-12). Not compiled yet; the libspeechd calls were checked
with a ctypes prototype against speech-dispatcher 0.12.1.

## What is wrong now

- Firefox lists the voices of the DefaultModule only and registers none as
  default. An utterance without a voice (Reader View, most sites) gets the last
  voice of its language in hash-table order, and Firefox always sends that
  voice name to speechd. So LanguageDefaultModule and the modules' voice per
  language are never used, and other modules are unreachable.
  Bugs 1821875, 1269297, 1656695.
- `AddVoice(..., voice->mName.EqualsLiteral("default"), true)` passes the
  "is default" guess as aLocalService: every speechd voice reports
  `localService == false`. Bug 1840267 (separate one-line patch).

## The patch

- One voice "Speech Dispatcher" per language that some module speaks (the
  languages of all modules), registered with SetDefaultVoice, so
  FindVoiceByLang picks it for an utterance without a voice.
- It is spoken on a second connection that never sets a synthesis voice:
  before each utterance SET OUTPUT_MODULE back to the DefaultModule (read
  with spd_get_output_module at start; it also clears the voice), then
  SET LANGUAGE, which switches to LanguageDefaultModule when one is
  configured. The module then uses its own voice for the language.
- Voices chosen explicitly by a page work as before, on the first connection.
- New libspeechd symbols: spd_set_language, spd_set_output_module,
  spd_get_output_module, spd_list_modules, free_spd_modules, free_spd_voices.
  All exist in 0.8.2, the minimum Firefox checks for.
- SPDVoice fields share one allocation (strsep), so the lists are freed
  with free_spd_voices, not field by field.

Not in this patch (possible follow-ups): voices of all modules like Chromium's
tts_linux.cc (with espeak "+variant" voices filtered: 14,805 of espeak-ng's
voices are variants), the localService fix.

## Draft comment for bug 1821875

> Speech Dispatcher already knows which module and voice to use per language
> (DefaultModule, LanguageDefaultModule, each module's voice for a language),
> but the speechd backend never lets it decide: it lists the voices of the
> default module only, registers no default voice, and always sends an
> explicit voice name. The attached patch adds one "Speech Dispatcher" voice
> per language, marked default, which sends only the language on a separate
> connection; explicitly chosen voices are unchanged. Only libspeechd calls
> present since 0.8.2 are used.

## Draft comment for bug 1840267 (speechd-local-service.patch)

> The speechd backend passes `voice->mName.EqualsLiteral("default")` as
> aLocalService, so every Speech Dispatcher voice reports
> `localService == false`, although Speech Dispatcher synthesizes on the
> local machine. The attached one-line patch passes true.

Both patches were written with an AI assistant and are not compiled; the
comments say so. Apply the local-service one first: the default-voice patch
touches the line next to it.
