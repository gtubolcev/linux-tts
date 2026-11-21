/* numerals/symbols.py: normalize_math_symbols, normalize_greek_letters,
 * normalize_standalone_currency. Each replacement sees the input of its own sub as the text
 * (m->s), as the oracle's closures do. */
#include "rn-nx.h"
#include "rn-npass.h"
#include "rn-pat.h"

#define S(x) rn_pat(RN_NUMERALS_SYMBOLS_##x)

/* MATH_EXPRESSION_CHAR_PATTERN as a predicate */
static int math_char(uint32_t c)
{
	return (c >= '0' && c <= '9') || rn_is_ascii_alpha(c) || (c >= 0x410 && c <= 0x44F) ||
	       c == 0x401 || c == 0x451 || nx_inset(c, U"_,.%()+-/*^°№");
}

/* _extract_math_expression_side(text, start, step): *digit if it contains a digit; 0 if
 * it is empty */
static int side(const uint32_t *s, size_t n, long idx, int step, int *digit)
{
	int saw = 0;
	*digit = 0;
	for (; idx >= 0 && (size_t)idx < n; idx += step) {
		uint32_t c = s[idx];
		if (rn_py_isspace(c))
			continue;
		if (!math_char(c))
			break;
		saw = 1;
		if (rn_py_isdigit(c))
			*digit = 1;
	}
	return saw;
}

static int read_equals(const uint32_t *s, size_t n, size_t idx)
{
	/* prev_char/next_char "" at the ends is `in "<>!="` too */
	if (idx == 0 || nx_inset(s[idx - 1], U"<>!=") || idx + 1 >= n ||
	    nx_inset(s[idx + 1], U"<>!="))
		return 0;
	int dl, dr;
	if (!side(s, n, (long)idx - 1, -1, &dl) || !side(s, n, (long)idx + 1, 1, &dr))
		return 0;
	return dl || dr;
}

static void contextual_equals(rn_str *t)
{
	size_t last = 0, n = t->n;
	const uint32_t *s = t->s;
	rn_str o = {0};
	int any = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] != '=' || !read_equals(s, n, i))
			continue;
		size_t le = i;
		while (le > last && (s[le - 1] == ' ' || s[le - 1] == '\t'))
			le--;
		size_t rs = i + 1;
		while (rs < n && (s[rs] == ' ' || s[rs] == '\t'))
			rs++;
		nx_put(&o, s + last, le - last);
		nx_lit(&o, U" равно ");
		last = rs;
		any = 1;
	}
	if (!any || last == 0) {
		rn_str_free(&o);
		return;
	}
	nx_put(&o, s + last, n - last);
	rn_str_free(t);
	*t = o;
}

/* ---- the subs ---- */

static int inch_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t st = m->start[0];
	if (!nx_found(S(INCH_CONTEXT_PATTERN), m->s, nx_left(st, 64), st))
		return nx_whole(m, o);
	nx_putg(o, m, S(INCH_MARK_PATTERN), "value");
	nx_lit(o, U" дюйм");
	return 0;
}

static int angle_repl(const rn_match *m, rn_str *o, void *ctx)
{
	size_t st = m->start[0];
	if (!nx_found(S(ANGLE_CONTEXT_PATTERN), m->s, nx_left(st, 80), st))
		return nx_whole(m, o);
	nx_putg(o, m, S(ANGLE_MINUTE_MARK_PATTERN), "value");   /* the same group in both */
	nx_lit(o, ctx ? U" сек" : U" мин");
	return 0;
}

static int dplus_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	nx_putg(o, m, S(DOUBLE_PLUS_VERSION_PATTERN), "name");
	nx_lit(o, U" плюс плюс ");
	nx_putg(o, m, S(DOUBLE_PLUS_VERSION_PATTERN), "version");
	return 0;
}

