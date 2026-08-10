-- Interactive Playerbot Social Chat: index the event type filter path.
--
-- Adds one index to `playerbot_social_event`. It creates no table, alters no column, and reads or
-- backfills nothing.
--
-- Why this index exists.
--
-- The plan's durable model says every filter or ordering path is indexed, and `event_type` was the
-- one that was not. Two callers filter on it and both grow with the table: the Medivh feed exposes
-- an event type filter, and the diagnostics projection counts `social.telemetry.gap` rows to report
-- queue pressure. Without a leading `event_type` index each of those is a scan of the whole rolling
-- telemetry table, which is the table that grows fastest in the entire feature.
--
-- Why (`event_type`, `id`) rather than (`event_type`, `occurred_at`), which is the shape the other
-- composites use. The feed pages by the primary key and not by time, because `id` is the collector's
-- cursor and is strictly increasing, while two events written in the same second have no defined
-- order under `occurred_at`. An index whose second column is the ordering the query actually uses
-- can satisfy the filter and the page together; one on `occurred_at` would satisfy only the filter
-- and leave a sort behind.
--
-- One index rather than several. `zone_id` alone and `outcome` alone are also filterable and are
-- covered only as the leading or second column of an existing composite, so a query that names
-- either without its partner still falls back to a bounded backward scan of the primary key. That is
-- left alone deliberately: this table is the hottest writer in the feature, retention bounds the
-- worst case, and adding index maintenance for a filter combination nobody has been observed using
-- would cost every insert to speed up a query that may never be issued.
--
-- Idempotent by INFORMATION_SCHEMA guard rather than by ADD INDEX IF NOT EXISTS, which MySQL does
-- not support, following the pattern used by 2026_08_02_00 in this directory.

SET @social_event_type_index_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_event'
    AND INDEX_NAME = 'ix_social_event_type'
);

SET @ddl := IF(@social_event_type_index_exists = 0,
  'ALTER TABLE `playerbot_social_event` ADD KEY `ix_social_event_type` (`event_type`, `id`);',
  'SELECT "Index ix_social_event_type already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
