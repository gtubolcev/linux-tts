/*
 * ru-homograph-rules - context rules for Russian homographs (see
 * ru-homograph-rules.h)
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ru-homograph-rules.h"

#include <string.h>

#define RULES_ERROR (g_quark_from_static_string ("ru-homograph-rules"))
#define NEAR 4                  /* words away, same clause */
#define FAR 6                   /* words away, any clause, when NEAR finds nothing */

/* The table.  Each line:
 *
 *   forms | sense: triggers | sense: triggers ...
 *
 * forms: the words (lower case, е for ё) the line is for.
 * sense: the start of the stressed form, through the "+" or a letter more
 *   ("р+у" = ру́ки, "рук+" = руки́); the rest of the word follows unchanged.
 * triggers, one of them is enough:
 *   word, stem*   a word (or a word starting so) up to 4 words away in the
 *                 same clause, or else up to 6 words away
 *   <t, >t        t is the word right before / after, no punctuation between
 *   t may be a class: @gen @acc (prepositions with genitive / accusative),
 *   @pl (plural modifier: мои, эти, новые), @fem (feminine oblique singular
 *   modifier: моей, этой, новой), @mgen (masculine/neuter genitive: моего,
 *   нового), @num (digits).
 * Two senses triggered: no decision, the model decides.
 *
 * Every sense must give a variant of omographs.json.gz:
 * ru-homograph-test --check-rules checks it. */
