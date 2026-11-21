/* math_formulas.py: $…$ blocks read out as Russian words. */
#include <string.h>

#include "rn-pass.h"
#include "rn-pat.h"
#include "rn-util.h"

struct word {
	const char *name;
	const char32_t *w;
};

struct sym {
	uint32_t c;
	const char32_t *w;
};

/* tables generated from math_formulas.py */
static const struct word GREEK[] = {
	{"alpha", U"альфа"},
	{"beta", U"бета"},
	{"gamma", U"гамма"},
	{"delta", U"дельта"},
	{"epsilon", U"эпсилон"},
	{"varepsilon", U"эпсилон"},
	{"zeta", U"дзета"},
	{"eta", U"эта"},
	{"theta", U"тета"},
	{"vartheta", U"тета"},
	{"iota", U"иота"},
	{"kappa", U"каппа"},
	{"varkappa", U"каппа"},
	{"lambda", U"ламбда"},
	{"mu", U"мю"},
	{"nu", U"ню"},
	{"xi", U"кси"},
	{"omicron", U"омикрон"},
	{"pi", U"пи"},
	{"rho", U"ро"},
	{"sigma", U"сигма"},
	{"varsigma", U"сигма"},
	{"tau", U"тау"},
	{"upsilon", U"ипсилон"},
	{"phi", U"фи"},
	{"varphi", U"фи"},
	{"chi", U"хи"},
	{"psi", U"пси"},
	{"omega", U"омега"},
};

static const struct word OPERATORS[] = {
	{"times", U"умножить на"},
	{"cdot", U"умножить на"},
	{"div", U"делить на"},
	{"pm", U"плюс минус"},
	{"mp", U"минус плюс"},
	{"le", U"меньше или равно"},
	{"leq", U"меньше или равно"},
	{"leqslant", U"меньше или равно"},
	{"ge", U"больше или равно"},
	{"geq", U"больше или равно"},
	{"geqslant", U"больше или равно"},
	{"ll", U"много меньше"},
	{"gg", U"много больше"},
	{"ne", U"не равно"},
	{"neq", U"не равно"},
	{"approx", U"примерно равно"},
	{"equiv", U"тождественно равно"},
	{"cong", U"равно"},
	{"sim", U"порядка"},
	{"propto", U"пропорционально"},
	{"infty", U"бесконечность"},
	{"in", U"принадлежит"},
	{"notin", U"не принадлежит"},
	{"ni", U"содержит"},
	{"subset", U"подмножество"},
	{"subseteq", U"подмножество"},
	{"supset", U"надмножество"},
	{"supseteq", U"надмножество"},
	{"cup", U"объединение"},
	{"bigcup", U"объединение"},
	{"cap", U"пересечение"},
	{"bigcap", U"пересечение"},
	{"setminus", U"без"},
	{"emptyset", U"пустое множество"},
	{"varnothing", U"пустое множество"},
	{"forall", U"для любого"},
	{"exists", U"существует"},
	{"nexists", U"не существует"},
	{"neg", U"не"},
	{"not", U"не"},
	{"land", U"и"},
	{"lor", U"или"},
	{"wedge", U"и"},
	{"vee", U"или"},
	{"to", U"стремится к"},
	{"rightarrow", U"стремится к"},
	{"longrightarrow", U"стремится к"},
	{"leftarrow", U"следует из"},
	{"Rightarrow", U"следует"},
	{"implies", U"следует"},
	{"Leftarrow", U"следует из"},
	{"leftrightarrow", U"равносильно"},
	{"Leftrightarrow", U"равносильно"},
	{"iff", U"равносильно"},
	{"mapsto", U"переходит в"},
	{"mid", U"такой что"},
	{"perp", U"перпендикулярно"},
	{"parallel", U"параллельно"},
	{"oplus", U"прямая сумма"},
	{"otimes", U"тензорное произведение"},
	{"odot", U"произведение"},
	{"bigcirc", U"кружок"},
	{"triangle", U"треугольник"},
	{"sum", U"сумма"},
	{"prod", U"произведение"},
	{"int", U"интеграл"},
	{"iint", U"двойной интеграл"},
	{"oint", U"контурный интеграл"},
	{"partial", U"частная производная"},
	{"nabla", U"набла"},
	{"lim", U"предел"},
	{"inf", U"инфимум"},
	{"sup", U"супремум"},
	{"min", U"минимум"},
	{"max", U"максимум"},
	{"log", U"логарифм"},
	{"ln", U"натуральный логарифм"},
	{"lg", U"десятичный логарифм"},
	{"exp", U"экспонента"},
	{"sin", U"синус"},
	{"cos", U"косинус"},
	{"tan", U"тангенс"},
	{"tg", U"тангенс"},
	{"cot", U"котангенс"},
	{"ctg", U"котангенс"},
	{"arcsin", U"арксинус"},
	{"arccos", U"арккосинус"},
	{"arctan", U"арктангенс"},
	{"degree", U"градусов"},
	{"circ", U"градусов"},
	{"prime", U"штрих"},
	{"sharp", U"диез"},
	{"flat", U"бемоль"},
	{"aleph", U"алеф"},
	{"dots", U"и так далее"},
	{"ldots", U"и так далее"},
	{"cdots", U"и так далее"},
	{"vdots", U"и так далее"},
	{"quad", U""},
	{"qquad", U""},
	{",", U""},
	{";", U""},
	{":", U""},
	{"!", U""},
};

