# rangeagg: TODO

Everything marked "my proposal" waits on jb. A finished item leaves this list for docs/done.md, with the date and jb's words. A line links the NOTES ID for its numbers and never copies them.

## Open

- [ ] **M0.5, draft the design email** (my proposal; DESIGN.md §7). Drafted only; sending is jb's call.
- [ ] **M1, the first slice, in core** (jb 2026-10-19: "lets start with some basic agg-functions, like sum, count, min, max?" and "i want this new index tobe part of postgres"). DESIGN.md §6 and §7, as patches 0001 and 0002. In order:
  1. Check RESEARCH.md §10's executor paths and the update result.
  2. 0001: the delete callback, tested through `dummy_index_am`.
  3. Check how core can own hidden heaps per index (RESEARCH.md §10).
  4. 0002: the AM in `src/backend/access/rangeagg/`: catalog rows, reloptions, metapage, hidden heaps, `pg_rangeagg_seal`, `pg_rangeagg_buckets`, its docs chapter.
  5. Regress tests against a plain `GROUP BY` under the same snapshot.
  6. The isolation tests of DESIGN.md §5.
- [ ] **M2, the planner path** (my proposal; DESIGN.md §7).
- [ ] **M3, benchmarks** at 1k, 100k, 1M and 10M rows, maybe 100M (jb 2026-10-19; DESIGN.md §7). Each says first what question it answers.

## Built, waiting on jb's review

- [ ] **These docs** (jb 2026-10-19: "Im starting to lose oversight, i think we need a to write all ou knowlage down into md files, see; @../project-orc/ as template of the md files"). Reworked for core the same day: W in the metapage, hidden heaps instead of extension tables (DESIGN.md §2, §3.2, §6).

## Questions for jb

- [ ] **The name users type** in `USING`, and so `pg_<name>_*` and `src/backend/access/<name>/` (jb asked for other suggestions). Mine:
  - **`binagg`**, my pick: aggregates per bin, the word `date_bin` uses, and it still fits integer keys (M4);
  - **`tally`**: short and plain, but it reads as counting only;
  - **`rangeagg`**: too close to the existing `range_agg()` aggregate (`pg_proc.dat`);
  - not `rollup`: a keyword (`kwlist.h:394`), and confusable with `GROUP BY ROLLUP`.

  Expect pgsql-hackers to weigh in anyway.
- [ ] **The design email** (DESIGN.md §7, M0.5): send it before M1's code, as upstream advises (RESEARCH.md §9.7), or after a working prototype?
- [ ] **Meson.** It isn't installed here, so new `meson.build` files can't be checked. May I install it (`brew install meson`)?

## Parked

- [ ] **D1's finer grain:** per-chunk stamps, a new VM format, or heap page LSNs (ALTERNATIVES.md §2.4).
- [ ] **BRIN that tightens min/max on resummarize,** a small patch of its own (ALTERNATIVES.md §11).