static const gchar *const builtin[] = {

  /* nouns: genitive singular vs nominative/accusative plural */
  /* feminine, the genitive stressed at the end */
  "руки | рук+: <@fem <@gen | р+у: <@pl <@acc",
  "ноги | ног+: <@fem <@gen | н+о: <@pl <@acc",
  "реки | рек+: <@fem <@gen | р+е: <@pl <@acc",
  "стены | стен+: <@fem <@gen | ст+е: <@pl <@acc",
  "воды | вод+: <@fem <@gen | в+о: <@pl <@acc",
  "горы | гор+: <@fem <@gen | г+о: <@pl <@acc",
  "головы | голов+: <@fem <@gen | г+о: <@pl <@acc",
  "главы | глав+: <@fem <@gen | гл+а: <@pl <@acc",
  "стороны | сторон+: <@fem <@gen | ст+о: <@pl <@acc",
  "земли | земл+: <@fem <@gen | з+е: <@pl <@acc",
  "страны | стран+: <@fem <@gen | стр+а: <@pl <@acc",
  "спины | спин+: <@fem <@gen | сп+и: <@pl <@acc",
  "души | душ+: <@fem <@gen | д+у: <@pl <@acc",
  "цены | цен+: <@fem <@gen | ц+е: <@pl <@acc",
  "среды | сред+: <@fem <@gen | ср+е: <@pl <@acc",
  "строки | строк+: <@fem <@gen | стр+о: <@pl <@acc",
  "игры | игр+: <@fem <@gen | +и: <@pl <@acc",
  "волны | волн+: <@fem <@gen | в+о: <@pl <@acc",
  "войны | войн+: <@fem <@gen | в+о: <@pl <@acc",
  "зимы | зим+: <@fem <@gen | з+и: <@pl <@acc",
  "доски | доск+: <@fem <@gen | д+о: <@pl <@acc",
  "глубины | глубин+: <@fem <@gen | глуб+и: <@pl <@acc",
  "высоты | высот+: <@fem <@gen | выс+о: <@pl <@acc",
  "величины | величин+: <@fem <@gen | велич+и: <@pl <@acc",
  "длины | длин+: <@fem <@gen | дл+и: <@pl <@acc",
  "частоты | частот+: <@fem <@gen | част+о: <@pl <@acc",
  "чистоты | чистот+: <@fem <@gen | чист+о: <@pl <@acc",
  "семьи | семь+: <@fem <@gen | с+е: <@pl <@acc",
  "трубы | труб+: <@fem <@gen | тр+у: <@pl <@acc",
  "дыры | дыр+: <@fem <@gen | д+ы: <@pl <@acc",
  "норы | нор+: <@fem <@gen | н+о: <@pl <@acc",
  "козы | коз+: <@fem <@gen | к+о: <@pl <@acc",
  "свечи | свеч+: <@fem <@gen | св+е: <@pl <@acc",
  "змеи | зме+: <@fem <@gen | зм+е: <@pl <@acc",
  /* neuter, the genitive stressed at the end */
  "окна | окн+: <@mgen <@gen | +о: <@pl <@acc",
  "числа | числ+: <@mgen <@gen | ч+и: <@pl <@acc",
  "письма | письм+: <@mgen <@gen | п+и: <@pl <@acc",
  "вина | вин+: <@mgen <@gen | в+и: <@pl <@acc",
  "лица | лиц+: <@mgen <@gen | л+и: <@pl <@acc",
  "кольца | кольц+: <@mgen <@gen | к+о: <@pl <@acc",
  "яйца | яйц+: <@mgen <@gen | +я: <@pl <@acc",
  "ядра | ядр+: <@mgen <@gen | +я: <@pl <@acc",
  "пятна | пятн+: <@mgen <@gen | п+я: <@pl <@acc",
  "утра | утр+: <@mgen <@gen | +у: <@pl",
  /* masculine and neuter, the genitive stressed on the stem */
  "дома | д+о: <@mgen <@gen | дом+: <@pl <@acc",
  "голоса | г+о: <@mgen <@gen | голос+: <@pl <@acc",
  "слова | сл+о: <@mgen <@gen | слов+: <@pl <@acc",
  "глаза | гл+а: <@mgen <@gen | глаз+: <@pl <@acc",
  "места | м+е: <@mgen <@gen | мест+: <@pl <@acc",
  "цвета | цв+е: <@mgen <@gen | цвет+: <@pl <@acc",
  "дела | д+е: <@mgen <@gen | дел+: <@pl <@acc",
  "тела | т+е: <@mgen <@gen | тел+: <@pl <@acc",
  "поля | п+о: <@mgen <@gen | пол+: <@pl <@acc",
  "моря | м+о: <@mgen <@gen | мор+: <@pl <@acc",
  "леса | л+е: <@mgen <@gen | лес+: <@pl <@acc",
  "берега | б+е: <@mgen <@gen | берег+: <@pl <@acc",
  "города | г+о: <@mgen <@gen | город+: <@pl <@acc",
  "века | в+е: <@mgen <@gen | век+: <@pl <@acc",
  "края | кр+а: <@mgen <@gen | кра+: <@pl <@acc",
  "острова | +о: <@mgen <@gen | остров+: <@pl <@acc",
  "облака | +о: <@mgen <@gen | облак+: <@pl <@acc",
  "вечера | в+е: <@mgen <@gen | вечер+: <@pl <@acc",
  "холода | х+о: <@mgen <@gen | холод+: <@pl <@acc",
  "номера | н+о: <@mgen <@gen | номер+: <@pl <@acc",
  "адреса | +а: <@mgen <@gen | адрес+: <@pl <@acc",
  "корпуса | к+о: <@mgen <@gen | корпус+: <@pl <@acc",
  "поезда | п+о: <@mgen <@gen | поезд+: <@pl <@acc",
  "паспорта | п+а: <@mgen <@gen | паспорт+: <@pl <@acc",
  "тома | т+о: <@mgen <@gen | том+: <@pl <@acc",
  "сорта | с+о: <@mgen <@gen | сорт+: <@pl <@acc",
  "рода | р+о: <@mgen <@gen | род+: <@pl <@acc",
  "рога | р+о: <@mgen <@gen | рог+: <@pl <@acc",
  "меха | м+е: <@mgen <@gen | мех+: <@pl <@acc",
  "снега | сн+е: <@mgen <@gen | снег+: <@pl <@acc",
  "луга | л+у: <@mgen <@gen | луг+: <@pl <@acc",
  "бока | б+о: <@mgen <@gen | бок+: <@pl <@acc",
  "веса | в+е: <@mgen <@gen | вес+: <@pl <@acc",
  "тона | т+о: <@mgen <@gen | тон+: <@pl <@acc",
  "права | пр+а: <@mgen <@gen | прав+: <@pl <@acc",
  "сердца | с+е: <@mgen <@gen | сердц+: <@pl <@acc",
  "зеркала | з+е: <@mgen <@gen | зеркал+: <@pl <@acc",
  "стада | ст+а: <@mgen <@gen | стад+: <@pl <@acc",
  /* animate: the accusative plural is the genitive form, so no @acc */
  "мастера | м+а: <@mgen <@gen | мастер+: <@pl",
  "директора | дир+е: <@mgen <@gen | директор+: <@pl",
  "доктора | д+о: <@mgen <@gen | доктор+: <@pl",
  "профессора | проф+е: <@mgen <@gen | профессор+: <@pl",

  /* two words with one spelling */
  "замок | з+а: средневеков* старинн* рыцарск* королевск* феодальн* песочн* воздушн* готическ* древн* неприступн* башн* корол* граф* барон* принц* призрак* привидени* крепост* дворц* | зам+о: висяч* навесн* дверн* врезн* кодов* амбарн* ключ* отмычк* скважин* щелкн* взлома* запер* заперт* отпер* зажиган* молни* ржав* заржав* <на <под",
  "замка замку замком замке замки замков замкам замками замках | з+а: средневеков* старинн* рыцарск* королевск* феодальн* песочн* воздушн* готическ* древн* неприступн* башн* корол* граф* барон* принц* призрак* привидени* крепост* дворц* | замк+: висяч* навесн* дверн* врезн* кодов* амбарн* ключ* отмычк* скважин* щелкн* взлома* запер* заперт* отпер* зажиган* молни* ржав* заржав* <на <под",
  "мука муки муку мукой муке | мук+: пшеничн* ржан* кукурузн* рисов* гречнев* овсян* блин* тест* пирог* хлеб* испек* испеч* сорт* мешок мешк* сахар* яйц* масл* замес* просеи* просея* килограмм* грамм* стакан* ложк* щепотк* <из | м+у: адск* душевн* мучени* страдан* совест* творчеств* ревност* выбор* крестн* смертн* родов* нечеловеческ* невыносим* танталов*",
  "орган органа органы органов органе органом органам органами органах | +о: власт* государствен* исполнительн* законодательн* местн* самоуправлен* внутренн* пищеварен* дыхани* чувств* половы* донор* пересад* надзор* следствен* правоохранительн* управлен* контрол* здравоохранен* муниципальн* федеральн* региональн* судебн* организм* | орг+а: органист* бах* концерт* музык* клавиш* токкат* фуг* костел* собор* <на",
  "хлопок | хл+о: ткан* рубаш* рубах* натуральн* выращ* сбор* урожа* египетск* узбек* волокн* плантац* сорочк* футболк* джинс* шерст* шелк* синтетик* полиэстер* процент* | хлоп+о: громк* раздал* ладош* ладон* выстрел* оглушительн* резк*",
  "хлопка хлопку хлопком хлопке | хл+о: ткан* рубаш* рубах* натуральн* выращ* сбор* урожа* египетск* узбек* волокн* плантац* сорочк* футболк* джинс* шерст* шелк* синтетик* полиэстер* процент* | хлопк+: громк* раздал* ладош* ладон* выстрел* оглушительн* резк*",
  "духи духов духами | дух+: флакон* аромат* запах* пахл* пахн* французск* женск* мужск* туалетн* парфюм* брызну* надушил* | д+у: злы* зло* добры* предк* лесн* призрак* вызыва* умерш* шаман* нечист* демон* бесплотн*",
  "белки белкам | белк+: углевод* жир* яичн* аминокисл* синтез* ферм* молочн* растительн* рацион* питани* калори* мышц* >глаз | б+е: орех* дупл* прыга* рыж* хвост* шишк* бурундук* пушист* грызун* кормушк* белочк*",
  "белок | бел+: углевод* жир* яичн* аминокисл* синтез* ферм* молочн* растительн* рацион* питани* калори* мышц* | б+е: орех* дупл* бурундук* грызун*",
  "атлас атласа атласе | атл+: шелк* ткан* плать* лент* блестящ* | +а: географ* карт* анатомическ* звездн* автомобильн* >мира",
  "ирис ириса | ир+: конфет* сладк* тянуч* жева* сливочн* кондитер* фантик* | +и: цвет* цвел* сад* клумб* лепест* букет* луковиц* болотн* садов* корневищ*",
  "острота остроты остроту остротой | острот+: зрени* нож* лезви* перц* вкус* ощущен* проблем* конфликт* слух* >ума >момента >ситуации >боли блюд* соус* | остр+о: смешн* удачн* пошл* плоск* шутк* отпустил* отпуска* рассмеш* рассмея* остроумн* каламбур* анекдот*",
  "полки полкам полках | п+о: книжн* книг* кухонн* шкаф* магазин* стеклянн* вешал* повесил* расставил* стеллаж* пыль* | полк+: солдат* войск* арми* пехотн* кавалер* гвардейск* танков* стрелков* командир* наступа* фронт* дивизи* бригад* батальон* полковник* генерал*",
  "стрелки стрелкам | стр+е: часов* часы циферблат* компас* указыва* показыва* вправо влево перевод* минутн* секундн* | стрелк+: метк* снайпер* отряд* винтов* лучн* автоматчик* полка полк полку роты взвода батальона дивизии",
  "кружки кружкам | кр+у: чай* кофе* пив* налил* налить налью нальет* чашк* керамич* фарфор* эмалированн* кипят* | кружк+: рисовани* танц* шахмат* музыкальн* литературн* посеща* записа* школьн* спортивн* творческ* >по",
  "уже | +у: <гораздо <намного <чуть <немного <значительно <куда <несколько <вдвое <втрое >чем >прежнего >обычного",
  "плачу | плач+: >за >налог* >налоги >штраф* >деньги >наличными >по | пл+а: слез* навзрыд горько рыда* >от",
  "стоит стоят | ст+о: >ли >того >денег >рубл* >доллар* >евро >копе* >тысяч* >миллион* >дорого >дешево >недорого >немало >@num | сто+: >на >у >возле >около >перед >под >над >рядом >посреди >среди >вдоль >напротив >неподвижно",
  "дорогой | дор+о: <@fem | дорог+: <мой <твой <наш <ваш <свой <очень >друг >мой >подарок >товарищ",
  "целую | цел+: >тебя >вас >его >ее >их >крепко >нежно <крепко <нежно | ц+е: >ночь >неделю >вечность >жизнь >минуту >секунду >кучу >пачку >тарелку >пиццу >буханку >банку >бутылку >зиму",
  "сорока | сорок+: <@gen <более <менее <свыше <лет >лет >рублей >минут >человек >процентов >тысяч >градусов >километров >дней >@num | сор+о: белобок* трещ* стрекот* птиц* воровк*",
  "села | сел+: <@gen <@mgen жител* окраин* | с+е: >на >за <она",
  "засыпали | засып+: сон* спать кроват* постел* устал* | зас+ы: песк* земл* яму ям* щебн* снег* листв* вопрос* подарк* цветам*",
  "жаркое | жарк+: <на подал* говядин* свинин* баранин* горшоч* приготов* картош* | ж+а: >лето >солнце >время >утро >дыхание >пламя >лето",
  "видение | в+и: >мира >ситуации >проблемы <свое <собственное <особое <художественное <стратегическое <ясное <четкое | вид+е: явил* призрач* ночн* страшн* чудн*",
  NULL
};

