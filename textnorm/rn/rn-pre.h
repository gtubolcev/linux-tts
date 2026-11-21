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
	X(clean_numbers)                                                                        \
	X(convert_line_numbering)                                                               \
	X(convert_inline_list_markers)                                                          \
	X(convert_bracketed_numbers)                                                            \
	X(restore_non_normative_day_suffix)                                                     \
	X(normalize_page_references)                                                            \
	X(normalize_section_references)                                                         \
	X(normalize_administrative_law_references)                                              \
	X(normalize_complex_legal_references)                                                   \
	X(normalize_clause_references)                                                          \
	X(normalize_numbered_references)                                                        \
	X(normalize_isbn_identifiers)                                                           \
	X(normalize_number_signs)                                                               \
	X(normalize_identifier_letter_suffixes)                                                 \
	X(normalize_vehicle_registration_plates)                                                \
	X(normalize_phone_numbers)                                                              \
	X(normalize_labelled_identifiers)                                                       \
	X(drop_superscript_footnote_markers)                                                    \
	X(fix_glued_numbers)                                                                    \
	X(normalize_era_abbreviations)                                                          \
	X(normalize_numeric_abbreviations)                                                      \
	X(normalize_international_phone_numbers)                                                \
	X(expand_address_abbreviations)                                                         \
	X(normalize_first_word_caps)                                                            \
	X(normalize_caps_lines)

#define RN_PRE_DECL(name) int rn_pre_##name(const rn_str *in, rn_str *out);
RN_PRE_FUNCS(RN_PRE_DECL)
#undef RN_PRE_DECL

/* classify_bracketed_numeric_content: 'n'umeric, 'r'eference or 'o'ther */
char rn_classify_bracketed(const uint32_t *s, size_t n);

/* text_scan.first_non_whitespace_index; *saw_newline may be NULL */
size_t rn_first_non_ws(const uint32_t *s, size_t n, size_t start, int *saw_newline);
/* abbreviation_context.should_keep_terminal_abbreviation_dot */
int rn_keep_terminal_abbr_dot(const uint32_t *s, size_t n, size_t end);

#endif
