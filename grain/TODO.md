# grain: TODO

Everything marked "my proposal" waits on jb. A finished item leaves this list for docs/done.md, with the date and jb's words. A line links the NOTES ID for its numbers and never copies them.

## Open

- [ ] **P1, the read ceiling** (jb 2026-10-19: "i want proof that this works, and that it brings the insame powerup"). Done, waiting on jb's review: NOTES.md P1.
- [ ] **M0.5, draft the design email,** after M1 and M3 (jb 2026-10-19; DESIGN.md §7). Drafted only; sending is jb's call. Started early (jb 2026-10-20: "Lets start on a design email"): one email with the full design, in RFC.txt (jb 2026-10-20: "lets do one rfc, if they want it broken up, we can do that later").
- [ ] **M1, the first slice, in core** (jb 2026-10-19: "lets start with some basic agg-functions, like sum, count, min, max?" and "i want this new index tobe part of postgres"). DESIGN.md §6 and §7, as patches 0001 and 0002. In order:
  1. ~~Check the executor paths~~ and ~~0001: the delete callback~~: built and tested, uncommitted (NOTES.md M1.1).
  2. Check how core can own hidden heaps per index (RESEARCH.md §10).
  3. 0002: the AM in `src/backend/access/grain/`: catalog rows, reloptions, metapage, hidden heaps, `pg_grain_seal`, `pg_grain_buckets`, its docs chapter.
  4. Regress tests against a plain `GROUP BY` under the same snapshot.
  5. The isolation tests of DESIGN.md §5.
- [ ] **M2, the planner path** (my proposal; DESIGN.md §7).
- [ ] **M3, benchmarks** at 1k, 100k, 1M and 10M rows, maybe 100M (jb 2026-10-19; DESIGN.md §7). Each says first what question it answers.

## Built, waiting on jb's review

- [ ] **These docs** (jb 2026-10-19: "Im starting to lose oversight, i think we need a to write all ou knowlage down into md files, see; @../project-orc/ as template of the md files"). Reworked for core the same day: W in the metapage, hidden heaps instead of extension tables (DESIGN.md §2, §3.2, §6).

## Questions for jb

- [ ] **Meson and DocBook.** Neither is installed here, so new `meson.build` files and the SGML docs can't be checked. May I install them (`brew install meson docbook docbook-xsl`)?

## Parked

- [ ] **D1 at a finer granularity:** per-chunk stamps, a new VM format, or heap page LSNs (ALTERNATIVES.md §2.4).
- [ ] **BRIN that tightens min/max on resummarize,** a small patch of its own (ALTERNATIVES.md §11).
