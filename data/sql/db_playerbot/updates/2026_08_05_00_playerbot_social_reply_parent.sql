-- Interactive Playerbot Social Chat: preserve the exact parent event identity.
--
-- The value is intentionally nullable and has no foreign key. Retention may delete a parent before
-- its child, and the child must remain readable as a reply whose original line is no longer present.

SET @social_event_reply_parent_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_event'
    AND COLUMN_NAME = 'reply_to_event_public_id'
);

SET @ddl := IF(@social_event_reply_parent_exists = 0,
  'ALTER TABLE `playerbot_social_event` ADD COLUMN `reply_to_event_public_id` CHAR(36) DEFAULT NULL AFTER `thread_public_id`;',
  'SELECT "Column reply_to_event_public_id already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
