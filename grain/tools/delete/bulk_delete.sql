-- Every row, rolled back so the table stays the same.
BEGIN;
DELETE FROM {{T}};
ROLLBACK;