static const char *const TRANSPARENT[] = {
	"Big",
	"Bigg",
	"acute",
	"bar",
	"big",
	"bigg",
	"breve",
	"check",
	"ddot",
	"displaystyle",
	"dot",
	"grave",
	"hat",
	"left",
	"limits",
	"mathbb",
	"mathbf",
	"mathcal",
	"mathfrak",
	"mathit",
	"mathrm",
	"mathsf",
	"operatorname",
	"overline",
	"right",
	"stackrel",
	"text",
	"textbf",
	"textit",
	"textrm",
	"tilde",
	"vec",
	"widehat",
};

static const struct sym SYMBOLS[] = {
	{0x3d, U" равно "},
	{0x2b, U" плюс "},
	{0x2d, U" минус "},
	{0x2212, U" минус "},
	{0x2a, U" умножить на "},
	{0x2f, U" делить на "},
	{0x3c, U" меньше "},
	{0x3e, U" больше "},
	{0x25, U" процент "},
	{0x28, U" "},
	{0x29, U" "},
	{0x5b, U" "},
	{0x5d, U" "},
	{0x7b, U" "},
	{0x7d, U" "},
	{0x7c, U" "},
	{0x5c, U" "},
	{0x26, U" "},
	{0x27, U" штрих "},
};

#define N(a) (sizeof a / sizeof *a)
#define P(id) rn_pat(RN_MATH_FORMULAS_##id)
#define MAX_BODY_CHARS 200

/* the ASCII name s[0..n) in a table of names */
static int name_eq(const uint32_t *s, size_t n, const char *name)
{
	size_t i = 0;
	for (; i < n && name[i]; i++)
		if (s[i] != (unsigned char)name[i])
			return 0;
	return i == n && !name[i];
}

static const char32_t *lookup(const struct word *t, size_t nt, const uint32_t *s, size_t n)
{
	for (size_t i = 0; i < nt; i++)
		if (name_eq(s, n, t[i].name))
			return t[i].w;
	return NULL;
}

static int in_names(const char *const *t, size_t nt, const uint32_t *s, size_t n)
{
	for (size_t i = 0; i < nt; i++)
		if (name_eq(s, n, t[i]))
			return 1;
	return 0;
}

/* p.match(s[a..b]) as a separate string */
static int match_tok(const rn_re *re, const uint32_t *s, size_t n)
{
	rn_match m;
	return rn_match_at(re, s, n, 0, &m) == 1;
}

