-- Grounded Social V2 profile compatibility.
--
-- Version 2 stored traits use the same bounded shape as version 3. Their biographies do not:
-- version 2 allowed generated gameplay claims that version 3 intentionally invalidated. Migrate
-- only the coherent version 2 pair, preserve social_traits byte for byte, and return biography
-- generation to its ordinary absent state. Mixed and future rows remain untouched so the reader
-- can reject them explicitly.

UPDATE `playerbot_social_profile`
SET `schema_version` = 3,
    `traits_version` = 3,
    `biography_state` = 'absent',
    `biography_request_token` = 0,
    `biography_attempted_at` = NULL,
    `biography` = NULL,
    `biography_generated_at` = NULL
WHERE `schema_version` = 2
  AND `traits_version` = 2;
