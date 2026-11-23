/*
 * tts-module - the common part of the speech-dispatcher output modules
 *
 * Implements the module_* functions the module library (libspeechd_module)
 * calls, so that every engine gets the same protocol handling:
 *
 *  - the main thread only reads the server's commands; SPEAK queues the
 *    message and returns at once (module_speak), STOP and PAUSE only set a
 *    flag.  So the module always reacts to STOP at once, also when STOP and
 *    the next SPEAK come in one write;
 *  - a worker thread speaks the queued messages: 701 BEGIN, the audio (705)
 *    sentence by sentence, the index marks (700) in order, then 702 END,
 *    703 STOP or 704 PAUSE.  Every message gets exactly one BEGIN and one
 *    of END / STOP / PAUSE;
 *  - a loader thread loads the engine's models; the worker waits for the
 *    model of its voice but gives up as soon as the message is stopped;
 *  - the text goes through SSML parsing (index marks kept), the normalizer,
 *    the sentence splitter and optionally the audio copy (tts-norm,
 *    tts-copy);
 *  - the configuration file: the keys common to all modules
 *    (<Prefix>Voice, <Prefix>IdleTimeout, <Prefix>Preload (1 or a voice),
 *    <Prefix>FirstChunk, <Prefix>MaxModels, <Prefix>MaxRate, <Prefix>ModelMaxSpeed, Debug, TextNormalization..., AudioCopyDir); the
 *    rest goes to the engine;
 *  - after <Prefix>IdleTimeout seconds without speech the models are
 *    unloaded and the normalizer stopped.
 *
 * An engine fills a TtsEngine named tts_engine and registers its voices
 * with tts_voice_add() in its init callback.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TTS_MODULE_H
#define TTS_MODULE_H

#include <glib.h>
#include <speech-dispatcher/spd_module_main.h>

#include "ru-stress.h"

/* the voice settings of a message, taken when it is queued */
typedef struct
{
  const gchar *voice_name;   /* SET synthesis_voice; NULL: none */
  const gchar *language;     /* SET language ("ru", "en-US"); NULL: none */
  const gchar *gender;       /* from SET voice_type: "male", "female" or NULL */
  int type_index;            /* male2 -> 1 */
  int rate, pitch, volume;   /* -100..100 */
  int punctuation;           /* SET punctuation_mode: 0 none, 1 some, 2 all, 3 most */
  gboolean spelling;
} TtsSettings;

typedef struct
{
  gchar *name;       /* vosk-ru-female-1 */
  gchar *language;   /* ru */
  gchar *gender;     /* "male", "female" or NULL */
  gpointer model;    /* what the loader loads for it; NULL: nothing */
  gpointer data;     /* the engine's */
} TtsVoice;

typedef struct
{
  const gchar *text;         /* one piece (a sentence or a part) */
  TtsVoice *voice;
  const TtsSettings *settings;
  SPDMessageType type;       /* TEXT, CHAR or KEY */
  gboolean first;            /* the first piece of the message */
} TtsPiece;

/* What the text stages after the normalizer give a voice: its profile
 * (RESEARCH-text-normalization.md section 5).  The frame fills in the
 * defaults (no stress stage, mixed case) and asks the engine per message. */
typedef struct
{
  gboolean normalize;    /* numbers, dates, units ... become words
                          * (TextNormalization); FALSE: the engine's own
                          * frontend reads the text as it is */
  gboolean stress;       /* ё and stress marks from the stress dictionaries
                          * (TextStress, on by default); FALSE: the engine
                          * finds the stress itself */
  RuStressMark mark;     /* the mark the voice reads; RU_STRESS_NONE: only
                          * ё.  TextStressMark overrides it */
  gboolean lowercase;    /* the engine knows lower case only */
} TtsTextProfile;

typedef struct
{
  const char *name;          /* "sd_vosk", for the log */
  const char *prefix;        /* "Vosk": VoskVoice, VoskIdleTimeout... */
  const char *alt_prefix;    /* another prefix of the same keys; may be NULL */
  int max_models;            /* default of <Prefix>MaxModels: models kept
                              * loaded at once (0 means 1) */
  int idle_timeout;          /* default of <Prefix>IdleTimeout, s; 0 never */
  int first_chunk;           /* default of <Prefix>FirstChunk, characters */
  int max_piece;             /* longest piece given to speak(), characters;
                              * 0: TTS_MAX_PIECE.  Longer ones are cut at a
                              * comma or space (tts_cut_point) */

  /* before the configuration is read: the defaults; may be NULL */
  void (*setup) (void);
  /* an engine key of the configuration file: TRUE if it is one; may be NULL */
  gboolean (*config) (const gchar *key, const gchar *value);
  /* after the configuration: registers the voices; FALSE with *msg on
   * failure (no voices is a failure) */
  gboolean (*init) (gchar **msg);
  /* in the loader thread; may take long.  NULL: nothing to load */
  gboolean (*load) (gpointer model, GError **error);
  /* in the loader thread or in the worker when idle */
  void (*unload) (gpointer model);
  /* in the worker: synthesizes the piece and sends the audio with
   * tts_audio_float() / tts_audio_s16(), checking tts_stopped() when it
   * can.  The model of the voice is loaded.  FALSE (with *error) when it
   * failed; the message goes on with the next piece. */
  gboolean (*speak) (const TtsPiece *piece, GError **error);
  /* the language the text of a message is read in, for the normalizer;
   * NULL: the voice's */
  const gchar *(*text_language) (const TtsSettings *s, const TtsVoice *voice);
  /* the text profile of a voice (its model is loaded), in the worker; may
   * be NULL: the defaults */
  void (*text_profile) (const TtsVoice *voice, TtsTextProfile *p);
  /* at QUIT after the worker stopped; may be NULL */
  void (*close) (void);
  /* the file or directory a model is loaded from; may be NULL.  Before a
   * model is loaded for the first time (or after its files changed), a
   * child process (this program again, with LINUX_TTS_PROBE=<voice>) runs
   * init and load for it, so that a broken model that crashes or hangs the
   * engine fails the load instead of taking the module down.  The result is
   * kept in $XDG_CACHE_HOME/linux-tts/probe-<module>.ini.  NULL here or as
   * the result: no test load. */
  const gchar *(*model_path) (gpointer model);
  /* what else decides whether a model loads, for the test load's verdict:
   * the provider, a plugin's version (newly allocated); may be NULL.  The
   * program and the libraries it runs are taken into account anyway. */
  gchar *(*probe_salt) (void);
} TtsEngine;

