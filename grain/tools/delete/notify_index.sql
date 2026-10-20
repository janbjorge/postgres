-- An index whose AM asks for the delete callback. dummy_index_am is only
-- installed in the patched build.
CREATE EXTENSION IF NOT EXISTS dummy_index_am;
CREATE INDEX ON {{T}} USING dummy_index_am (v);
