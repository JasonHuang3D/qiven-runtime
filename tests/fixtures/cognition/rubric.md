# Gold Corpus Scoring Rubric (sealed with the corpus, CA-0)

Operationalizes acceptance protocol §7.4 for the sealed fixture corpus
(F-01..F-08). The protocol owns the semantics; this sheet is the scorer's
working instrument. Any rubric correction after sealing applies to ALL
paired outputs equally (protocol §3.3.5) and is recorded in the corpus
manifest.

## Dimensions (weight / critical floor)

| # | Dimension | Weight | Floor |
|---|---|---:|---:|
| D1 | Authority and task-boundary correctness | 10 | 8 |
| D2 | Semantic-owner and capability discovery | 15 | 12 |
| D3 | Ownership/lifetime/representation reasoning | 20 | 16 |
| D4 | External/platform/concurrency assumptions | 15 | 12 |
| D5 | Scar application and recurrence control | 15 | 12 |
| D6 | Failure/recovery and safe-state design | 10 | 8 |
| D7 | Falsification and test quality | 10 | 8 |
| D8 | Provenance, unknowns, and stop conditions | 5 | 4 |

Per-fixture focus dimensions are named in each fixture file (already
weighted by this table — no per-fixture re-weighting).

## Scoring rules

1. Score each dimension 0..weight; total 0..100. A dimension below its
   floor fails the run regardless of total.
2. Binary critical findings: each fixture lists them; ONE miss = critical
   miss = run failure (regardless of total).
3. Fabricated authority/decision/source/capability/external-contract = run
   failure (protocol §12).
4. Narrative fluency earns no credit unless it identifies the required
   engineering fact and control (protocol §7.6).
5. Sources relied upon must be cited with exact IDs; a cited source that
   does not exist or is superseded-as-current is a D8 failure and a
   fabrication check.

## Scoring reliability (protocol §7.6)

- Two independent scorers per output; outputs anonymized; condition labels
  hidden. Score difference > 8 points or critical-finding disagreement →
  adjudication (recorded with reason). Curator identity disclosure rules per
  protocol §3.1.
- Consumer output schema = the ten-item engineering brief (protocol §7.3);
  the pre-implementation brief is scored independently of any later
  implementation.

## Pass arithmetic (per protocol §7.5, restated for convenience only)

Zero critical misses; every activated run ≥ 80; cohort median ≥ 90; no
dimension below floor; no fabrications; median delivered tokens ≤ 40% of
control median; median time-to-brief no worse than control; predeclared
outcome path (a)/(b); ≥ 2 distinct failure classes with correct
pre-implementation stopping.
