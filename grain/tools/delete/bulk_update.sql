-- Every row, rolled back so the table stays the same.
BEGIN;
UPDATE {{T}} SET k = k + 1;
ROLLBACK;
