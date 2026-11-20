/*
 * tts-keys - what to say for a CHAR or KEY message
 *
 * A screen reader sends single characters (CHAR "ё", CHAR ",") and key names
 * (KEY "control_alt_delete", KEY "kp-enter") when it echoes typing.  A neural
 * voice reads a lone letter badly and cannot read the Latin key names at all
 * (a Russian voice says nothing for "ctrl"), so they are turned into words of
 * the voice's language: the name of the letter, the punctuation mark, the key.
 * Russian, German and English; Cyrillic-only languages use the Russian words,
 * everything else the English ones.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "tts-module.h"

typedef enum { L_EN, L_RU, L_DE } Lang;

typedef struct
{
  const char *name;
  const char *en, *ru, *de;   /* NULL: use en */
} Word;

/* the key names of SSIP (the KEY command) */
static const Word keys[] = {
  { "space", "space", "пробел", "Leertaste" },
  { "underscore", "underscore", "подчёркивание", "Unterstrich" },
  { "double-quote", "double quote", "кавычка", "Anführungszeichen" },
  { "alt", "alt", "альт", "Alt" },
  { "control", "control", "контрол", "Steuerung" },
  { "hyper", "hyper", "хайпер", "Hyper" },
  { "meta", "meta", "мета", "Meta" },
  { "shift", "shift", "шифт", "Umschalt" },
  { "super", "super", "супер", "Super" },
  { "backspace", "backspace", "бэкспейс", "Rücktaste" },
  { "break", "break", "брейк", "Pause" },
  { "delete", "delete", "делит", "Entfernen" },
  { "down", "down", "вниз", "runter" },
  { "end", "end", "энд", "Ende" },
  { "enter", "enter", "ввод", "Eingabe" },
  { "escape", "escape", "эскейп", "Escape" },
  { "home", "home", "хоум", "Pos1" },
  { "insert", "insert", "инсерт", "Einfügen" },
  { "left", "left", "влево", "links" },
  { "menu", "menu", "меню", "Menü" },
  { "next", "page down", "страница вниз", "Bild runter" },
  { "num-lock", "num lock", "нам лок", "Num-Taste" },
  { "pause", "pause", "пауза", "Pause" },
  { "print", "print", "принт скрин", "Druck" },
  { "prior", "page up", "страница вверх", "Bild hoch" },
  { "return", "return", "ввод", "Eingabe" },
  { "right", "right", "вправо", "rechts" },
  { "scroll-lock", "scroll lock", "скролл лок", "Rollen" },
  { "tab", "tab", "таб", "Tabulator" },
  { "up", "up", "вверх", "hoch" },
  { "window", "windows", "виндоус", "Windows" },
  { "kp", "keypad", "цифровой блок", "Ziffernblock" },
  { "plus", "plus", "плюс", "plus" },
  { "minus", "minus", "минус", "minus" },
  { "multiply", "times", "умножить", "mal" },
  { "divide", "divide", "разделить", "geteilt" },
  { "decimal", "decimal", "точка", "Komma" },
};