static int sci_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = S(SCIENTIFIC_EXPONENT_PATTERN);
	size_t en, un;
	const uint32_t *e = nx_grp(m, p, "exponent", &en);
	char num[96];
	if (nx_pyint(e, en, num, sizeof num) < 0)
		return nx_whole(m, o);   /* too long: num2words raises → str(number) below */
	rn_str r = {0};
	nx_putg(&r, m, p, "base");
	nx_lit(&r, U" в минус ");
	nx_ordinal(&r, num, rn_g.gent, rn_g.femn, 0, 1);
	nx_lit(&r, U" степени");
	const uint32_t *u = nx_grp(m, p, "unit", &un);
	if (u && un) {
		const rn_unit *ui = rn_unit_resolve(u, un);
		if (!ui) {
			rn_str_free(&r);
			return nx_whole(m, o);
		}
		nx_putc(&r, ' ');
		nx_render_unit(&r, ui, nx_gs("nomn sing"), 0);
	}
	nx_put(o, r.s, r.n);
	rn_str_free(&r);
	return 0;
}

static int camera_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t st = m->start[0];
	if (!nx_found(S(L317), m->s, nx_left(st, 80), st))
		return nx_whole(m, o);
	nx_lit(o, U"f ");
	nx_putg(o, m, S(CAMERA_APERTURE_PATTERN), "whole");
	nx_lit(o, U" и ");
	nx_putg(o, m, S(CAMERA_APERTURE_PATTERN), "fraction");
	return 0;
}

static int ipv4_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	nx_putg(o, m, S(IPV4_PORT_PATTERN), "address");
	nx_lit(o, U" двоеточие ");
	nx_putg(o, m, S(IPV4_PORT_PATTERN), "port");
	return 0;
}

static int dim_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = S(DIMENSION_OPERATOR_PATTERN);
	size_t en = m->end[0], tl = m->n - en < 40 ? m->n - en : 40;
	rn_match um;
	int r = rn_match_at(S(L213), m->s + en, tl, 0, &um);
	if (r < 0)
		nx_fail();
	int ok = 0;
	if (r > 0) {
		size_t un;
		const uint32_t *u = nx_grp(&um, S(L213), "unit", &un);
		ok = rn_unit_resolve(u, un) != NULL;
	}
	if (!ok) {
		size_t st = m->start[0];
		ok = nx_found(S(DIMENSION_HEAD_PATTERN), m->s, nx_left(st, 30), st);
	}
	if (!ok)
		return nx_whole(m, o);
	nx_putg(o, m, p, "left");
	nx_lit(o, U" на ");
	nx_putg(o, m, p, "right");
	return 0;
}

static int relation_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t st = m->start[0], a = nx_left(st, 40);
	const char32_t *rel;
	if (nx_found(S(RATIO_CONTEXT_PATTERN), m->s, a, st))
		rel = U" к ";
	else if (nx_found(S(SCORE_CONTEXT_PATTERN), m->s, a, st))
		rel = U" — ";
	else
		return nx_whole(m, o);
	nx_putg(o, m, S(NUMERIC_RELATION_PATTERN), "left");
	nx_lit(o, rel);
	nx_putg(o, m, S(NUMERIC_RELATION_PATTERN), "right");
	return 0;
}

static int ineq_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const rn_re *p = S(INEQUALITY_OPERATOR_PATTERN);
	size_t n;
	const uint32_t *op = nx_grp(m, p, "operator", &n);
	nx_putg(o, m, p, "left");
	nx_lit(o, *op == '<' ? U" меньше " : U" больше ");
	return 0;
}

typedef struct {
	size_t *sp;   /* pairs */
	size_t n;
} spans;

static int minus_repl(const rn_match *m, rn_str *o, void *ctx)
{
	const spans *sp = ctx;
	size_t pos = m->start[0];
	for (size_t i = 0; i < sp->n; i++)
		if (sp->sp[2 * i] <= pos && pos < sp->sp[2 * i + 1]) {
			nx_lit(o, U" минус ");
			return 0;
		}
	return nx_whole(m, o);
}

