/*
 * main.c -- czo(1)
 *
 * Reads one value per line from stdin (or takes values as arguments) and
 * emits a tab-separated canonicalization record per value:
 *
 *   verdict  reason  class  digest  skeleton  transforms  canon
 *
 * Exit status: 0 if every value canonicalized, 1 if any was rejected,
 * 2 on usage error.
 */

#include "czo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_ (CZO_MAX_INPUT + 2)

static void usage(FILE *f)
{
    fprintf(f,
        "czo " CZO_VERSION " -- Canonicalizer Zero (spec v%u)\n"
        "\n"
        "usage: czo [options] [value ...]\n"
        "       cat values.txt | czo [options]\n"
        "\n"
        "options:\n"
        "  -c, --class C   auto (default), id, name, numeric\n"
        "  -H, --header    print a header row\n"
        "  -q, --quiet     print digest only\n"
        "      --selftest  run the built-in vector suite and exit\n"
        "      --version   print version and exit\n"
        "  -h, --help      this text\n"
        "\n"
        "output columns:\n"
        "  verdict     OK | AMBIGUOUS | REJECT\n"
        "  reason      why, when not OK\n"
        "  class       resolved field class\n"
        "  digest      FNV-1a-64 of the canonical form -- the identity\n"
        "  skeleton    FNV-1a-64 of the confusable-folded form\n"
        "  transforms  lineage: which folds fired\n"
        "  canon       the canonical bytes\n",
        CZO_SPEC_VERSION);
}

static int parse_class(const char *s, czo_class *out)
{
    if (strcmp(s, "auto")    == 0) { *out = CZO_CLASS_AUTO;    return 0; }
    if (strcmp(s, "id")      == 0) { *out = CZO_CLASS_ID;      return 0; }
    if (strcmp(s, "name")    == 0) { *out = CZO_CLASS_NAME;    return 0; }
    if (strcmp(s, "numeric") == 0) { *out = CZO_CLASS_NUMERIC; return 0; }
    return -1;
}

static int emit(const char *value, size_t len, czo_class cls, int quiet)
{
    czo_result r;
    char tf[256];

    czo_canonicalize(value, len, cls, &r);
    czo_transforms_str(r.transforms, tf, sizeof(tf));

    if (quiet) {
        printf("%016llx\n", (unsigned long long)r.digest);
    } else {
        printf("%s\t%s\t%s\t%016llx\t%016llx\t%s\t%s\n",
               czo_verdict_str(r.verdict),
               czo_reason_str(r.reason),
               czo_class_str(r.resolved_class),
               (unsigned long long)r.digest,
               (unsigned long long)r.skeleton,
               tf,
               r.canon);
    }
    return (r.verdict == CZO_REJECT) ? 1 : 0;
}

int main(int argc, char **argv)
{
    czo_class cls = CZO_CLASS_AUTO;
    int quiet = 0, header = 0;
    int bad = 0;
    int i, first_value = argc;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0)            { first_value = i + 1; break; }
        if (a[0] != '-' || a[1] == '\0')     { first_value = i;     break; }

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout); return 0;
        }
        if (strcmp(a, "--version") == 0) {
            printf("czo %s (spec v%u)\n", CZO_VERSION, CZO_SPEC_VERSION);
            return 0;
        }
        if (strcmp(a, "--selftest") == 0) {
            return czo_selftest() == 0 ? 0 : 1;
        }
        if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            quiet = 1; continue;
        }
        if (strcmp(a, "-H") == 0 || strcmp(a, "--header") == 0) {
            header = 1; continue;
        }
        if (strcmp(a, "-c") == 0 || strcmp(a, "--class") == 0) {
            if (i + 1 >= argc || parse_class(argv[++i], &cls) != 0) {
                fprintf(stderr, "czo: bad --class\n");
                usage(stderr); return 2;
            }
            continue;
        }
        if (strncmp(a, "--class=", 8) == 0) {
            if (parse_class(a + 8, &cls) != 0) {
                fprintf(stderr, "czo: bad --class\n");
                return 2;
            }
            continue;
        }
        fprintf(stderr, "czo: unknown option %s\n", a);
        usage(stderr);
        return 2;
    }

    if (header && !quiet)
        printf("verdict\treason\tclass\tdigest\tskeleton\ttransforms\tcanon\n");

    if (first_value < argc) {
        for (i = first_value; i < argc; i++)
            bad |= emit(argv[i], strlen(argv[i]), cls, quiet);
        return bad;
    }

    {
        char line[LINE_MAX_];
        while (fgets(line, (int)sizeof(line), stdin) != NULL) {
            size_t n = strlen(line);
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
            bad |= emit(line, n, cls, quiet);
        }
    }
    return bad;
}
