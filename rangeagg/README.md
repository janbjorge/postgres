# rangeagg

Exact time-bucket aggregates (`count`, `sum`, `min`, `max` per hour, and so on) on an ordinary PostgreSQL table, kept fast by a new index type built into PostgreSQL itself, not an extension (jb 2026-10-19). Code name rangeagg (mine). Prototype work in this PostgreSQL tree, branch `rangeagg-proto`.

**Status (2026-10-19):** the design is chosen and nothing is built. Next is M1 in core, the first slice with `count`, `sum`, `min` and `max` ([TODO.md](TODO.md)).

**The one question** ([DESIGN.md §1](DESIGN.md#1-what-and-the-one-question)): can a plain per-hour `GROUP BY` over 30 days of a log table be answered exactly, from a few hundred summary rows plus the open hours, with no triggers and almost no cost on insert?

- [DESIGN.md](DESIGN.md): the target. The decisions with jb's words, the design (sealed buckets behind a watermark), why it is exact, its races, the one core change, and the milestones.
- [ALTERNATIVES.md](ALTERNATIVES.md): every design weighed and why it was dropped or parked, including the visibility-map proof.
- [RESEARCH.md](RESEARCH.md): what PostgreSQL does, read in this tree with file:line, and what is still to check.
- [NOTES.md](NOTES.md): dated entries: the build, the experiments, the numbers.
- [TODO.md](TODO.md): the list, and the questions for jb.
- [SPEC.md](SPEC.md): the starting spec, word for word.
- [docs/done.md](docs/done.md): finished requests.
- [AGENTS.md](AGENTS.md): rules for agents.
- [tools/](tools/): `vm_lsn_test.sql` (NOTES.md M0.1) and `aggs.py` (NOTES.md M0.3).