static int is_math_body(const uint32_t *b, size_t n)
{
	rn_match m;
	if (!n)
		return 0;
	for (size_t i = 0; i + 1 < n; i++)
		if (b[i] == '\n' && b[i + 1] == '\n')
			return 0;
	if (rn_search(P(COMMAND_RE), b, n, 0, &m) == 1)
		return 1;
	int tokens = 0;
	for (size_t i = 0; i < n;) {   /* body.split() */
		while (i < n && rn_is_space(b[i]))
			i++;
		if (i == n)
			break;
		size_t k = i;
		while (k < n && !rn_is_space(b[k]))
			k++;
		tokens++;
		if (!(match_tok(P(NUMBER_RE), b + i, k - i) || match_tok(P(VARIABLE_RE), b + i, k - i) ||
		      match_tok(P(COEFFICIENT_RE), b + i, k - i) ||
		      match_tok(P(OPERATOR_TOKEN_RE), b + i, k - i)))
			return 0;
		i = k;
	}
	if (!tokens)
		return 0;
	return rn_search(P(LATIN_RE), b, n, 0, &m) == 1 ||
	       rn_search(P(COMPARISON_RE), b, n, 0, &m) == 1;
}

static int put_group(const rn_match *m, int g, rn_str *out)
{
	size_t n;
	const uint32_t *s = rn_group(m, g, &n);
	return rn_str_put(out, s, n);
}

static int put_spaced(rn_str *out, const char32_t *w)
{
	return rn_str_putc(out, ' ') < 0 || rn_puts(out, (const uint32_t *)w) < 0 ||
	       rn_str_putc(out, ' ') < 0 ? -1 : 0;
}

static const char *const STRUCTURAL[] = {"frac", "dfrac", "tfrac", "sqrt"};

static int repl_transparent(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *name = rn_group(m, 1, &n);
	if (in_names(STRUCTURAL, N(STRUCTURAL), name, n))
		return put_group(m, 0, out);
	if (!in_names(TRANSPARENT, N(TRANSPARENT), name, n) &&
	    (lookup(GREEK, N(GREEK), name, n) || lookup(OPERATORS, N(OPERATORS), name, n)))
		return put_group(m, 0, out);
	/* f" {inner} " for wrappers and unknown commands alike */
	return rn_str_putc(out, ' ') < 0 || put_group(m, 2, out) < 0 || rn_str_putc(out, ' ') < 0
	       ? -1 : 0;
}

static int repl_command(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *name = rn_group(m, 1, &n);
	const char32_t *w = lookup(GREEK, N(GREEK), name, n);
	if (!w)
		w = lookup(OPERATORS, N(OPERATORS), name, n);
	if (!w && n && n < 32) {
		/* \Delta reads as дельта: the first letter lowered */
		uint32_t low[32];
		memcpy(low, name, n * sizeof *low);
		if (low[0] >= 'A' && low[0] <= 'Z')
			low[0] |= 0x20;
		w = lookup(GREEK, N(GREEK), low, n);
	}
	return w ? put_spaced(out, w) : rn_str_putc(out, ' ');
}

/* _expand_nested: pattern.sub until nothing changes, at most 10 times */
static int expand_nested(const rn_re *re, const uint32_t *tmpl, rn_str *text)
{
	for (int i = 0; i < 10; i++) {
		rn_str before = {0};
		if (rn_str_copy(&before, text) < 0 || rn_resub_tmpl(re, text, tmpl) < 0) {
			rn_str_free(&before);
			return -1;
		}
		int same = rn_str_eq(&before, text);
		rn_str_free(&before);
		if (same)
			break;
	}
	return 0;
}

