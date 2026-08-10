-- Interactive Playerbot Social Chat: durable schema (schema version 1).
--
-- Adds the social actor, profile, relationship, memory, event, consent, moderation, and runtime
-- control tables plus the Claude budget ledger. It creates new tables only and never reads, alters,
-- or backfills an existing Playerbots table. There is no legacy import and no seeded budget amount.
--
-- Runtime controls are intentionally not seeded. Until an operator persists the first change, the
-- social manager uses its configured defaults, so a fresh install and an untouched install behave
-- identically.
--
-- Database version and where the bounds are actually enforced. AzerothCore refuses to start below
-- MySQL 8.0.0 (MIN_MYSQL_SERVER_VERSION in src/server/database/Database/DatabaseWorkerPool.h), and
-- MySQL enforces CHECK constraints only from 8.0.16. On a server between 8.0.0 and 8.0.15 the CHECK
-- clauses below parse but do not reject, so they are defense in depth and not the primary guard.
-- The designated guard is PlayerbotSocialClampRelationship in src/Bot/Social/PlayerbotSocialTypes.h,
-- which clamps every relationship value into range and substitutes neutral for NaN. It is covered by
-- PlayerbotSocialContractTest but, as of this revision, has no production caller: the repository that
-- will call it arrives in Task 5, whose Definition of Done makes wiring it a required criterion.
-- Until then the bound is available rather than enforced, so on a server below 8.0.16 an out of range
-- value bound directly by a future writer would be accepted. Do not treat these CHECK clauses as the
-- reason that cannot happen.
--
-- Rollback order (rehearse against a disposable schema, never the live database). The AzerothCore
-- updater is forward only, so a live rollback means disabling the feature gate and restoring the
-- pre-update backup, or executing this exact removal under separate review. No existing Playerbots
-- table appears in this list:
--
--   DROP TABLE IF EXISTS `playerbot_claude_budget_reservation`;
--   DROP TABLE IF EXISTS `playerbot_claude_daily_budget`;
--   DROP TABLE IF EXISTS `playerbot_social_runtime_control`;
--   DROP TABLE IF EXISTS `playerbot_social_moderation_case`;
--   DROP TABLE IF EXISTS `playerbot_social_consent`;
--   DROP TABLE IF EXISTS `playerbot_social_event`;
--   DROP TABLE IF EXISTS `playerbot_social_memory`;
--   DROP TABLE IF EXISTS `playerbot_social_relationship`;
--   DROP TABLE IF EXISTS `playerbot_social_profile`;
--   DROP TABLE IF EXISTS `playerbot_social_actor`;

-- Maps an internal character GUID to the opaque public identity used outside the worldserver.
-- The character GUID never leaves this table and is never part of a Medivh payload.
CREATE TABLE IF NOT EXISTS `playerbot_social_actor` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `public_id` CHAR(36) NOT NULL COMMENT 'Opaque act_ prefixed identity',
    `character_guid` INT UNSIGNED NOT NULL,
    `display_name` VARCHAR(12) NOT NULL,
    `actor_kind` ENUM('bot', 'player') NOT NULL,
    `last_seen_at` DATETIME NOT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_social_actor_public_id` (`public_id`),
    UNIQUE KEY `uk_social_actor_character_guid` (`character_guid`),
    KEY `ix_social_actor_kind_last_seen` (`actor_kind`, `last_seen_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Opaque public identity for every character the social feature has observed';

