/* the numerals passes (numerals/*.py), each in place on *t: 0, or -1 (rn_mx_failed) */
#ifndef RN_NPASS_H
#define RN_NPASS_H

#include <stdint.h>

#include "rn-re.h"

/* X(oracle function name without "normalize_", C name without "rn_n_") */
#define RN_NPASS_FUNCS(X)                                          \
	X(math_symbols, math_symbols)                                  \
	X(greek_letters, greek_letters)                                \
	X(standalone_currency, standalone_currency)                    \
	X(numeric_unit_hyphen_links, numeric_unit_hyphen_links)        \
	X(spaced_numeric_hyphen_words, spaced_numeric_hyphen_words)    \
	X(edition_ordinals, edition_ordinals)                          \
	X(prefixed_ordinal_adverbs, prefixed_ordinal_adverbs)          \
	X(thousands_abbreviated_attributes, thousands_abbreviated_attributes) \
	X(compound_numeric_adjectives, compound_numeric_adjectives)    \
	X(heading_ranges, heading_ranges)                              \
	X(heading_numbers, heading_numbers)                            \
	X(hyphenated_words, hyphenated_words)                          \
	X(square_unit_attributes, square_unit_attributes)              \
	X(versions, versions)                                          \
	X(decimals, decimals)                                          \
	X(fractions, fractions)                                        \
	X(numeric_suffixes, numeric_suffixes)                          \
	X(numeric_unit_ranges, numeric_unit_ranges)                    \
	X(remaining_post_numeral_abbreviations, remaining_post_numeral_abbreviations) \
	X(cardinal_numerals, cardinal_numerals)                        \
	X(all_digits_everywhere, all_digits_everywhere)

#define RN_NPASS_DECL(py, c) int rn_n_##c(rn_str *t);
RN_NPASS_FUNCS(RN_NPASS_DECL)
#undef RN_NPASS_DECL

int rn_n_is_ordinal_suffix(const uint32_t *s, size_t n);
int rn_n_is_cardinal_suffix(const uint32_t *s, size_t n);
int rn_n_is_numeric_suffix(const uint32_t *s, size_t n);
int rn_n_safe_hyphen_unit(const uint32_t *s, size_t n);
/* normalize_numeric_suffixes(t, capitalize_text_start=caps) */
int rn_n_numeric_suffixes_ex(rn_str *t, int caps);

#endif