/* normalize_arithmetic_minus */
static void arithmetic_minus(rn_str *t)
{
	int has_eq = 0;
	for (size_t i = 0; i < t->n; i++)
		if (t->s[i] == '=')
			has_eq = 1;
	if (!has_eq)
		return;
	spans sp = {0};
	size_t cap = 0;
	rn_iter it = rn_iter_init(S(EQUATION_PATTERN), t->s, t->n, 0);
	rn_match m;
	int r;
	while ((r = rn_iter_next(&it, &m)) == 1) {
		int eqin = 0;
		for (size_t i = m.start[0]; i < m.end[0]; i++)
			if (t->s[i] == '=')
				eqin = 1;
		if (!eqin)
			continue;
		if (sp.n == cap) {
			cap = cap ? 2 * cap : 8;
			size_t *ns = realloc(sp.sp, 2 * cap * sizeof *ns);
			if (!ns) {
				nx_fail();
				break;
			}
			sp.sp = ns;
		}
		sp.sp[2 * sp.n] = m.start[0];
		sp.sp[2 * sp.n + 1] = m.end[0];
		sp.n++;
	}
	if (r < 0)
		nx_fail();
	if (sp.n)
		nx_resub(S(ASCII_BINARY_MINUS_PATTERN), t, minus_repl, &sp);
	free(sp.sp);
}

static int binop_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	size_t n;
	uint32_t c = *nx_grp(m, S(BINARY_MATH_OPERATOR_PATTERN), "operator", &n);
	nx_lit(o, c == '+' ? U" плюс " : c == 0x2212 ? U" минус " : c == '^' ? U" в степени "
	                                                                     : U" умножить на ");
	return 0;
}

static void tmpl(const rn_re *re, rn_str *t, const char32_t *s)
{
	if (rn_resub_tmpl(re, t, (const uint32_t *)s) < 0)
		nx_fail();
}

int rn_n_math_symbols(rn_str *t)
{
	static const char32_t *const MATH[][2] = {
		{U"×", U" умножить на "},
		{U"÷", U" разделить на "},
		{U"±", U" плюс-минус "},
		{U"∓", U" минус-плюс "},
		{U"≈", U" приблизительно равно "},
		{U"≠", U" не равно "},
		{U"≤", U" меньше или равно "},
		{U"≥", U" больше или равно "},
		{U"∞", U" бесконечность "},
		{U"√", U" корень из "},
		{U"∑", U" сумма "},
		{U"∏", U" произведение "},
		{U"∫", U" интеграл "},
		{U"∂", U" дель "},
		{U"∈", U" принадлежит "},
		{U"∉", U" не принадлежит "},
		{U"∪", U" объединение "},
		{U"∩", U" пересечение "},
		{U"⇒", U" следует "},
		{U"№", U" номер "},
	};
	nx_resub(S(INCH_MARK_PATTERN), t, inch_repl, NULL);
	nx_resub(S(ANGLE_MINUTE_MARK_PATTERN), t, angle_repl, NULL);
	nx_resub(S(INCH_MARK_PATTERN), t, angle_repl, (void *)1);   /* ANGLE_SECOND_MARK */
	tmpl(S(MATH_X_VARIABLE_PATTERN), t, U"икс");
	nx_resub(S(DOUBLE_PLUS_VERSION_PATTERN), t, dplus_repl, NULL);
	nx_resub(S(SCIENTIFIC_EXPONENT_PATTERN), t, sci_repl, NULL);
	nx_resub(S(CAMERA_APERTURE_PATTERN), t, camera_repl, NULL);
	nx_resub(S(IPV4_PORT_PATTERN), t, ipv4_repl, NULL);
	tmpl(S(WORD_PLUS_NUMBER_PATTERN), t, U" плюс ");
	nx_resub(S(DIMENSION_OPERATOR_PATTERN), t, dim_repl, NULL);
	nx_resub(S(NUMERIC_RELATION_PATTERN), t, relation_repl, NULL);
	nx_resub(S(INEQUALITY_OPERATOR_PATTERN), t, ineq_repl, NULL);
	arithmetic_minus(t);
	contextual_equals(t);
	tmpl(S(APPROXIMATE_NUMBER_PATTERN), t, U"примерно ");
	nx_resub(S(BINARY_MATH_OPERATOR_PATTERN), t, binop_repl, NULL);
	for (size_t i = 0; i < sizeof MATH / sizeof *MATH; i++)
		if (rn_py_rereplace(t, (const uint32_t *)MATH[i][0], (const uint32_t *)MATH[i][1]) < 0)
			nx_fail();
	return rn_mx_failed ? -1 : 0;
}

