--source suite/versioning/key_type.inc
--require suite/versioning/primary_or_unique.require
disable_query_log;
eval select "$KEY_TYPE" != "key" as pk_or_unique;
enable_query_log;
