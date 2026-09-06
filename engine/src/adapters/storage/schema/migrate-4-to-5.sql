-- Version 4 -> 5: a session may be a sample, seeded for demonstration and
-- removable in one go without touching a real record.
ALTER TABLE sessions ADD COLUMN demo INTEGER NOT NULL DEFAULT 0;
