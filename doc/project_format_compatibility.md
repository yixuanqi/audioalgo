# Project file compatibility and backup policy

## Supported format

The current persisted `schema_version` is `1`. A schema-v1 project stores its project ID, name, compute revision, selected output node, nodes and their layout coordinates and parameters, and graph connections. Resource references are UTF-8 strings: relative paths resolve from the project file's directory and absolute paths remain absolute.

The model stores only values. It does not persist processor instances, JUCE objects, views, or GUI pointers. Unknown project, node, connection, and non-scalar parameter data is retained as opaque JSON so a host without a plugin can preserve the data without interpreting it. A missing plugin is reported and prevents execution.

## Loading and migration

Schema 1 is the only schema currently read and written. Loading does not migrate or rewrite a project. A schema version other than 1 returns `project_schema_unsupported`; the input file's bytes remain unchanged and loading does not create a backup. The frozen schema-v1 fixture is the compatibility reference for save, reopen, and run behavior.

## Saving and backups

An ordinary schema-v1 save writes a temporary sibling and atomically replaces the project file. It does not create `.bak` or `.schema-1.bak` sidecars. If replacement fails, the original project bytes and reopened state remain intact. The NED-13.d file-lock fixture verifies that failure path.

No schema migration is implemented today, so there is no migration backup to produce. Before a future migration changes schema `N`, the migration must copy the original bytes to `<project-file>.schema-N.bak`, verify that copy, write and validate the migrated document separately, and only then atomically replace the project. If backup creation or validation fails, the original project must remain untouched. The backup is retained after a successful migration.