-- One row per bot. Social traits and biography are versioned structured documents validated in C++
-- before they are written, so a model proposal can never widen the stored shape.
CREATE TABLE IF NOT EXISTS `playerbot_social_profile` (
    `bot_actor_id` INT UNSIGNED NOT NULL,
    `schema_version` INT UNSIGNED NOT NULL,
    `traits_version` INT UNSIGNED NOT NULL DEFAULT 1,
    `social_traits` JSON DEFAULT NULL,
    `biography_state` ENUM('absent', 'pending', 'ready', 'retryable_failure') NOT NULL DEFAULT 'absent',
    `biography` JSON DEFAULT NULL,
    `biography_generated_at` DATETIME DEFAULT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`bot_actor_id`),
    KEY `ix_social_profile_biography_state` (`biography_state`),
    CONSTRAINT `ck_social_profile_schema_version` CHECK (`schema_version` >= 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Persisted social traits and stable biography for one bot';

-- Directional: the pair (bot, subject) is distinct from (subject, bot). Bot A liking a character
-- says nothing about what that character's own bot relationship looks like, or about bot B.
CREATE TABLE IF NOT EXISTS `playerbot_social_relationship` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `public_id` CHAR(36) NOT NULL COMMENT 'Opaque rel_ prefixed identity',
    `bot_actor_id` INT UNSIGNED NOT NULL,
    `subject_actor_id` INT UNSIGNED NOT NULL,
    `familiarity` FLOAT NOT NULL DEFAULT 0,
    `affinity` FLOAT NOT NULL DEFAULT 0,
    `trust` FLOAT NOT NULL DEFAULT 0,
    `interaction_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `last_interaction_at` DATETIME DEFAULT NULL,
    `last_decay_at` DATETIME DEFAULT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_social_relationship_public_id` (`public_id`),
    UNIQUE KEY `uk_social_relationship_pair` (`bot_actor_id`, `subject_actor_id`),
    KEY `ix_social_relationship_subject` (`subject_actor_id`),
    KEY `ix_social_relationship_decay` (`last_decay_at`),
    CONSTRAINT `ck_social_relationship_familiarity` CHECK (`familiarity` >= 0 AND `familiarity` <= 1),
    CONSTRAINT `ck_social_relationship_affinity` CHECK (`affinity` >= -1 AND `affinity` <= 1),
    CONSTRAINT `ck_social_relationship_trust` CHECK (`trust` >= -1 AND `trust` <= 1),
    CONSTRAINT `ck_social_relationship_directional` CHECK (`bot_actor_id` <> `subject_actor_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Bounded directional familiarity, affinity, and trust from one bot toward one character';

-- Private per-bot memory. Content is a paraphrase, never raw chat, credentials, contact details, or
-- exact slur text. privacy_scope is the channel lattice guard: a party or whisper fact can never be
-- retrieved for a General or say request.
CREATE TABLE IF NOT EXISTS `playerbot_social_memory` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `public_id` CHAR(36) NOT NULL COMMENT 'Opaque mem_ prefixed identity',
    `bot_actor_id` INT UNSIGNED NOT NULL,
    `subject_actor_id` INT UNSIGNED DEFAULT NULL,
    `category` ENUM('fact', 'impression', 'interaction', 'event') NOT NULL,
    `content` VARCHAR(512) NOT NULL,
    `provenance` ENUM('participated', 'addressed', 'hearsay', 'assistance', 'pvp') NOT NULL,
    `confidence` FLOAT NOT NULL DEFAULT 0,
    `significance` FLOAT NOT NULL DEFAULT 0,
    `privacy_scope` ENUM('public', 'party', 'whisper') NOT NULL,
    `expires_at` DATETIME DEFAULT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_social_memory_public_id` (`public_id`),
    KEY `ix_social_memory_retrieval` (`bot_actor_id`, `privacy_scope`, `significance`, `created_at`),
    KEY `ix_social_memory_owner_subject` (`bot_actor_id`, `subject_actor_id`),
    KEY `ix_social_memory_expiry` (`expires_at`),
    CONSTRAINT `ck_social_memory_confidence` CHECK (`confidence` >= 0 AND `confidence` <= 1),
    CONSTRAINT `ck_social_memory_significance` CHECK (`significance` >= 0 AND `significance` <= 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Paraphrased private memory owned by one bot, scoped by channel privacy';

-- Rolling raw telemetry and the only place retained message text exists. Purging this table must
-- never break a relationship, memory, or moderation record, so nothing else stores raw text.
-- event_type carries a versioned vocabulary that grows across the feature and is validated in C++;
-- the genuinely closed dimensions are constrained here as enums.
CREATE TABLE IF NOT EXISTS `playerbot_social_event` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'Internal collector cursor',
    `public_id` CHAR(36) NOT NULL COMMENT 'Opaque evt_ prefixed identity',
    `thread_public_id` CHAR(36) DEFAULT NULL COMMENT 'Opaque thr_ prefixed identity',
    `schema_version` INT UNSIGNED NOT NULL,
    `event_type` VARCHAR(48) NOT NULL,
    `origin` ENUM('social', 'combat_status', 'party_status', 'legacy', 'assistance', 'pvp',
        'control', 'system') NOT NULL,
    `channel` ENUM('general', 'say', 'party', 'whisper') DEFAULT NULL,
    `zone_id` INT UNSIGNED DEFAULT NULL,
    `actor_id` INT UNSIGNED DEFAULT NULL,
    `target_actor_id` INT UNSIGNED DEFAULT NULL,
    `bot_actor_id` INT UNSIGNED DEFAULT NULL,
    `outcome` ENUM('delivered', 'suppressed', 'failed', 'recorded') NOT NULL,
    `reason` VARCHAR(64) DEFAULT NULL,
    `message_text` VARCHAR(512) DEFAULT NULL COMMENT 'Retained chat text, never a prompt or secret',
    `diagnostics` JSON DEFAULT NULL COMMENT 'Bounded safe metadata, never raw prompts or credentials',
    `occurred_at` DATETIME NOT NULL,
    `expires_at` DATETIME NOT NULL,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_social_event_public_id` (`public_id`),
    KEY `ix_social_event_thread` (`thread_public_id`, `occurred_at`),
    KEY `ix_social_event_occurred` (`occurred_at`),
    KEY `ix_social_event_expiry` (`expires_at`),
    KEY `ix_social_event_channel_zone` (`channel`, `zone_id`, `occurred_at`),
    KEY `ix_social_event_actor` (`actor_id`, `occurred_at`),
    KEY `ix_social_event_bot` (`bot_actor_id`, `occurred_at`),
    KEY `ix_social_event_outcome` (`outcome`, `occurred_at`),
    CONSTRAINT `ck_social_event_schema_version` CHECK (`schema_version` >= 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Rolling social telemetry with a hard minimum 48 hour raw text retention';

-- Keyed by character GUID rather than by actor, because an opted out character must cause no
-- persistent social read or write at all, including the creation of an actor row.
CREATE TABLE IF NOT EXISTS `playerbot_social_consent` (
    `character_guid` INT UNSIGNED NOT NULL,
    `opted_out` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`character_guid`),
    CONSTRAINT `ck_social_consent_opted_out` CHECK (`opted_out` IN (0, 1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Global per-character social opt out';

-- Sanitized and grouped. One row per subject and objective category, so repeated behavior increments
-- a count instead of accumulating rows. Evidence never contains the exact offending text.
CREATE TABLE IF NOT EXISTS `playerbot_social_moderation_case` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `public_id` CHAR(36) NOT NULL COMMENT 'Opaque cas_ prefixed identity',
    `subject_actor_id` INT UNSIGNED NOT NULL,
    `category` ENUM('slur', 'threat', 'sexual_degradation', 'targeted_abuse',
        'instruction_leak_attempt') NOT NULL,
    `occurrence_count` INT UNSIGNED NOT NULL DEFAULT 1,
    `first_occurred_at` DATETIME NOT NULL,
    `last_occurred_at` DATETIME NOT NULL,
    `status` ENUM('open', 'acknowledged') NOT NULL DEFAULT 'open',
    `evidence` JSON DEFAULT NULL COMMENT 'Sanitized categories and counts, never raw offending text',
    `acknowledged_at` DATETIME DEFAULT NULL,
    `acknowledged_by` VARCHAR(64) DEFAULT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_social_moderation_public_id` (`public_id`),
    UNIQUE KEY `uk_social_moderation_subject_category` (`subject_actor_id`, `category`),
    KEY `ix_social_moderation_status` (`status`, `last_occurred_at`),
    CONSTRAINT `ck_social_moderation_occurrence` CHECK (`occurrence_count` >= 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Sanitized grouped abuse cases that outlive raw telemetry and never auto-sanction';

-- Singleton. The budget circuit breaker lives here rather than on the daily budget row on purpose:
-- an integrity incident must keep admission closed until an operator clears it, not reopen silently
-- at the next UTC day rollover.
CREATE TABLE IF NOT EXISTS `playerbot_social_runtime_control` (
    `id` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `paused` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `density_profile` ENUM('quiet', 'normal', 'lively') NOT NULL DEFAULT 'normal',
    `general_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `say_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `party_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `whisper_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `budget_circuit_open` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `budget_circuit_reason` VARCHAR(128) DEFAULT NULL,
    `budget_circuit_opened_at` DATETIME DEFAULT NULL,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    CONSTRAINT `ck_social_runtime_control_singleton` CHECK (`id` = 1),
    CONSTRAINT `ck_social_runtime_control_paused` CHECK (`paused` IN (0, 1)),
    CONSTRAINT `ck_social_runtime_control_circuit` CHECK (`budget_circuit_open` IN (0, 1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Persisted operator controls reapplied on restart';

-- One row per UTC day. The admission transaction locks this row before comparing reserved plus spent
-- against the configured ceiling. The ledger starts empty: no amount is seeded and nothing is
-- imported from the retired sidecar SQLite file.
CREATE TABLE IF NOT EXISTS `playerbot_claude_daily_budget` (
    `budget_date` DATE NOT NULL COMMENT 'UTC day',
    `reserved_usd` DECIMAL(12, 6) NOT NULL DEFAULT 0,
    `spent_usd` DECIMAL(12, 6) NOT NULL DEFAULT 0,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`budget_date`),
    CONSTRAINT `ck_claude_daily_budget_reserved` CHECK (`reserved_usd` >= 0),
    CONSTRAINT `ck_claude_daily_budget_spent` CHECK (`spent_usd` >= 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Locked daily aggregate for Claude spend admission';

-- One row per model attempt, including each retry. actual_cost_usd is deliberately NOT constrained
-- to be at most max_cost_usd: an impossible provider report must be recorded truthfully so it can
-- open the circuit breaker, rather than being rejected or silently truncated.
CREATE TABLE IF NOT EXISTS `playerbot_claude_budget_reservation` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `public_id` CHAR(36) NOT NULL COMMENT 'Opaque req_ prefixed identity',
    `budget_date` DATE NOT NULL COMMENT 'UTC day',
    `request_kind` ENUM('chat_response', 'backstory_generation', 'memory_extraction',
        'moderation_classification', 'career_generation') NOT NULL,
    `priority_lane` ENUM('direct_human', 'mixed_human_bot', 'career_generation',
        'bot_only_continuation', 'new_starter', 'background_extraction') NOT NULL,
    `model` VARCHAR(64) NOT NULL,
    `max_cost_usd` DECIMAL(12, 6) NOT NULL,
    `actual_cost_usd` DECIMAL(12, 6) DEFAULT NULL,
    `state` ENUM('reserved', 'completed', 'released', 'expired') NOT NULL DEFAULT 'reserved',
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `expires_at` DATETIME NOT NULL,
    `settled_at` DATETIME DEFAULT NULL,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_claude_reservation_public_id` (`public_id`),
    KEY `ix_claude_reservation_day_state` (`budget_date`, `state`),
    KEY `ix_claude_reservation_expiry` (`state`, `expires_at`),
    CONSTRAINT `ck_claude_reservation_max_cost` CHECK (`max_cost_usd` >= 0),
    CONSTRAINT `ck_claude_reservation_actual_cost` CHECK (`actual_cost_usd` IS NULL OR `actual_cost_usd` >= 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='One conservative maximum reservation per Claude request attempt';
