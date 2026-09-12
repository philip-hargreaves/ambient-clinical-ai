-- Version 5 -> 6: documents carry the nonce sequence of their payload, so a
-- rewrite never reseals under a used IV, and the kind check admits guidance.
-- Rebuilt with foreign keys off: dropping the old table must not cascade into
-- note_options. Existing rows were sealed at 0.
CREATE TABLE documents_v6 (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    kind         TEXT    NOT NULL
                 CHECK (kind IN ('note', 'patient', 'translation', 'label',
                                 'summary', 'reflection', 'guidance')),
    seq          INTEGER NOT NULL DEFAULT 0,
    language     TEXT    NOT NULL,
    payload      BLOB    NOT NULL,
    generated_at TEXT,
    edited_at    TEXT,
    PRIMARY KEY (session_id, kind)
);
INSERT INTO documents_v6 (session_id, kind, language, payload, generated_at, edited_at)
    SELECT session_id, kind, language, payload, generated_at, edited_at FROM documents;
DROP TABLE documents;
ALTER TABLE documents_v6 RENAME TO documents;
