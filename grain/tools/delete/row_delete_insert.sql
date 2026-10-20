-- One row per transaction; put back so the table keeps its size.
\set id random(1, 1000000)
BEGIN;
DELETE FROM {{T}} WHERE id = :id;
INSERT INTO {{T}} VALUES (:id, :id, :id);
COMMIT;
