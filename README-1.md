# Canonicalizer Zero (CZO)

**One identity. One representation. One lineage.**

CZO is a Phase 0 gate: it runs before any comparison, join, aggregation, or
match. It takes a byte string and returns a canonical byte string, a stable
64-bit identity digest, a confusable-folded skeleton digest, and a bitmask
recording exactly which transforms fired.

Freestanding C99. No allocation, no I/O in the library, no locale, no
dependencies, no configuration. Two files.

```
$ printf 'Acme Corp.\n  ACME   CORP \nacme\xe2\x80\x8b corp\n' | czo -H
verdict  reason  class  digest            skeleton          transforms                    canon
OK       -       name   a342a637ca117aaf  a342a637ca117aaf  case_fold,sep_strip           acme corp
OK       -       name   a342a637ca117aaf  a342a637ca117aaf  trim,ws_collapse,case_fold    acme corp
OK       -       name   a342a637ca117aaf  a342a637ca117aaf  zw_strip                      acme corp
```

Same identity, three different representations, and a record of what it took
to get there.

## Build

```sh
make          # builds ./czo and czo.o
make test     # built-in vector suite
make fuzz     # 400k randomized cases under ASan + UBSan
make libczo.a # static library for embedding
```

## Library

```c
#include "czo.h"

czo_result r;
czo_canonicalize_str("INV-2024/0071", CZO_CLASS_ID, &r);

r.verdict     /* CZO_OK | CZO_AMBIGUOUS | CZO_REJECT */
r.canon       /* "inv20240071" */
r.digest      /* FNV-1a-64 of canon -- the identity */
r.skeleton    /* FNV-1a-64 of the confusable-folded form */
r.transforms  /* CZO_T_CASE_FOLD | CZO_T_SEP_STRIP -- the lineage */
```

## The three verdicts

| Verdict | Meaning | What you do with it |
|---|---|---|
| `OK` | The canonical form is authoritative. | Pass downstream. |
| `AMBIGUOUS` | A canonical form exists, but producing it discarded something that may be semantic. | Quarantine. Do not feed to a matcher without a policy. |
| `REJECT` | No canonical form exists. `canon` is empty. | Drop or bounce back to the source. |

`AMBIGUOUS` is the whole point. Most pipelines have exactly two outcomes —
"parsed" and "crashed" — and silently absorb the ambiguity into whichever
answer they produce. CZO makes it a third, first-class outcome that leaves
the system rather than dissolving into it.

Current ambiguity conditions:

- **`mixed_script`** — two of {Latin, Greek, Cyrillic} in one token. Classic
  homograph. CZO does **not** silently fold it into Latin; that would destroy
  the evidence. It emits the true canonical form, flags the token, and gives
  you a skeleton digest so you can find the Latin twin yourself.
- **`leading_zero_numeric`** — `0042` canonicalizes to `42`, because in most
  data they are one identity. In account and check numbers they are not. CZO
  picks the shorter form and refuses to pretend it knows which case you're in.

Rejection conditions: empty after canonicalization, over `CZO_MAX_INPUT`,
invalid UTF-8 (including overlongs and surrogates), and non-whitespace
control characters.

## Guaranteed properties

- **Deterministic.** Identical input bytes produce identical output bytes,
  on every platform, forever, at a given `CZO_SPEC_VERSION`.
- **Idempotent.** `czo(czo(x)) == czo(x)` for every accepted input. Verified
  against 400,000 randomized inputs under ASan and UBSan, not just asserted.
- **Total.** Every input produces a fully initialized result. No undefined
  behavior, no partial writes, no allocation, no failure path that isn't a
  verdict.
- **Auditable.** The transform bitmask is a complete account of the distance
  between what arrived and what was stored.

The pipeline order in `czo.c` is part of the contract. Changing it changes
digests and requires a `CZO_SPEC_VERSION` bump. Stored digests are only
comparable within a spec version.

## Field classes

`CZO_CLASS_AUTO` infers; you should usually declare instead.

- `id` — separators (`- _ . / : \ #` and spaces) are noise and are removed.
- `numeric` — everything but digits is removed.
- `name` — word boundaries are signal. Punctuation is dropped, connectors
  become spaces, runs of space collapse.

## What CZO does not do

This list is the product boundary, not a to-do list.

- **No corpus.** CZO sees one value at a time and has no memory. Whether two
  canonical forms are a duplicate, a collision, or a coincidence is a
  question about a *set*, and CZO has no set.
- **No matching, scoring, or fuzzy comparison.** No edit distance, no
  blocking keys, no thresholds. Digests are equal or they are not.
- **No Unicode normalization (NFC/NFD/NFKC).** CZO folds Latin-1, Latin
  Extended-A, common compatibility characters, and combining marks in the
  U+0300 block to ASCII. That covers the overwhelming majority of real
  business data and requires no ICU. It is not a Unicode normalization
  implementation and does not claim to be.
- **No case folding outside ASCII, Cyrillic, and Greek.** Turkish dotless
  ı, Cherokee, and other locale-sensitive cases are left alone rather than
  folded wrongly.
- **No business rules.** Corporate suffixes (`Inc`, `LLC`, `GmbH`), address
  abbreviations, date and currency parsing, and industry-specific identifier
  formats are domain knowledge, not canonicalization. They belong to the
  layer above.
- **Ligatures collapse to their first letter.** `Æ` → `AE`, but `Ĳ` → `I`.
  Documented and deliberate; revisit only with a spec version bump.

## Where this sits

CZO is the first of five stages in a normalize → test → route → trace →
audit spine. It is the only one that can be specified without reference to
your data, which is why it is the one that can be given away.

Canonicalization is roughly 60–70% of the *volume* of work in a
deduplication pipeline and close to 0% of the *judgment*. The judgment —
what constitutes a duplicate, which collisions matter, what a finding is
worth, which exceptions are real — lives in the engine above this line, and
is not in this repository.

## License

Apache-2.0. (Add `LICENSE` before publishing; Apache-2.0 rather than MIT
because it carries an explicit patent grant, which matters for a
company-backed project that others will embed.)
