# Project file compatibility and backup policy

## Supported format

Legacy projects retain `schema_version: 1`. Managed authoring nodes use schema 2 with per-node exact package ID, content hash, relative package path and wrapper version. A project stores its project ID, name, compute revision, selected output node, nodes and their layout coordinates and parameters, and graph connections. Resource references are UTF-8 strings: relative paths resolve from the project file's directory and absolute paths remain absolute.

The model stores only values. It does not persist processor instances, JUCE objects, views, or GUI pointers. Unknown project, node, connection, and non-scalar parameter data is retained as opaque JSON so a host without a plugin can preserve the data without interpreting it. A missing plugin is reported and prevents execution.

## Loading and migration

Schemas 1 and 2 are read and written. Loading does not migrate or rewrite a project. Other versions return `project_schema_unsupported`; input bytes remain unchanged and loading does not create a backup. Schema 1 stays legacy unless a managed node is explicitly introduced. Exact locks cannot fall back to a type-only or latest replacement. The frozen schema-v1 fixture remains the compatibility reference; an independently retained schema-1-only CLI rejects schema 2 explicitly (NA0005 evidence).

## Saving and backups

An ordinary schema-v1 save writes a temporary sibling and atomically replaces the project file. It does not create `.bak` or `.schema-1.bak` sidecars. If replacement fails, the original project bytes and reopened state remain intact. The NED-13.d file-lock fixture verifies that failure path.

The first save of schema 2 over an existing schema 1 file copies the original bytes to `<project-file>.schema-1.bak`, verifies that copy, writes and validates the migrated document separately, checks the original has not changed, and atomically replaces it. Conflicting backups are never overwritten. Any failure preserves the original; the backup remains after success. New files create no fictitious backup. NA0007 fault tests cover each transaction phase. The first-upgrade GUI prompt and portable managed-package export are still outstanding; this IO implementation does not claim those behaviors.
