-- Interactive Playerbot Social Chat: index the zone filter path.
--
-- Adds one index to `playerbot_social_event`. It creates no table, alters no column, and reads or
-- backfills nothing.
--
-- Why this index exists.
--
-- The plan's durable model says every filter or ordering path is indexed. `zone_id` appears in
-- `ix_social_event_channel_zone (channel, zone_id, occurred_at)`, which serves a query that names a
-- channel, and only that. The Social feed exposes zone as a filter of its own, so a caller asking
-- for one zone across every channel had no index to use and fell back to scanning the primary key
-- backwards until the page filled.
--
-- The previous revision argued that fallback was acceptable because retention bounds the table and
-- no such query had been observed. That reasoning was wrong on the point that matters: the filter is
-- exposed by the API, so the query is not hypothetical, and a contract the plan states plainly is
-- not something an implementation gets to decide is unnecessary. Recorded here rather than quietly
-- corrected, because the earlier judgement is on the Task 12 checkpoint in writing.
--
-- The second column is `id` rather than `occurred_at`, matching `ix_social_event_type` and for the
-- same reason: the feed pages by the primary key, so an index whose trailing column is the ordering
-- the query uses satisfies the filter and the page together.
--
-- Idempotent by INFORMATION_SCHEMA guard rather than by ADD INDEX IF NOT EXISTS, which MySQL does
-- not support, following the pattern used by the revisions beside it.

SET @social_event_zone_index_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_event'
    AND INDEX_NAME = 'ix_social_event_zone'
);

SET @ddl := IF(@social_event_zone_index_exists = 0,
  'ALTER TABLE `playerbot_social_event` ADD KEY `ix_social_event_zone` (`zone_id`, `id`);',
  'SELECT "Index ix_social_event_zone already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