typedef struct
{
  gchar *sense;                 /* "рук+" */
  gchar **trig;
} Sense;

typedef struct
{
  Sense *senses;
  guint n;
} Line;

struct RuHomoRules
{
  GPtrArray *lines;             /* Line *, owned */
  GHashTable *forms;            /* form -> GPtrArray of Line * */
};

static const gchar *const gen_w[] = {
  "у", "из", "от", "до", "без", "для", "около", "возле", "мимо", "после", "кроме", "вокруг",
  "среди", "против", "вдоль", "ради", "из-за", "из-под", "напротив", "позади", "внутри", "вне",
  "с", "со", "ни", "нет", "посреди", "вместо", NULL
};
static const gchar *const acc_w[] = {
  "в", "во", "на", "за", "про", "через", "сквозь", "под", "подо", NULL
};
static const gchar *const pl_w[] = {
  "мои", "твои", "свои", "наши", "ваши", "эти", "те", "все", "обе", "оба", "такие", "какие", NULL
};
static const gchar *const fem_w[] = {
  "моей", "твоей", "своей", "нашей", "вашей", "этой", "той", "одной", "всей", "самой", NULL
};
/* -ой words that are no feminine modifiers */
static const gchar *const fem_stop[] = {
  "мой", "твой", "свой", "домой", "порой", "собой", "тобой", "мной", "ей", "ней", "чей", "сей",
  "зимой", "весной", "открой", "закрой", "прикрой", "накрой", "покрой", "укрой", "раскрой",
  "построй", "настрой", "устрой", "строй", "успокой", "спой", "пой", "вырой", "зарой", "рой", NULL
};
/* -мой words that are */
static const gchar *const fem_ok[] = { "прямой", "немой", "самой", NULL };
static const gchar *const mgen_stop[] = {
  "его", "него", "много", "немного", "долго", "строго", "дорого", "убого", "полого", "ничего",
  "никого", "чего", "кого", NULL
};

