-- One row per transaction. Not HOT (k has a btree), so the delete callback runs.
\set id random(1, 1000000)
UPDATE {{T}} SET k = k + 1 WHERE id = :id;
