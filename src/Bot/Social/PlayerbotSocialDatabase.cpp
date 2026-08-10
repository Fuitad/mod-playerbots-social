#include "Bot/Social/PlayerbotSocialDatabase.h"

#include <memory>
#include <stdexcept>
#include <string_view>

#include "DatabaseEnv.h"

namespace
{
std::string_view StatementSql(PlayerbotSocialStatementId id)
{
    switch (id)
    {
        case PLAYERBOT_SOCIAL_STMT_DEL_ACTOR_BY_GUID:
            return "DELETE FROM playerbot_social_actor WHERE character_guid = ?";
        case PLAYERBOT_SOCIAL_STMT_DEL_EVENT_EXPIRED:
            return "DELETE FROM playerbot_social_event WHERE expires_at <= FROM_UNIXTIME(?) LIMIT ?";
        case PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_BOT:
            return "DELETE FROM playerbot_social_memory WHERE bot_actor_id = ?";
        case PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_PUBLIC_ID:
            return "DELETE FROM playerbot_social_memory WHERE public_id = ?";
        case PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_SUBJECT:
            return "DELETE FROM playerbot_social_memory WHERE subject_actor_id = ?";
        case PLAYERBOT_SOCIAL_STMT_DEL_PROFILE_BY_BOT:
            return "DELETE FROM playerbot_social_profile WHERE bot_actor_id = ?";
        case PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_BOT:
            return "DELETE FROM playerbot_social_relationship WHERE bot_actor_id = ?";
        case PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_PUBLIC_ID:
            return "DELETE FROM playerbot_social_relationship WHERE public_id = ?";
        case PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_SUBJECT:
            return "DELETE FROM playerbot_social_relationship WHERE subject_actor_id = ?";
        case PLAYERBOT_SOCIAL_STMT_INS_ACTOR:
            return "INSERT INTO playerbot_social_actor (public_id, character_guid, display_name, actor_kind, "
                   "last_seen_at) VALUES (?, ?, ?, ?, FROM_UNIXTIME(?)) ON DUPLICATE KEY UPDATE "
                   "display_name = VALUES(display_name), last_seen_at = VALUES(last_seen_at)";
        case PLAYERBOT_SOCIAL_STMT_INS_CONSENT:
            return "INSERT INTO playerbot_social_consent (character_guid, opted_out) VALUES (?, ?) "
                   "ON DUPLICATE KEY UPDATE opted_out = VALUES(opted_out)";
        case PLAYERBOT_SOCIAL_STMT_INS_EVENT:
            return "INSERT INTO playerbot_social_event (public_id, thread_public_id, reply_to_event_public_id, "
                   "source_event_public_id, schema_version, event_type, origin, channel, zone_id, actor_id, "
                   "target_actor_id, bot_actor_id, outcome, reason, message_text, diagnostics, occurred_at, "
                   "expires_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?), "
                   "FROM_UNIXTIME(?))";
        case PLAYERBOT_SOCIAL_STMT_INS_MEMORY:
            return "INSERT INTO playerbot_social_memory (public_id, bot_actor_id, subject_actor_id, category, "
                   "content, provenance, confidence, significance, privacy_scope, source_event_public_id, "
                   "source_thread_public_id, source_kind, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                   "FROM_UNIXTIME(?))";
        case PLAYERBOT_SOCIAL_STMT_INS_PROFILE:
            return "INSERT INTO playerbot_social_profile (bot_actor_id, schema_version, traits_version, "
                   "biography_state, biography_request_token, biography_attempted_at, biography, "
                   "biography_generated_at) VALUES (?, ?, ?, ?, ?, FROM_UNIXTIME(?), ?, "
                   "IF(? = 'ready', NOW(), NULL)) ON DUPLICATE KEY UPDATE schema_version = VALUES(schema_version), "
                   "biography_state = VALUES(biography_state), biography_request_token = "
                   "VALUES(biography_request_token), biography_attempted_at = VALUES(biography_attempted_at), "
                   "biography = VALUES(biography), biography_generated_at = VALUES(biography_generated_at)";
        case PLAYERBOT_SOCIAL_STMT_INS_RELATIONSHIP:
            return "INSERT INTO playerbot_social_relationship (public_id, bot_actor_id, subject_actor_id, "
                   "familiarity, affinity, trust, interaction_count, last_interaction_at) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?)) ON DUPLICATE KEY UPDATE familiarity = "
                   "VALUES(familiarity), affinity = VALUES(affinity), trust = VALUES(trust), interaction_count = "
                   "interaction_count + VALUES(interaction_count), last_interaction_at = "
                   "VALUES(last_interaction_at)";
        case PLAYERBOT_SOCIAL_STMT_INS_RELATIONSHIP_DELTA:
            return "INSERT INTO playerbot_social_relationship (public_id, bot_actor_id, subject_actor_id, "
                   "familiarity, affinity, trust, interaction_count, last_interaction_at) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?)) ON DUPLICATE KEY UPDATE familiarity = "
                   "LEAST(1, GREATEST(0, familiarity + VALUES(familiarity))), affinity = "
                   "LEAST(1, GREATEST(-1, affinity + VALUES(affinity))), trust = "
                   "LEAST(1, GREATEST(-1, trust + VALUES(trust))), interaction_count = interaction_count + "
                   "VALUES(interaction_count), last_interaction_at = VALUES(last_interaction_at)";
        case PLAYERBOT_SOCIAL_STMT_INS_RUNTIME_CONTROL:
            return "INSERT INTO playerbot_social_runtime_control (id, paused, density_profile, general_enabled, "
                   "say_enabled, party_enabled, whisper_enabled, budget_circuit_open, budget_circuit_reason, "
                   "budget_circuit_opened_at) VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?)) "
                   "ON DUPLICATE KEY UPDATE paused = VALUES(paused), density_profile = VALUES(density_profile), "
                   "general_enabled = VALUES(general_enabled), say_enabled = VALUES(say_enabled), "
                   "party_enabled = VALUES(party_enabled), whisper_enabled = VALUES(whisper_enabled)";
        case PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_BY_GUID:
            return "SELECT id, public_id, display_name, actor_kind, last_seen_at FROM playerbot_social_actor "
                   "WHERE character_guid = ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_ID_BY_GUID:
        case PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_ID_FOR_COHORT_PURGE:
            return "SELECT COUNT(*), COALESCE(MIN(id), 0) FROM playerbot_social_actor WHERE character_guid = ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_CONSENT:
            return "SELECT COUNT(*), COALESCE(MAX(opted_out), 0) FROM playerbot_social_consent "
                   "WHERE character_guid = ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_ANY_SCOPE:
            return "SELECT public_id, subject_actor_id, category, content, provenance, confidence, significance, "
                   "privacy_scope, source_event_public_id, source_thread_public_id, source_kind "
                   "FROM playerbot_social_memory WHERE bot_actor_id = ? AND "
                   "(expires_at IS NULL OR expires_at > FROM_UNIXTIME(?)) "
                   "ORDER BY significance DESC, created_at DESC LIMIT ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_OWNER_BY_PUBLIC_ID:
            return "SELECT bot_actor_id FROM playerbot_social_memory WHERE public_id = ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_PARTY_SCOPE:
            return "SELECT public_id, subject_actor_id, category, content, provenance, confidence, significance, "
                   "privacy_scope, source_event_public_id, source_thread_public_id, source_kind "
                   "FROM playerbot_social_memory WHERE bot_actor_id = ? AND privacy_scope IN ('public', 'party') "
                   "AND (expires_at IS NULL OR expires_at > FROM_UNIXTIME(?)) "
                   "ORDER BY significance DESC, created_at DESC LIMIT ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_PUBLIC_SCOPE:
            return "SELECT public_id, subject_actor_id, category, content, provenance, confidence, significance, "
                   "privacy_scope, source_event_public_id, source_thread_public_id, source_kind "
                   "FROM playerbot_social_memory WHERE bot_actor_id = ? AND privacy_scope = 'public' "
                   "AND (expires_at IS NULL OR expires_at > FROM_UNIXTIME(?)) "
                   "ORDER BY significance DESC, created_at DESC LIMIT ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_MODERATION_CASE_EXISTS_BY_PUBLIC_ID:
            return "SELECT 1 FROM playerbot_social_moderation_case WHERE public_id = ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_PROFILE:
            return "SELECT (p.bot_actor_id IS NOT NULL), p.schema_version, p.traits_version, "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.warmth')), '50'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.talkativeness')), '50'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.curiosity')), '50'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.humor')), '50'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.formality')), '50'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.last_evolved_at')), '0'), "
                   "COALESCE(JSON_LENGTH(JSON_EXTRACT(p.social_traits, '$.interests')), 0), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.interests[0]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.interests[1]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.interests[2]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.interests[3]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.interests[4]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.interests[5]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.interests[6]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.interests[7]')), ''), "
                   "COALESCE(JSON_LENGTH(JSON_EXTRACT(p.social_traits, '$.aversions')), 0), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.aversions[0]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.aversions[1]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.aversions[2]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.aversions[3]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.aversions[4]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.aversions[5]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.aversions[6]')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.social_traits, '$.aversions[7]')), ''), "
                   "p.biography_state, p.biography_request_token, "
                   "COALESCE(UNIX_TIMESTAMP(p.biography_attempted_at), 0), (p.biography IS NOT NULL), "
                   "COALESCE(JSON_CONTAINS_PATH(p.biography, 'all', '$.version', '$.character_name', '$.race_id', "
                   "'$.class_id', '$.gender_id', '$.origin', '$.motivation', '$.formative_experience', "
                   "'$.interests', '$.aversions', '$.preferred_topics', '$.mannerisms', '$.values'), 0), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.version')), '0'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.character_name')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.race_id')), '0'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.class_id')), '0'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.gender_id')), '0'), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.origin')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.motivation')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.formative_experience')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.interests')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.aversions')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.preferred_topics')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.mannerisms')), ''), "
                   "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p.biography, '$.values')), '') FROM (SELECT 1) seed "
                   "LEFT JOIN playerbot_social_actor a ON a.character_guid = ? "
                   "LEFT JOIN playerbot_social_profile p ON p.bot_actor_id = a.id";
        case PLAYERBOT_SOCIAL_STMT_SEL_RELATIONSHIP:
            return "SELECT public_id, familiarity, affinity, trust, interaction_count, last_interaction_at "
                   "FROM playerbot_social_relationship WHERE bot_actor_id = ? AND subject_actor_id = ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_RELATIONSHIP_EXISTS_BY_PUBLIC_ID:
            return "SELECT 1 FROM playerbot_social_relationship WHERE public_id = ?";
        case PLAYERBOT_SOCIAL_STMT_SEL_RUNTIME_CONTROL:
            return "SELECT paused, density_profile, general_enabled, say_enabled, party_enabled, whisper_enabled, "
                   "budget_circuit_open, budget_circuit_reason, budget_circuit_opened_at "
                   "FROM playerbot_social_runtime_control WHERE id = 1";
        case PLAYERBOT_SOCIAL_STMT_UPD_MODERATION_CASE_ACK:
            return "UPDATE playerbot_social_moderation_case SET status = 'acknowledged', "
                   "acknowledged_at = FROM_UNIXTIME(?), acknowledged_by = ? WHERE public_id = ?";
        case PLAYERBOT_SOCIAL_STMT_UPD_PROFILE_TRAITS:
            return "INSERT INTO playerbot_social_profile (bot_actor_id, schema_version, traits_version, "
                   "social_traits, biography_state) VALUES (?, ?, ?, ?, 'absent') ON DUPLICATE KEY UPDATE "
                   "schema_version = VALUES(schema_version), traits_version = VALUES(traits_version), "
                   "social_traits = VALUES(social_traits)";
    }
    throw std::logic_error("unknown Playerbots Social statement");
}
}  // namespace