static gboolean
in (const gchar *const *list, const gchar *w)
{
  return g_strv_contains (list, w);
}

static gboolean
ends (const gchar *w, const gchar *suffix)
{
  return g_str_has_suffix (w, suffix);
}

static gboolean
class_match (const gchar *c, const gchar *w)
{
  glong len = g_utf8_strlen (w, -1);

  if (strcmp (c, "gen") == 0)
    return in (gen_w, w);
  if (strcmp (c, "acc") == 0)
    return in (acc_w, w);
  if (strcmp (c, "num") == 0)
    {
      for (const gchar *p = w; *p != '\0'; p++)
        if (!g_ascii_isdigit (*p))
          return FALSE;
      return *w != '\0';
    }
  if (strcmp (c, "pl") == 0)
    {
      static const gchar *const soft[] = { "нние", "шние", "дние", "тние", "жние", "рние", "иние", NULL };

      if (in (pl_w, w))
        return TRUE;
      if (len < 4)
        return FALSE;
      if (ends (w, "ые"))
        return TRUE;
      if (ends (w, "ие"))
        {
          gunichar k = g_utf8_get_char (g_utf8_offset_to_pointer (w, len - 3));

          if (k != 0 && g_utf8_strchr ("кгхжшчщ", -1, k) != NULL)
            return TRUE;
          for (guint i = 0; soft[i] != NULL; i++)
            if (ends (w, soft[i]))
              return TRUE;
        }
      return FALSE;
    }
  if (strcmp (c, "fem") == 0)
    {
      if (in (fem_w, w))
        return TRUE;
      if (in (fem_stop, w) || len < 4)
        return FALSE;
      if (ends (w, "мой") && !in (fem_ok, w))
        return FALSE;
      return ends (w, "ой") || ends (w, "ней") || ends (w, "шей") || ends (w, "жей");
    }
  if (strcmp (c, "mgen") == 0)
    return len >= 4 && !in (mgen_stop, w) && (ends (w, "ого") || ends (w, "его"));
  return FALSE;
}

