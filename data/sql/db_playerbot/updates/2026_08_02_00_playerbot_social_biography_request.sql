-- Interactive Playerbot Social Chat: durable biography request identity.
--
-- Adds the two columns a biography request needs to survive a worldserver restart. It alters one
-- existing table, adds columns only, and never drops, renames, reads or backfills anything.
--
-- Why both columns, and why durable.
--
-- `playerbot_social_profile.biography_state` is already durable, and a request in flight is
-- recorded as 'pending'. That state alone gives exactly one of the two guarantees the feature
-- needs. Requiring the profile to still be pending before a completion is applied does stop a
-- completion ever replacing a biography that is already ready. It does NOT identify WHICH request
-- a completion answers, so after the pending timeout and a fresh request, a very late reply to the
-- superseded call still finds the profile pending and would be accepted.
--
-- `biography_request_token` closes that. It is minted per request, sent to the provider, and echoed
-- back, so a completion carrying a token the profile is no longer waiting on is discarded.
--
-- `biography_attempted_at` is what makes the timeout evaluable at all. It is the single timestamp
-- both waiting states measure from: a retryable failure waits out a short backoff and a pending
-- request waits out the longer window after which it is presumed abandoned. Without it, a restart
-- leaves a profile pending with no way to tell a request issued a minute ago from one issued last
-- week, so the request could never be retried and that bot would never get a biography.
--
-- Idempotent by INFORMATION_SCHEMA guard rather than by ADD COLUMN IF NOT EXISTS, which MySQL does
-- not support, following the pattern already used by 2025_04_26_00.sql in this directory.

SET @token_column_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_profile'
    AND COLUMN_NAME = 'biography_request_token'
);

SET @ddl := IF(@token_column_exists = 0,
  'ALTER TABLE `playerbot_social_profile` ADD COLUMN `biography_request_token` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''Identifies which request a completion answers; 0 means none is in flight'' AFTER `biography_state`;',
  'SELECT "Column biography_request_token already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @attempted_column_exists := (
  SELECT COUNT(1)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'playerbot_social_profile'
    AND COLUMN_NAME = 'biography_attempted_at'
);

SET @ddl := IF(@attempted_column_exists = 0,
  'ALTER TABLE `playerbot_social_profile` ADD COLUMN `biography_attempted_at` DATETIME DEFAULT NULL COMMENT ''When the last generation was attempted; both the retry backoff and the pending timeout measure from this'' AFTER `biography_request_token`;',
  'SELECT "Column biography_attempted_at already exists.";'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
