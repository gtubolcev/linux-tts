/* Our own first pass for technical text (QUIRKS.md R27–R30), before the URL stage:
 *
 * R27 RN_FIX_TECH_NUM: a number after a Latin name or after "версия"/"ядро" is a name or
 *     a version, not a counted amount: "с GNOME 50" → "с GNOME пятьдесят" (not
 *     "пятидесяти"), "Python 3.10" → "три точка десять" (not "три целых десять сотых"),
 *     "Ubuntu 26.04" → "двадцать шесть точка ноль четыре", "ядро 7.0" → "семь точка ноль".
 *     The number is read here, in the nominative.
 * R28 RN_FIX_NO_SIGN: "No 5" (speech-dispatcher's NFKC turns "№" into "No") → "№ 5", in a
 *     text with Cyrillic letters; "No. 5" is read by the preprocess stage already.
 * R29 RN_FIX_DIV_SLASH: "10 / 2" with spaces → "10 делить на 2" (without spaces "10/2"
 *     stays a fraction or a reference).
 * R30 RN_FIX_LAT_WORDS: Latin names with an established Russian reading ("Linux" →
 *     "линукс", "GitHub" → "гитхаб", "GNOME" → "гном") from our table, outside URLs, paths
 *     and e-mail addresses; rn_tech_word() serves the same table to the latinization
 *     stage for the words inside URLs.
 */
#include <string.h>

#include "rn-num.h"
#include "rn-pass.h"
#include "rn-pystr.h"
#include "rn-util.h"

/* lower-case ASCII key → reading; names whose IPA reading comes out wrong or that the
 * caps stage would spell letter by letter */
static const struct {
	const char *key;
	const uint32_t *reading;
} WORDS[] = {
	{"android", RN_U("андроид")},
	{"chrome", RN_U("хром")},
	{"chromium", RN_U("хромиум")},
	{"com", RN_U("ком")},
	{"debian", RN_U("дебиан")},
	{"fedora", RN_U("федора")},
	{"firefox", RN_U("файрфокс")},
	{"git", RN_U("гит")},
	{"github", RN_U("гитхаб")},
	{"gitlab", RN_U("гитлаб")},
	{"gnome", RN_U("гном")},
	{"gnu", RN_U("гну")},
	{"google", RN_U("гугл")},
	{"http", RN_U("эйч ти ти пи")},
	{"https", RN_U("эйч ти ти пи эс")},
	{"iphone", RN_U("айфон")},
	{"json", RN_U("джейсон")},
	{"libreoffice", RN_U("либреофис")},
	{"linux", RN_U("линукс")},
	{"microsoft", RN_U("майкрософт")},
	{"mozilla", RN_U("мозилла")},
	{"nvidia", RN_U("энвидиа")},
	{"org", RN_U("орг")},
	{"python", RN_U("пайтон")},
	{"telegram", RN_U("телеграм")},
	{"thunderbird", RN_U("тандербёрд")},
	{"ubuntu", RN_U("убунту")},
	{"www", RN_U("вэ вэ вэ")},
	{"youtube", RN_U("ютуб")},
};

/* the reading of a lower-case ASCII word from WORDS, or NULL */
const uint32_t *rn_tech_word(const uint32_t *w, size_t n)
{
	if (!(rn_fixes & RN_FIX_LAT_WORDS) || n == 0 || n > 15)
		return NULL;
	char k[16];
	for (size_t i = 0; i < n; i++) {
		if (w[i] >= 0x80)
			return NULL;
		k[i] = (char)(w[i] | (rn_is_ascii_alpha(w[i]) ? 0x20 : 0));
	}
	k[n] = 0;
	size_t lo = 0, hi = sizeof WORDS / sizeof *WORDS;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = strcmp(k, WORDS[mid].key);
		if (!c)
			return WORDS[mid].reading;
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return NULL;
}

