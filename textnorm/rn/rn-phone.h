/* the part of phonenumbers (libphonenumber's Python port) that phones.py uses */
#ifndef RN_PHONE_H
#define RN_PHONE_H

#include <stddef.h>

/* parse("+" + digits, None), is_valid_number() and
 * format_number(E164) == "+" + digits. 1: valid, *cc set and, if groups is not NULL, the
 * digit runs of format_number(INTERNATIONAL) copied to groups separated by ' ' (cap bytes);
 * 0: parse raised, invalid or E164 differs; -1: error. digits: ASCII 0-9 only. */
int rn_phone_international(const char *digits, int *cc, char *groups, size_t cap);

#endif
