-- State artifacts are managed objects. Their physical locator is portable and
-- relative to DbConfigPaths::object_store_root; filename remains presentation
-- metadata. DBService reconciles and fills legacy rows before exposing StateDB.
ALTER TABLE state_artifact
ADD COLUMN object_relpath TEXT NOT NULL DEFAULT '';