void PlayerbotSocialPreparedStatement::SetLiteral(std::size_t index, std::string value)
{
    if (_parameters.size() <= index)
        _parameters.resize(index + 1);
    _parameters[index] = std::move(value);
}

void PlayerbotSocialPreparedStatement::SetData(std::size_t index, std::nullptr_t) { SetLiteral(index, "NULL"); }

void PlayerbotSocialPreparedStatement::SetData(std::size_t index, std::string const& value)
{
    std::string escaped = value;
    PlayerbotsDatabase.EscapeString(escaped);
    SetLiteral(index, "'" + escaped + "'");
}

void PlayerbotSocialPreparedStatement::SetData(std::size_t index, std::string_view value)
{
    SetData(index, std::string(value));
}

void PlayerbotSocialPreparedStatement::SetData(std::size_t index, char const* value)
{
    if (value)
        SetData(index, std::string(value));
    else
        SetData(index, nullptr);
}

std::string PlayerbotSocialPreparedStatement::Build() const
{
    std::string sql(StatementSql(_id));
    std::size_t searchFrom = 0;
    for (std::string const& parameter : _parameters)
    {
        std::size_t const placeholder = sql.find('?', searchFrom);
        if (placeholder == std::string::npos)
            throw std::logic_error("too many Playerbots Social statement parameters");
        sql.replace(placeholder, 1, parameter);
        searchFrom = placeholder + parameter.size();
    }
    if (sql.find('?', searchFrom) != std::string::npos)
        throw std::logic_error("missing Playerbots Social statement parameter");
    return sql;
}

PlayerbotSocialPreparedStatement* NewPlayerbotSocialStatement(PlayerbotSocialStatementId id)
{
    return new PlayerbotSocialPreparedStatement(id);
}

std::string ConsumePlayerbotSocialSql(PlayerbotSocialPreparedStatement* statement)
{
    std::unique_ptr<PlayerbotSocialPreparedStatement> owned(statement);
    return owned->Build();
}

void PlayerbotSocialExecute(PlayerbotSocialPreparedStatement* statement)
{
    PlayerbotsDatabase.Execute(ConsumePlayerbotSocialSql(statement));
}

QueryResult PlayerbotSocialQuery(PlayerbotSocialPreparedStatement* statement)
{
    return PlayerbotsDatabase.Query(ConsumePlayerbotSocialSql(statement));
}

QueryCallback PlayerbotSocialAsyncQuery(PlayerbotSocialPreparedStatement* statement)
{
    return PlayerbotsDatabase.AsyncQuery(ConsumePlayerbotSocialSql(statement));
}