static gboolean
known_class (const gchar *c)
{
  static const gchar *const names[] = { "gen", "acc", "num", "pl", "fem", "mgen", NULL };
  return in (names, c);
}

/* t without "<" / ">" */
static gboolean
trig_match (const gchar *t, const gchar *w)
{
  gsize n = strlen (t);

  if (*t == '@')
    return class_match (t + 1, w);
  if (n > 0 && t[n - 1] == '*')
    return strncmp (w, t, n - 1) == 0;
  return strcmp (t, w) == 0;
}

static void
line_free (gpointer p)
{
  Line *l = p;

  for (guint i = 0; i < l->n; i++)
    {
      g_free (l->senses[i].sense);
      g_strfreev (l->senses[i].trig);
    }
  g_free (l->senses);
  g_free (l);
}

/* the sense applied to form: "рук+" + "и"; NULL if it does not fit */
static gchar *
sense_form (const gchar *sense, const gchar *form)
{
  const gchar *plus = strchr (sense, '+');
  GString *bare;
  gchar *v = NULL;

  if (plus == NULL || strchr (plus + 1, '+') != NULL)
    return NULL;
  bare = g_string_new_len (sense, plus - sense);
  g_string_append (bare, plus + 1);
  if (g_str_has_prefix (form, bare->str))
    v = g_strconcat (sense, form + bare->len, NULL);
  g_string_free (bare, TRUE);
  return v;
}

