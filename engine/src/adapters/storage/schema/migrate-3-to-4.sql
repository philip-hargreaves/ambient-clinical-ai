-- Version 3 -> 4: documents gain two kinds, the anonymised case summary and
-- the clinician's appraisal reflection. SQLite cannot widen a CHECK in place,
-- so the table is rebuilt. Run with foreign keys off: dropping the old table
-- must not cascade into note_options.
CREATE TABLE documents_v4 (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    kind         TEXT    NOT NULL
                 CHECK (kind IN ('note', 'patient', 'translation', 'label',
                                 'summary', 'reflection')),
    language     TEXT    NOT NULL,
    payload      BLOB    NOT NULL,
    generated_at TEXT,
    edited_at    TEXT,
    PRIMARY KEY (session_id, kind)
);
INSERT INTO documents_v4
    SELECT session_id, kind, language, payload, generated_at, edited_at FROM documents;
DROP TABLE documents;
ALTER TABLE documents_v4 RENAME TO documents;
