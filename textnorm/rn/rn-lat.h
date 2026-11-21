/* latinization.py with the tts() options (rn-lat.c) */
#ifndef RN_LAT_H
#define RN_LAT_H

#include "rn-re.h"

extern int rn_lat_stress;   /* enable_latinization_stress_marks (tts(): 0) */
/* the word → IPA table of gen-ipa.py; 0, -1 with a static message in *err */
int rn_lat_open(const char *path, const char **err);
void rn_lat_close(void);
/* run_latinization; -1 also when no table is open and the text has Latin letters */
int rn_run_latinization(const rn_str *in, rn_str *out);

#endif
