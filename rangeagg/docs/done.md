# Done

Finished requests from jb, newest first. Each quotes the request and says what came of it.

## 2026-10-19

- [x] **"Also, lets read up on https://wiki.postgresql.org/wiki/Submitting_a_Patch so we make sure we do not have todo loads of double work"**:
  - the rules are in RESEARCH.md §9.7, and their consequences in AGENTS.md;
  - DESIGN.md §7 now starts with a design email (M0.5) and splits M1 into patches 0001 and 0002.
- [x] **"4, sure"**, to committing the docs: committed on `rangeagg-proto`.
- [x] **"3, Unsure about these bencharmsk, lets, try a few order of magnitude, like 1k, 100k, 1M, 10M, meybe 100M?"** DESIGN.md §7, M3.
- [x] **"2, Yes, drop it, i feel that partitions has to meny wierd quarks and details."** Route 1 dropped (ALTERNATIVES.md §3).
- [x] **"The goal here is NOT yet another postgres extension, i want this new index tobe part of postgres"**, which also answered whether a core patch is acceptable. DESIGN.md is now a built-in index AM (§2, §6). Being in core changed the state (§3.2):
  - W moved from a reloption to the index's metapage, so writers need no relcache and the sealer no ALTER INDEX;
  - the extension's three tables became two hidden heaps owned by the index, and a bucket counts as sealed when its row is visible.
- [x] **"I think we need to seach postgres mailing lists, if there some something like this being worked on, or has been worked on."** Three searches: IVM and TimescaleDB, index AM delete notification and BRIN, the planner side (RESEARCH.md §9). Nothing like DESIGN.md has been proposed. They led to:
  - summaries hold typed values, never serialized states (DESIGN.md §3.2);
  - the reloption lock-level question closed by reading the code (RESEARCH.md §3).
- [x] **"TimescaleDB is relted, but i think TimescaleDB is overkill for loads of problmes that are in this is sphare."** Taken as a call (DESIGN.md §2, §8). Route 2, a plain table with an index-like declaration, became the target over Route 1's partitions (ALTERNATIVES.md §3, §10).
- [x] **"I know we can do triggers, but i think most ppl. fear triggers, as drift other issues creep in."** Triggers are out (ALTERNATIVES.md §7). Two trigger-free routes came of it:
  - Route 1, partitions with the VM page LSN (ALTERNATIVES.md §3);
  - Route 2, a core delete callback (DESIGN.md §6).
  - Also rejected on the way: a heap wrapped in another table AM, and changes read from WAL (ALTERNATIVES.md §8, §9).
- [x] **"say i have a downloads log, it contains some data, and a done_at timestamp, could i create an 'index' on this table and the done_at? and get a super fast view of the download, say for the last 30 days."** The declaration and the plain query it serves (DESIGN.md §3.1). It needs a planner hook, a delete callback, a sealer and hidden state (DESIGN.md §6, §7).
- [x] **"So say i wanted to capture downloads per hours, cound i use this?"** Yes; worked through in DESIGN.md §3.5. The SQL form written for it is ALTERNATIVES.md §7.
- [x] **"Assuming that these buckets are failir small, i find it hard to beleave that this is a problem? ... index might not be right option, nor that we should lean on btree, we might need to consider one of the 100s of table flags to get what we need. I refuse that this cant solved in a clean and perfrment way"** The sealed buckets behind a watermark, using CREATE INDEX CONCURRENTLY's wait (DESIGN.md §3, §4; RESEARCH.md §5).
- [x] **"incrent / decremtn, was one suggestion, there must be tricks we can do"** Deltas tagged with transaction ids, D5 (ALTERNATIVES.md §6). Later superseded.
- [x] **"I dont understand why we need to offload this to VACUUM? ... it shoud thus be pretty easy to find that relevent bucket, and incremt / decremt it?"** Why one counter per bucket breaks: snapshots, abort, delete, min/max (ALTERNATIVES.md §5).
- [x] **The spec's deliverables 1 and 2: a design note, then a go/no-go on D1 vs D3** (SPEC.md §9).
  - Found that mechanism (a) is unsound on master, tested on a local build (NOTES.md M0.1).
  - Proposed mechanism (d), the VM page LSN, with a proof and a race table (ALTERNATIVES.md §2).
  - Go on D1 as a prototype, no-go on D3 in core, D2 rejected.
  - Overtaken the same day by the talk above. The note's content now lives in ALTERNATIVES.md, RESEARCH.md and NOTES.md M0 to M0.3.
