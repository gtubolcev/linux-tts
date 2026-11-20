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
 *    <Prefix>FirstChunk, <Prefix>MaxModels, Debug, TextNormalization..., AudioCopyDir); the
 *    rest goes to the engine;
 *  - after <Prefix>IdleTimeout seconds without speech the models are
 *    unloaded and the normalizer stopped.
 *
 * An engine fills a TtsEngine named tts_engine and registers its voices
 * with tts_voice_add() in its init callback.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef TTS_MODULE_H
#define TTS_MODULE_H

#include <glib.h>
#include <speech-dispatcher/spd_module_main.h>

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

typedef struct
{
  const char *name;          /* "sd_vosk", for the log */
  const char *prefix;        /* "Vosk": VoskVoice, VoskIdleTimeout... */
  const char *alt_prefix;    /* another prefix of the same keys; may be NULL */
  int max_models;            /* default of <Prefix>MaxModels: models kept
                              * loaded at once (0 means 1) */
  int idle_timeout;          /* default of <Prefix>IdleTimeout, s; 0 never */
  int first_chunk;           /* default of <Prefix>FirstChunk, characters */

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
  /* at QUIT after the worker stopped; may be NULL */
  void (*close) (void);
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
gboolean tts_audio_s16 (const gint16 *samples, gsize n, int sample_rate);

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
/* the text of a CHAR or KEY message ("space" -> " ", "shift_a" -> "shift a") */
gchar *tts_key_text (const gchar *data, SPDMessageType type);

#endif