/* the engine, defined by the module */
extern const TtsEngine tts_engine;

/* the Debug value of the configuration (0 off) */
extern int tts_debug;

#define TTS_DBG(...) do { if (tts_debug) tts_log (__VA_ARGS__); } while (0)
#define TTS_ERR(...) tts_log (__VA_ARGS__)
void tts_log (const char *fmt, ...) G_GNUC_PRINTF (1, 2);

/* in init: registers a voice; the strings are copied.  The voices are
 * listed in this order, and the first voice of a language is its default. */
TtsVoice *tts_voice_add (const gchar *name, const gchar *language, const gchar *gender,
                         gpointer model, gpointer data);
GPtrArray *tts_voices (void);

/* <Prefix>IdleTimeout, s; 0 never */
int tts_idle_timeout (void);
/* TRUE from QUIT on: unload is called for the models still loaded */
gboolean tts_closing (void);

/* in the worker: TRUE once the message being spoken was stopped or paused */
gboolean tts_stopped (void);

/* in the worker: sends audio of the current message; the volume of the
 * message is applied.  FALSE when the message was stopped (the audio was
 * dropped). */
gboolean tts_audio_float (const float *samples, gsize n, int sample_rate);
/* the speed the model should synthesize at (its speed / length_scale
 * parameter): rate -100..0 is 0.5..1 times, 0..100 is 1..<Prefix>MaxRate
 * times (default 4).  Of that the model does up to <Prefix>ModelMaxSpeed
 * (default 2, more spoils most models); tts_audio_* speeds the audio up by
 * the rest at the same pitch (sonic). */
double tts_model_speed (const TtsSettings *settings);
gboolean tts_audio_s16 (const gint16 *samples, gsize n, int sample_rate);
/* tts_audio_float for a model that synthesized at model_speed instead of
 * tts_model_speed () (1.0 for a model without a speed parameter): the audio
 * is sped up by the whole rest */
gboolean tts_audio_float_at (const float *samples, gsize n, int sample_rate,
                             double model_speed);

/* helpers for the configuration keys */
gchar *tts_unquote (gchar *s);
gboolean tts_parse_bool (const gchar *v);

/* in load: Ubuntu's onnxruntime + libonnx print hundreds of "Schema error:
 * ... already registered" lines when a model loads; harmless.  Sends stderr
 * to /dev/null unless Debug is 2 or more; returns what to pass to
 * tts_stderr_restore().  Note: stderr is process-wide, the worker's log
 * lines meanwhile are lost too. */
int tts_stderr_off (void);
void tts_stderr_restore (int saved);

/* the text functions, exported for the tests */
typedef struct
{
  gchar *text;      /* plain text up to the next mark */
  gchar *mark;      /* the mark after it, or NULL */
} TtsSegment;
/* SSML (speech-dispatcher wraps all text in <speak>) to plain text, cut at
 * every <mark name="..."/>; an array of TtsSegment (free with
 * tts_segments_free) */
GArray *tts_ssml_parse (const char *data, gsize bytes);
void tts_segments_free (GArray *segments);
/* cuts the text after . ! ? … ; and line breaks; long pieces also after
 * commas.  With first_chunk > 0 the first piece is cut at a comma already
 * after first_chunk characters. */
GPtrArray *tts_split_sentences (const gchar *text, int first_chunk);
#define TTS_MAX_PIECE 400
/* where to cut a text before character limit: after the last comma-like
 * mark (, : ; —) past limit/2, else at the last space, else at limit; an
 * offset in characters, > 0 */
glong tts_cut_point (const gchar *text, glong limit);
/* the words for a CHAR or KEY message in the voice's language (tts-keys.c):
 * CHAR "," -> "comma"/"запятая", KEY "control_alt_delete" -> "контрол альт
 * делит".  The normalizer does not see them. */
gchar *tts_key_text (const gchar *data, SPDMessageType type, const gchar *language);

#endif