/* speak_formula(body), appended to out */
static int speak_formula(const uint32_t *body, size_t n, rn_str *out)
{
	rn_str t = {0}, before = {0}, u = {0};
	int r = -1;
	if (rn_str_put(&t, body, n) < 0)
		goto done;
	for (int i = 0; i < 10; i++) {
		if (rn_str_copy(&before, &t) < 0 ||
		    rn_resub(P(TRANSPARENT_RE), &t, repl_transparent, NULL) < 0 ||
		    rn_resub_tmpl(P(FRAC_RE), &t, RN_U(" \1 дробь \2 ")) < 0 ||
		    rn_resub_tmpl(P(ROOT_DEGREE_RE), &t, RN_U(" корень степени \1 из \2 ")) < 0 ||
		    rn_resub_tmpl(P(ROOT_RE), &t, RN_U(" корень из \1 ")) < 0)
			goto done;
		if (rn_str_eq(&before, &t))
			break;
	}
	if (rn_resub_tmpl(P(DEGREE_RE), &t, RN_U(" градусов ")) < 0 ||
	    rn_resub_tmpl(P(SCRIPT_COMMAND_RE), &t, RN_U("\1{\2}")) < 0 ||
	    expand_nested(P(POWER_BRACED_RE), RN_U(" в степени \1 "), &t) < 0 ||
	    rn_resub_tmpl(P(POWER_SHORT_RE), &t, RN_U(" в степени \1 ")) < 0 ||
	    expand_nested(P(INDEX_BRACED_RE), RN_U(" индекс \1 "), &t) < 0 ||
	    rn_resub_tmpl(P(INDEX_SHORT_RE), &t, RN_U(" индекс \1 ")) < 0 ||
	    rn_resub(P(ANY_COMMAND_RE), &t, repl_command, NULL) < 0)
		goto done;
	for (size_t i = 0; i < t.n; i++) {
		size_t k = 0;
		while (k < N(SYMBOLS) && SYMBOLS[k].c != t.s[i])
			k++;
		if ((k < N(SYMBOLS) ? rn_puts(&u, (const uint32_t *)SYMBOLS[k].w)
		                    : rn_str_putc(&u, t.s[i])) < 0)
			goto done;
	}
	if (rn_resub_tmpl(P(COEFFICIENT_SPLIT_RE), &u, RN_U(" ")) < 0 ||
	    rn_resub_tmpl(P(MULTI_SPACE_RE), &u, RN_U(" ")) < 0)
		goto done;
	size_t a = 0, b = u.n;   /* .strip() */
	while (a < b && rn_is_space(u.s[a]))
		a++;
	while (b > a && rn_is_space(u.s[b - 1]))
		b--;
	r = rn_str_put(out, u.s + a, b - a);
done:
	rn_str_free(&t);
	rn_str_free(&before);
	rn_str_free(&u);
	return r;
}

static int repl_block(const rn_match *m, rn_str *out, void *ctx)
{
	(void)ctx;
	size_t n;
	const uint32_t *body = rn_group(m, 1, &n);
	if (!is_math_body(body, n))
		return put_group(m, 0, out);
	size_t mark = out->n;
	if (rn_str_putc(out, ' ') < 0 || speak_formula(body, n, out) < 0)
		return -1;
	if (out->n == mark + 1)   /* nothing spoken: a single space */
		return 0;
	return rn_str_putc(out, ' ');
}

int rn_run_math(const rn_str *in, rn_str *out)
{
	size_t i = 0;
	while (i < in->n && in->s[i] != '$')
		i++;
	if (i == in->n)
		return rn_str_put(out, in->s, in->n);
	rn_str t = {0};
	int r = -1;
	if (rn_str_put(&t, in->s, in->n) < 0 || rn_resub(P(BLOCK_RE), &t, repl_block, NULL) < 0 ||
	    rn_resub_tmpl(P(MULTI_SPACE_RE), &t, RN_U(" ")) < 0 ||
	    rn_resub_tmpl(P(SPACE_BEFORE_PUNCT_RE), &t, RN_U("")) < 0)
		goto done;
	r = rn_str_put(out, t.s, t.n);
done:
	rn_str_free(&t);
	return r;
}
