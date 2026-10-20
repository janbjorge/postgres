-- A 1M-row table with a primary key and one more btree.
DROP TABLE IF EXISTS {{T}};
CREATE TABLE {{T}} (id int4 PRIMARY KEY, k int4 NOT NULL, v int4 NOT NULL);
INSERT INTO {{T}} SELECT i, i, i FROM generate_series(1, 1000000) i;
CREATE INDEX ON {{T}} (k);