void
ru_homo_rules_free (RuHomoRules *r)
{
  if (r == NULL)
    return;
  g_hash_table_unref (r->forms);
  g_ptr_array_unref (r->lines);
  g_free (r);
}

RuHomoRules *
ru_homo_rules_parse (const gchar *const *lines, GError **error)
{
  RuHomoRules *r = g_new0 (RuHomoRules, 1);

  r->lines = g_ptr_array_new_with_free_func (line_free);
  r->forms = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                    (GDestroyNotify) g_ptr_array_unref);
  for (guint ln = 0; lines[ln] != NULL; ln++)
    {
      gchar **parts = g_strsplit (lines[ln], "|", -1), **forms;
      Line *l;
      guint np = g_strv_length (parts);

      if (np < 2)
        {
          g_set_error (error, RULES_ERROR, 1, "rule %u: no sense: %s", ln + 1, lines[ln]);
          g_strfreev (parts);
          ru_homo_rules_free (r);
          return NULL;
        }
      l = g_new0 (Line, 1);
      l->senses = g_new0 (Sense, np - 1);
      g_ptr_array_add (r->lines, l);
      for (guint k = 1; k < np; k++)
        {
          gchar *colon = strchr (parts[k], ':');
          Sense *s = &l->senses[l->n++];

          if (colon != NULL)
            {
              *colon = '\0';
              s->sense = g_strstrip (g_strdup (parts[k]));
              s->trig = g_strsplit_set (g_strstrip (colon + 1), " \t", -1);
            }
          if (colon == NULL || *s->sense == '\0' || s->trig[0] == NULL || *s->trig[0] == '\0')
            {
              g_set_error (error, RULES_ERROR, 1, "rule %u: bad sense: %s", ln + 1, lines[ln]);
              g_strfreev (parts);
              ru_homo_rules_free (r);
              return NULL;
            }
          for (gchar **t = s->trig; *t != NULL; t++)
            {
              const gchar *c = *t + (**t == '<' || **t == '>');

              if (*c == '\0' || **t == '\0' || (*c == '@' && !known_class (c + 1)))
                {
                  g_set_error (error, RULES_ERROR, 1, "rule %u: bad trigger \"%s\"", ln + 1, *t);
                  g_strfreev (parts);
                  ru_homo_rules_free (r);
                  return NULL;
                }
            }
        }
      forms = g_strsplit_set (g_strstrip (parts[0]), " \t", -1);
      for (gchar **f = forms; *f != NULL; f++)
        {
          GPtrArray *a;

          if (**f == '\0')
            continue;
          for (guint k = 0; k < l->n; k++)
            {
              gchar *v = sense_form (l->senses[k].sense, *f);

              if (v == NULL)
                {
                  g_set_error (error, RULES_ERROR, 1, "rule %u: sense %s does not fit %s", ln + 1,
                               l->senses[k].sense, *f);
                  g_strfreev (forms);
                  g_strfreev (parts);
                  ru_homo_rules_free (r);
                  return NULL;
                }
              g_free (v);
            }
          if ((a = g_hash_table_lookup (r->forms, *f)) == NULL)
            {
              a = g_ptr_array_new ();
              g_hash_table_insert (r->forms, g_strdup (*f), a);
            }
          g_ptr_array_add (a, l);
        }
      g_strfreev (forms);
      g_strfreev (parts);
    }
  return r;
}