/* characters: punctuation, symbols, digits */
static const Word chars[] = {
  { " ", "space", "пробел", "Leerzeichen" },
  { ".", "dot", "точка", "Punkt" },
  { ",", "comma", "запятая", "Komma" },
  { ":", "colon", "двоеточие", "Doppelpunkt" },
  { ";", "semicolon", "точка с запятой", "Semikolon" },
  { "!", "exclamation mark", "восклицательный знак", "Ausrufezeichen" },
  { "?", "question mark", "вопросительный знак", "Fragezeichen" },
  { "-", "dash", "дефис", "Bindestrich" },
  { "–", "en dash", "тире", "Gedankenstrich" },
  { "—", "em dash", "тире", "Gedankenstrich" },
  { "_", "underscore", "подчёркивание", "Unterstrich" },
  { "'", "apostrophe", "апостроф", "Apostroph" },
  { "\"", "quote", "кавычка", "Anführungszeichen" },
  { "«", "left guillemet", "открывающая кавычка", "Anführungszeichen" },
  { "»", "right guillemet", "закрывающая кавычка", "Anführungszeichen" },
  { "„", "low quote", "кавычка", "Anführungszeichen unten" },
  { "“", "left quote", "кавычка", "Anführungszeichen oben" },
  { "”", "right quote", "кавычка", "Anführungszeichen oben" },
  { "(", "left paren", "открывающая скобка", "Klammer auf" },
  { ")", "right paren", "закрывающая скобка", "Klammer zu" },
  { "[", "left bracket", "открывающая квадратная скобка", "eckige Klammer auf" },
  { "]", "right bracket", "закрывающая квадратная скобка", "eckige Klammer zu" },
  { "{", "left brace", "открывающая фигурная скобка", "geschweifte Klammer auf" },
  { "}", "right brace", "закрывающая фигурная скобка", "geschweifte Klammer zu" },
  { "<", "less than", "меньше", "kleiner als" },
  { ">", "greater than", "больше", "größer als" },
  { "=", "equals", "равно", "gleich" },
  { "+", "plus", "плюс", "plus" },
  { "*", "star", "звёздочка", "Stern" },
  { "/", "slash", "слэш", "Schrägstrich" },
  { "\\", "backslash", "обратный слэш", "Backslash" },
  { "|", "bar", "вертикальная черта", "senkrechter Strich" },
  { "@", "at", "собака", "at" },
  { "#", "number sign", "решётка", "Raute" },
  { "$", "dollar", "доллар", "Dollar" },
  { "%", "percent", "процент", "Prozent" },
  { "^", "caret", "крышка", "Zirkumflex" },
  { "&", "ampersand", "амперсанд", "und" },
  { "~", "tilde", "тильда", "Tilde" },
  { "`", "grave", "обратный апостроф", "Gravis" },
  { "№", "number", "номер", "Nummer" },
  { "§", "section", "параграф", "Paragraf" },
  { "€", "euro", "евро", "Euro" },
  { "₽", "ruble", "рубль", "Rubel" },
  { "°", "degree", "градус", "Grad" },
  { "…", "ellipsis", "многоточие", "Auslassungspunkte" },
  { "\t", "tab", "табуляция", "Tabulator" },
  { "\n", "new line", "новая строка", "neue Zeile" },
  { "0", "zero", "ноль", "null" },
  { "1", "one", "один", "eins" },
  { "2", "two", "два", "zwei" },
  { "3", "three", "три", "drei" },
  { "4", "four", "четыре", "vier" },
  { "5", "five", "пять", "fünf" },
  { "6", "six", "шесть", "sechs" },
  { "7", "seven", "семь", "sieben" },
  { "8", "eight", "восемь", "acht" },
  { "9", "nine", "девять", "neun" },
};

/* the names of the Latin letters a..z */
static const char *const latin_en[26] = {
  "ay", "bee", "see", "dee", "ee", "eff", "gee", "aitch", "eye", "jay", "kay", "el", "em",
  "en", "oh", "pee", "cue", "ar", "ess", "tee", "you", "vee", "double you", "ex", "why", "zee"
};
static const char *const latin_ru[26] = {
  "эй", "би", "си", "ди", "и", "эф", "джи", "эйч", "ай", "джей", "кей", "эл", "эм",
  "эн", "оу", "пи", "кью", "ар", "эс", "ти", "ю", "ви", "дабл ю", "экс", "уай", "зед"
};
static const char *const latin_de[26] = {
  "a", "be", "ce", "de", "e", "eff", "ge", "ha", "i", "jott", "ka", "ell", "emm",
  "enn", "o", "pe", "ku", "err", "ess", "te", "u", "fau", "we", "iks", "üpsilon", "zett"
};

/* the names of the Russian letters а..я, then ё */
static const char *const cyr_ru[33] = {
  "а", "бэ", "вэ", "гэ", "дэ", "е", "жэ", "зэ", "и", "и краткое", "ка", "эль", "эм",
  "эн", "о", "пэ", "эр", "эс", "тэ", "у", "эф", "ха", "цэ", "че", "ша", "ща",
  "твёрдый знак", "ы", "мягкий знак", "э", "ю", "я", "ё"
};

static const struct { gunichar c; const char *de; } german[] = {
  { 0xe4, "ä" }, { 0xf6, "ö" }, { 0xfc, "ü" }, { 0xdf, "Eszett" },
};

