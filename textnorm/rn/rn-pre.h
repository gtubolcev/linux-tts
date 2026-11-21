/* the preprocess helpers (preprocess_utils.py, quotes.py), one per Python function */
#ifndef RN_PRE_H
#define RN_PRE_H

#include "rn-re.h"

#define RN_PRE_FUNCS(X)                                                                         \
	X(remove_decorative_separators)                                                         \
	X(remove_zero_width_formatting)                                                         \
	X(normalize_cyrillic_combining_stress_marks)                                            \
	X(collapse_letterspaced_emphasis)                                                       \
	X(protect_letter_hyphens)                                                               \
	X(restore_letter_hyphens)                                                               \
	X(protect_binary_math_operators)                                                        \
	X(restore_binary_math_operators)                                                        \
	X(normalize_explicit_dashes)                                                            \
	X(space_abbreviation_dot_before_digit)                                                  \
	X(normalize_unary_plus_numbers)                                                         \
	X(expand_na_prim_abbreviation)                                                          \
	X(expand_new_style_date_abbreviation)                                                   \
	X(expand_years_ago_abbreviation)                                                        \
	X(expand_arc_minutes)                                                                   \
	X(normalize_age_ranges)                                                                 \
	X(protect_negative_numbers)                                                             \
	X(apply_cleanup_replacements)                                                           \
	X(normalize_unicode_fractions)                                                          \
	X(normalize_linebreaks)                                                                 \
	X(join_hyphenated_line_breaks)                                                          \
	X(insert_missing_space_before_paren)                                                    \
	X(normalize_script_homoglyphs)                                                          \
	X(normalize_ascii_quote_pairs)                                                          \
	X(normalize_punctuation_spacing)                                                       \
	X(protect_unit_slashes)                                                                 \
	X(clean_numbers)

#define RN_PRE_DECL(name) int rn_pre_##name(const rn_str *in, rn_str *out);
RN_PRE_FUNCS(RN_PRE_DECL)
#undef RN_PRE_DECL

#endif