RuHomoRules *
ru_homo_rules_builtin (void)
{
  static gsize once;
  static RuHomoRules *r;

  if (g_once_init_enter (&once))
    {
      GError *error = NULL;

      if ((r = ru_homo_rules_parse (builtin, &error)) == NULL)
        {
          g_printerr ("ru-homograph-rules: %s\n", error->message);
          g_error_free (error);
        }
      g_once_init_leave (&once, 1);
    }
  return r;
}

gboolean
ru_homo_rules_has (RuHomoRules *r, const gchar *word)
{
  return r != NULL && word != NULL && g_hash_table_contains (r->forms, word);
}

void
ru_homo_rules_foreach (RuHomoRules *r, RuHomoRulesFunc func, gpointer data)
{
  GHashTableIter it;
  gpointer key, value;

  if (r == NULL)
    return;
  g_hash_table_iter_init (&it, r->forms);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      GPtrArray *a = value;

      for (guint i = 0; i < a->len; i++)
        {
          Line *l = a->pdata[i];

          for (guint k = 0; k < l->n; k++)
            {
              gchar *v = sense_form (l->senses[k].sense, key);
              func (key, v, data);
              g_free (v);
            }
        }
    }
}

/* the words [lo, hi) of the clause around i: no punctuation crossed, at
 * most max words away */
static void
clause (const gboolean *punct, guint n, guint i, guint max, guint *lo, guint *hi)
{
  *lo = i;
  while (*lo > 0 && i - *lo < max && !punct[*lo])
    (*lo)--;
  *hi = i + 1;
  while (*hi < n && *hi - i <= max && !punct[*hi])
    (*hi)++;
}

gchar *
ru_homo_rules_decide (RuHomoRules *r, gchar **words, const gboolean *punct, guint n, guint i,
                      const gchar **why)
{
  GPtrArray *a;
  const gchar *got = NULL, *got_why = NULL;
  gboolean two = FALSE;

  if (why != NULL)
    *why = NULL;
  if (r == NULL || i >= n || (a = g_hash_table_lookup (r->forms, words[i])) == NULL)
    return NULL;
  for (int pass = 0; pass < 2 && got == NULL; pass++)
    {
      guint lo, hi;

      if (pass == 0)
        clause (punct, n, i, NEAR, &lo, &hi);
      else
        lo = i > FAR ? i - FAR : 0, hi = MIN (n, i + FAR + 1);
      for (guint li = 0; li < a->len; li++)
        {
          Line *l = a->pdata[li];

          for (guint k = 0; k < l->n; k++)
            {
              const Sense *s = &l->senses[k];

              for (gchar **t = s->trig; *t != NULL; t++)
                {
                  gboolean ok = FALSE;

                  if (**t == '<')
                    ok = i > 0 && !punct[i] && trig_match (*t + 1, words[i - 1]);
                  else if (**t == '>')
                    ok = i + 1 < n && !punct[i + 1] && trig_match (*t + 1, words[i + 1]);
                  else
                    for (guint w = lo; w < hi && !ok; w++)
                      ok = w != i && trig_match (*t, words[w]);
                  if (ok)
                    {
                      if (got != NULL && strcmp (got, s->sense) != 0)
                        two = TRUE;
                      got = s->sense;
                      got_why = *t;
                      break;
                    }
                }
            }
        }
    }
  if (got == NULL || two)
    return NULL;
  if (why != NULL)
    *why = got_why;
  return sense_form (got, words[i]);
}
