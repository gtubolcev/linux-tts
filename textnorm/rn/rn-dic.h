/* dictionary.py DictionaryNormalizer: .dic substitution rules ("source=target" lines). */
#ifndef RN_DIC_H
#define RN_DIC_H

#include "rn-re.h"

typedef struct rn_dic rn_dic;

/* the rules of one .dic file (UTF-8 text); NULL with a message in err */
rn_dic *rn_dic_parse(const char *utf8, size_t len, char *err, size_t errlen);
void rn_dic_free(rn_dic *d);
/* normalizer.apply(text) in place; 0, -1 on error */
int rn_dic_apply(const rn_dic *d, rn_str *t);

#endif