static Lang
lang_of (const gchar *language)
{
  static const char *const cyrillic[] = { "ru", "uk", "be", "bg", "kk", "sr", "mk", NULL };

  if (language == NULL)
    return L_EN;
  for (guint i = 0; cyrillic[i] != NULL; i++)
    if (g_ascii_strncasecmp (language, cyrillic[i], 2) == 0)
      return L_RU;
  if (g_ascii_strncasecmp (language, "de", 2) == 0)
    return L_DE;
  return L_EN;
}

static const char *
word_in (const Word *w, Lang l)
{
  const char *s = l == L_RU ? w->ru : l == L_DE ? w->de : NULL;
  return s != NULL ? s : w->en;
}

/* one character: its name, or the character itself */
static void
append_char (GString *out, gunichar c, Lang l)
{
  gunichar lc = g_unichar_tolower (c);
  gchar buf[8];

  buf[g_unichar_to_utf8 (c, buf)] = '\0';
  for (guint i = 0; i < G_N_ELEMENTS (chars); i++)
    if (strcmp (chars[i].name, buf) == 0)
      {
        g_string_append (out, word_in (&chars[i], l));
        return;
      }
  if (lc >= 'a' && lc <= 'z')
    {
      const char *const *t = l == L_RU ? latin_ru : l == L_DE ? latin_de : latin_en;
      g_string_append (out, t[lc - 'a']);
      return;
    }
  if (l == L_RU && ((lc >= 0x430 && lc <= 0x44f) || lc == 0x451))
    {
      g_string_append (out, cyr_ru[lc == 0x451 ? 32 : lc - 0x430]);
      return;
    }
  if (l == L_DE)
    for (guint i = 0; i < G_N_ELEMENTS (german); i++)
      if (german[i].c == lc)
        {
          g_string_append (out, german[i].de);
          return;
        }
  buf[g_unichar_to_utf8 (lc, buf)] = '\0';
  g_string_append (out, buf);
}

static gboolean
is_key_name (const gchar *part)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    if (g_ascii_strcasecmp (keys[i].name, part) == 0)
      return TRUE;
  return FALSE;
}

/* one part of a key name: a key, a character or a function key */
static void
append_key (GString *out, const gchar *part, Lang l)
{
  if (*part == '\0')
    return;
  /* "kp-enter"; "num-lock" is a key itself */
  if (strchr (part, '-') != NULL && !is_key_name (part))
    {
      gchar **sub = g_strsplit (part, "-", -1);
      for (gchar **s = sub; *s != NULL; s++)
        append_key (out, *s, l);
      g_strfreev (sub);
      return;
    }
  if (out->len > 0)
    g_string_append_c (out, ' ');
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    if (g_ascii_strcasecmp (keys[i].name, part) == 0)
      {
        g_string_append (out, word_in (&keys[i], l));
        return;
      }
  if ((part[0] == 'f' || part[0] == 'F') && part[1] >= '1' && part[1] <= '9'
      && strspn (part + 1, "0123456789") == strlen (part + 1))
    {
      g_string_append (out, l == L_RU ? "эф " : "F");
      g_string_append (out, part + 1);
      return;
    }
  if (g_utf8_strlen (part, -1) == 1)
    {
      append_char (out, g_utf8_get_char (part), l);
      return;
    }
  g_string_append (out, part);
}

gchar *
tts_key_text (const gchar *data, SPDMessageType type, const gchar *language)
{
  Lang l = lang_of (language);
  GString *out = g_string_new (NULL);
  gchar *t = g_strdup (data);

  /* the server ends the data with a line break */
  g_strchomp (t);
  if (type == SPD_MSGTYPE_CHAR)
    {
      if (strcmp (t, "space") == 0 || *t == '\0')
        append_char (out, ' ', l);
      else if (g_utf8_validate (t, -1, NULL) && g_utf8_strlen (t, -1) == 1)
        append_char (out, g_utf8_get_char (t), l);
      else
        g_string_append (out, t);
    }
  else
    {
      /* "shift_a", "control_alt_delete"; "underscore" and "double-quote"
       * have no _ in them */
      gchar **parts = g_strsplit (g_strstrip (t), "_", -1);
      for (gchar **p = parts; *p != NULL; p++)
        append_key (out, *p, l);
      g_strfreev (parts);
      if (out->len == 0)
        append_char (out, ' ', l);
    }
  g_free (t);
  return g_string_free (out, FALSE);
}
