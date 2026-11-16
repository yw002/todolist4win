-- REFERENCE ONLY - the application never reads or executes this file.
--
-- Version 1 -> 2 for the local SQLite data source. The authoritative implementation lives in
-- src/data/SqliteSchema.cpp (kTaskImageDdl / kTaskImageIndexDdl and EnsureDatabaseAndSchema).
-- This file exists for review and for a human operator who needs to inspect an offline database;
-- keeping two executable copies would create two sources of truth.
--
-- SQLite DDL is transactional: the whole sequence runs inside BEGIN IMMEDIATE ... COMMIT and is
-- rolled back on any failure. Every step is idempotent so an interrupted run can simply be replayed.
--
--   BEGIN IMMEDIATE;

-- 1. Column guard (SQLite 3.45 has no ADD COLUMN IF NOT EXISTS). Only run the ALTER when this
--    returns 0.
SELECT COUNT(*) FROM pragma_table_info('task') WHERE name = 'body_doc';
ALTER TABLE task ADD COLUMN body_doc TEXT NULL;

-- 2. PNG BLOB table. mime has no CHECK restricting it to image/png: future image formats are a
--    document-format concern, not a storage constraint. image_key is COLLATE BINARY so a lowercase
--    SHA-256 hex key stays case-sensitive.
CREATE TABLE IF NOT EXISTS task_image (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id     INTEGER NOT NULL,
  image_key   TEXT    NOT NULL COLLATE BINARY,
  mime        TEXT    NOT NULL DEFAULT 'image/png',
  width       INTEGER NOT NULL CHECK (width  > 0 AND width  <= 8192),
  height      INTEGER NOT NULL CHECK (height > 0 AND height <= 8192),
  byte_size   INTEGER NOT NULL CHECK (byte_size > 0 AND byte_size <= 4194304),
  data        BLOB    NOT NULL CHECK (length(data) = byte_size),
  created_at  TEXT    NOT NULL,
  CONSTRAINT fk_task_image_task FOREIGN KEY (task_id)
      REFERENCES task(id) ON DELETE CASCADE ON UPDATE RESTRICT,
  CONSTRAINT uq_task_image_key  UNIQUE (task_id, image_key),
  CONSTRAINT ck_task_image_key  CHECK (length(image_key) = 64)
);
CREATE INDEX IF NOT EXISTS ix_task_image_task ON task_image(task_id);

-- 3. Version rows (written LAST, after every DDL step succeeded).
INSERT OR IGNORE INTO schema_migration(version, applied_at)
  VALUES (1, datetime('now','localtime'));
INSERT OR IGNORE INTO schema_migration(version, applied_at)
  VALUES (2, datetime('now','localtime'));

--   COMMIT;
-- 4. user_version is written AFTER the commit, outside the transaction.
PRAGMA user_version = 2;

-- No UPDATE, DELETE or DROP of user data appears anywhere in this migration. A v1 task row keeps
-- its note verbatim and its body_doc NULL until the user explicitly edits and saves that task.
