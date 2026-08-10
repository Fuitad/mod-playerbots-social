-- Grounded Social V2 provenance for starter deliveries and durable factual memory.
--
-- Existing rows remain valid because every new column is nullable. Event and memory retention may
-- remove an originating event before its child, so the source identities deliberately have no
-- foreign keys.
--
-- Rollback instruction for a disabled Social V2 runtime and a separately reviewed maintenance
-- operation:
--   ALTER TABLE `playerbot_social_event` DROP COLUMN `source_event_public_id`;
--   ALTER TABLE `playerbot_social_memory` DROP COLUMN `source_kind`,
--     DROP COLUMN `source_thread_public_id`, DROP COLUMN `source_event_public_id`;

SET @social_event_source_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_event'
    AND COLUMN_NAME = 'source_event_public_id'
);

SET @ddl := IF(@social_event_source_exists = 0,
  'ALTER TABLE `playerbot_social_event` ADD COLUMN `source_event_public_id` CHAR(36) DEFAULT NULL AFTER `reply_to_event_public_id`;',
  'SELECT "Column source_event_public_id already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @social_memory_source_event_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_memory'
    AND COLUMN_NAME = 'source_event_public_id'
);

SET @ddl := IF(@social_memory_source_event_exists = 0,
  'ALTER TABLE `playerbot_social_memory` ADD COLUMN `source_event_public_id` CHAR(36) DEFAULT NULL AFTER `privacy_scope`;',
  'SELECT "Column source_event_public_id already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @social_memory_source_thread_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_memory'
    AND COLUMN_NAME = 'source_thread_public_id'
);

SET @ddl := IF(@social_memory_source_thread_exists = 0,
  'ALTER TABLE `playerbot_social_memory` ADD COLUMN `source_thread_public_id` CHAR(36) DEFAULT NULL AFTER `source_event_public_id`;',
  'SELECT "Column source_thread_public_id already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @social_memory_source_kind_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_memory'
    AND COLUMN_NAME = 'source_kind'
);

SET @ddl := IF(@social_memory_source_kind_exists = 0,
  'ALTER TABLE `playerbot_social_memory` ADD COLUMN `source_kind` ENUM(''human_observation'', ''authoritative_source'') DEFAULT NULL AFTER `source_thread_public_id`;',
  'SELECT "Column source_kind already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