static const char32_t *greek_name(uint32_t c)
{
	static const struct {
		char32_t c;
		const char32_t *name;
	} G[] = {
		{U'α', U"альфа"}, {U'β', U"бета"},    {U'γ', U"гамма"},  {U'δ', U"дельта"},
		{U'ε', U"эпсилон"}, {U'ϵ', U"эпсилон"}, {U'ζ', U"дзета"},  {U'η', U"эта"},
		{U'θ', U"тета"},  {U'ϑ', U"тета"},    {U'ι', U"йота"},   {U'κ', U"каппа"},
		{U'ϰ', U"каппа"}, {U'λ', U"лямбда"},  {U'μ', U"мю"},     {U'µ', U"мю"},
		{U'ν', U"ню"},    {U'ξ', U"кси"},     {U'ο', U"омикрон"}, {U'π', U"пи"},
		{U'ϖ', U"пи"},    {U'ρ', U"ро"},      {U'ϱ', U"ро"},     {U'σ', U"сигма"},
		{U'ς', U"сигма"}, {U'τ', U"тау"},     {U'φ', U"фи"},     {U'ϕ', U"фи"},
		{U'χ', U"хи"},    {U'ψ', U"пси"},     {U'ω', U"омега"},  {U'Δ', U"дельта"},
		{U'Σ', U"сигма"}, {U'Ω', U"омега"},   {U'Π', U"пи"},     {U'Φ', U"фи"},
		{U'Γ', U"гамма"}, {U'Λ', U"лямбда"},  {U'Θ', U"тета"},
	};
	for (size_t i = 0; i < sizeof G / sizeof *G; i++)
		if (G[i].c == c)
			return G[i].name;
	return NULL;
}

static int greek_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	const char32_t *name;
	if (m->end[0] - m->start[0] != 1 || !(name = greek_name(m->s[m->start[0]])))
		return nx_whole(m, o);
	nx_lit(o, name);
	return 0;
}

int rn_n_greek_letters(rn_str *t)
{
	nx_resub(S(GREEK_PATTERN), t, greek_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}

static int currency_repl(const rn_match *m, rn_str *o, void *ctx)
{
	(void)ctx;
	static const struct {
		char32_t c;
		const char32_t *name;
	} C[] = {{U'$', U"доллар"}, {U'€', U"евро"},  {U'£', U"фунт"},  {U'¥', U"йена"},
	         {U'₽', U"рубль"},  {U'₴', U"гривна"}, {U'₸', U"тенге"}, {U'₺', U"лира"},
	         {U'₹', U"рупия"},  {U'¢', U"цент"}};
	uint32_t c = m->s[m->start[0]];
	for (size_t i = 0; i < sizeof C / sizeof *C; i++)
		if (C[i].c == c) {
			nx_lit(o, C[i].name);
			return 0;
		}
	return nx_whole(m, o);
}

int rn_n_standalone_currency(rn_str *t)
{
	nx_resub(rn_pat(RN_NUMERALS_CONSTANTS_STANDALONE_CURRENCY_PATTERN), t, currency_repl, NULL);
	return rn_mx_failed ? -1 : 0;
}