static int is_cyr(uint32_t c)
{
	return (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451;
}

static int word_char(uint32_t c)
{
	return rn_py_isalpha(c) || rn_py_isdecimal(c) || c == '_';
}

/* a character that joins the word to a URL, path or address */
static int link_char(uint32_t c)
{
	return c == '/' || c == '@' || c == '\\' || c == ':' || c == '_' || c == '.' || c == '=' ||
	       c == '?' || c == '&' || c == '#' || c == '%' || c == '~';
}

/* s[a:b] is a Latin name: ASCII letters, digits, "-", "+" ("Wi-Fi", "C++"), starting with
 * a letter, two letters at least, not part of a link */
static int latin_name(const uint32_t *s, size_t n, size_t a, size_t b)
{
	size_t letters = 0;
	if (!rn_is_ascii_alpha(s[a]))
		return 0;
	for (size_t i = a; i < b; i++) {
		if (rn_is_ascii_alpha(s[i]))
			letters++;
		else if (!rn_is_ascii_digit(s[i]) && s[i] != '-' && s[i] != '+')
			return 0;
	}
	if (letters < 2)
		return 0;
	if (a && (link_char(s[a - 1]) || word_char(s[a - 1])))
		return 0;
	if (b < n && (link_char(s[b]) && !(s[b] == '.' && (b + 1 == n || !word_char(s[b + 1])))))
		return 0;
	return 1;
}

static const uint32_t *const VERSION_WORDS[] = {
	U"версия", U"версии", U"версию", U"версией", U"версиям", U"версиях",
	U"ядро",   U"ядра",   U"ядру",   U"ядром",   U"ядре",
};

static int version_word(const uint32_t *s, size_t a, size_t b)
{
	rn_str low = {0};
	int r = 0;
	if (rn_py_lower(s + a, b - a, &low) == 0)
		for (size_t i = 0; i < sizeof VERSION_WORDS / sizeof *VERSION_WORDS && !r; i++)
			r = low.n == rn_u32len(VERSION_WORDS[i]) &&
			    !memcmp(low.s, VERSION_WORDS[i], low.n * sizeof *low.s);
	rn_str_free(&low);
	return r;
}

/* the number at s[c:]: digits, then ".digits" groups (speech-dispatcher writes "3 .10");
 * its end, and the number of dots, or 0 when it is not a plain number or version */
static size_t number_at(const uint32_t *s, size_t n, size_t c, int *dots)
{
	size_t e = c;
	*dots = 0;
	while (e < n && rn_is_ascii_digit(s[e]))
		e++;
	if (e == c)
		return 0;
	for (;;) {
		size_t d = e < n && s[e] == ' ' ? e + 1 : e;
		if (d + 1 < n && s[d] == '.' && rn_is_ascii_digit(s[d + 1])) {
			e = d + 1;
			while (e < n && rn_is_ascii_digit(s[e]))
				e++;
			(*dots)++;
			continue;
		}
		break;
	}
	/* "3,5", "14:30", "1/3", "3-й", "5%", "3x": not a bare number */
	if (e < n && (word_char(s[e]) || s[e] == '%' || s[e] == '-' || s[e] == '\'' ||
	              ((s[e] == ',' || s[e] == ':' || s[e] == '/' || s[e] == '.') && e + 1 < n &&
	               rn_is_ascii_digit(s[e + 1]))))
		return 0;
	return e;
}

/* digits as a cardinal; "04" with its zeros digit by digit ("ноль четыре") */
static int put_part(rn_str *out, const uint32_t *s, size_t n)
{
	char buf[32];
	if (n >= sizeof buf)
		return rn_str_put(out, s, n);
	if (n > 1 && s[0] == '0') {
		size_t i = 0;
		for (; i < n - 1 && s[i] == '0'; i++)
			if (rn_puts(out, RN_U("ноль ")) < 0)
				return -1;
		s += i;
		n -= i;
	}
	for (size_t i = 0; i < n; i++)
		buf[i] = (char)s[i];
	buf[n] = 0;
	if (rn_put_cardinal_a(out, buf) == 1)
		return rn_str_put(out, s, n);
	return 0;
}

static int put_version(rn_str *out, const uint32_t *s, size_t c, size_t e)
{
	size_t i = c;
	while (i < e) {
		size_t j = i;
		while (j < e && rn_is_ascii_digit(s[j]))
			j++;
		if (put_part(out, s + i, j - i) < 0)
			return -1;
		while (j < e && !rn_is_ascii_digit(s[j]))
			j++;
		if (j < e && rn_puts(out, RN_U(" точка ")) < 0)
			return -1;
		i = j;
	}
	return 0;
}

int rn_run_tech(const rn_str *in, rn_str *out)
{
	const uint32_t *s = in->s;
	size_t n = in->n, done = 0, i = 0;
	int cyr = 0;
	for (size_t k = 0; k < n && !cyr; k++)
		cyr = is_cyr(s[k]);
	while (i < n) {
		if (!word_char(s[i]) && s[i] != '/') {
			i++;
			continue;
		}
		/* R29: "10 / 2" */
		if (s[i] == '/') {
			if ((rn_fixes & RN_FIX_DIV_SLASH) && i >= 2 && i + 2 < n && s[i - 1] == ' ' &&
			    s[i + 1] == ' ' && rn_is_ascii_digit(s[i - 2]) && rn_is_ascii_digit(s[i + 2])) {
				if (rn_str_put(out, s + done, i - done) < 0 ||
				    rn_puts(out, RN_U("делить на")) < 0)
					return -1;
				done = i + 1;
			}
			i++;
			continue;
		}
		size_t a = i, b = i;
		while (b < n && (word_char(s[b]) || ((s[b] == '-' || s[b] == '+') && b > a)))
			b++;
		while (b > a + 1 && (s[b - 1] == '-'))
			b--;
		i = b;
		if (a && word_char(s[a - 1]))
			continue;
		/* R28: "No 5" */
		if ((rn_fixes & RN_FIX_NO_SIGN) && cyr && b - a == 2 && s[a] == 'N' && s[a + 1] == 'o' &&
		    (!a || !link_char(s[a - 1])) && b + 1 < n && s[b] == ' ' &&
		    rn_is_ascii_digit(s[b + 1])) {
			if (rn_str_put(out, s + done, a - done) < 0 || rn_str_putc(out, 0x2116) < 0)
				return -1;
			done = b;
			continue;
		}
		int lat = latin_name(s, n, a, b);
		const uint32_t *reading = lat ? rn_tech_word(s + a, b - a) : NULL;
		/* R27: the number after it */
		size_t e = 0;
		int dots = 0;
		if ((rn_fixes & RN_FIX_TECH_NUM) && b + 1 < n && s[b] == ' ' && rn_is_ascii_digit(s[b + 1]) &&
		    (lat || version_word(s, a, b))) {
			e = number_at(s, n, b + 1, &dots);
			if (e && !lat && !dots)
				e = 0;   /* "версия 5": the numerals stage reads it */
		}
		if (!reading && !e)
			continue;
		if (rn_str_put(out, s + done, a - done) < 0)
			return -1;
		if (reading ? rn_puts(out, reading) : rn_str_put(out, s + a, b - a))
			return -1;
		done = b;
		if (e) {
			if (rn_str_putc(out, ' ') < 0 || put_version(out, s, b + 1, e) < 0)
				return -1;
			done = i = e;
		}
	}
	return rn_str_put(out, s + done, n - done);
}
