#include "czo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long s = 88172645463325252UL;
static unsigned long xs(void){ s^=s<<13; s^=s>>7; s^=s<<17; return s; }
int main(void){
    static char in[CZO_MAX_INPUT+64];
    czo_result r, r2;
    long k;
    for (k = 0; k < 400000; k++) {
        size_t n = (size_t)(xs() % (CZO_MAX_INPUT + 32));
        size_t i;
        int cls = (int)(xs() % 4);
        for (i = 0; i < n; i++) {
            unsigned long m = xs() % 10;
            if (m < 5) in[i] = (char)(0x20 + (xs()%0x5f));      /* ascii  */
            else if (m < 8) in[i] = (char)(0x80 + (xs()%0x80)); /* high   */
            else in[i] = (char)(xs()%256);                      /* any    */
        }
        czo_canonicalize(in, n, (czo_class)cls, &r);
        if (r.verdict != CZO_REJECT) {
            if (r.canon[r.canon_len] != '\0') { printf("BAD nul term\n"); return 1; }
            if (strlen(r.canon) != r.canon_len) { printf("BAD len\n"); return 1; }
            /* idempotence on every accepted output */
            czo_canonicalize(r.canon, r.canon_len, r.resolved_class, &r2);
            if (r2.verdict == CZO_REJECT || r2.digest != r.digest) {
                printf("BAD idempotence at k=%ld cls=%d canon=[%s]\n", k, cls, r.canon);
                return 1;
            }
        } else if (r.canon_len != 0) { printf("BAD reject len\n"); return 1; }
    }
    printf("fuzz: 400000 cases clean\n");
    return 0;
}
