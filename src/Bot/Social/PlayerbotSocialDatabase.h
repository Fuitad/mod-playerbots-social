#ifndef PLAYERBOTS_PLAYERBOTSOCIALDATABASE_H
#define PLAYERBOTS_PLAYERBOTSOCIALDATABASE_H

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Define.h"
#include "QueryCallback.h"
#include "QueryResult.h"

enum PlayerbotSocialStatementId
{
    PLAYERBOT_SOCIAL_STMT_DEL_ACTOR_BY_GUID,
    PLAYERBOT_SOCIAL_STMT_DEL_EVENT_EXPIRED,
    PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_BOT,
    PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_PUBLIC_ID,
    PLAYERBOT_SOCIAL_STMT_DEL_MEMORY_BY_SUBJECT,
    PLAYERBOT_SOCIAL_STMT_DEL_PROFILE_BY_BOT,
    PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_BOT,
    PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_PUBLIC_ID,
    PLAYERBOT_SOCIAL_STMT_DEL_RELATIONSHIP_BY_SUBJECT,
    PLAYERBOT_SOCIAL_STMT_INS_ACTOR,
    PLAYERBOT_SOCIAL_STMT_INS_CONSENT,
    PLAYERBOT_SOCIAL_STMT_INS_EVENT,
    PLAYERBOT_SOCIAL_STMT_INS_MEMORY,
    PLAYERBOT_SOCIAL_STMT_INS_MODERATION_CASE,
    PLAYERBOT_SOCIAL_STMT_INS_PROFILE,
    PLAYERBOT_SOCIAL_STMT_INS_RELATIONSHIP,
    PLAYERBOT_SOCIAL_STMT_INS_RELATIONSHIP_DELTA,
    PLAYERBOT_SOCIAL_STMT_INS_RUNTIME_CONTROL,
    PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_BY_GUID,
    PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_ID_BY_GUID,
    PLAYERBOT_SOCIAL_STMT_SEL_ACTOR_ID_FOR_COHORT_PURGE,
    PLAYERBOT_SOCIAL_STMT_SEL_CONSENT,
    PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_ANY_SCOPE,
    PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_OWNER_BY_PUBLIC_ID,
    PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_PARTY_SCOPE,
    PLAYERBOT_SOCIAL_STMT_SEL_MEMORY_PUBLIC_SCOPE,
    PLAYERBOT_SOCIAL_STMT_SEL_MODERATION_CASE_EXISTS_BY_PUBLIC_ID,
    PLAYERBOT_SOCIAL_STMT_SEL_PROFILE,
    PLAYERBOT_SOCIAL_STMT_SEL_RELATIONSHIP,
    PLAYERBOT_SOCIAL_STMT_SEL_RELATIONSHIP_EXISTS_BY_PUBLIC_ID,
    PLAYERBOT_SOCIAL_STMT_SEL_RUNTIME_CONTROL,
    PLAYERBOT_SOCIAL_STMT_UPD_BUDGET_CIRCUIT,
    PLAYERBOT_SOCIAL_STMT_UPD_MODERATION_CASE_ACK,
    PLAYERBOT_SOCIAL_STMT_UPD_PROFILE_TRAITS
};

class PlayerbotSocialPreparedStatement
{
public:
    explicit PlayerbotSocialPreparedStatement(PlayerbotSocialStatementId id) : _id(id) {}

    void SetData(std::size_t index, std::nullptr_t);
    void SetData(std::size_t index, std::string const& value);
    void SetData(std::size_t index, std::string_view value);
    void SetData(std::size_t index, char const* value);

    template <typename T>
        requires std::is_arithmetic_v<T>
    void SetData(std::size_t index, T value)
    {
        if constexpr (std::is_same_v<T, bool>)
            SetLiteral(index, value ? "1" : "0");
        else if constexpr (std::is_floating_point_v<T>)
            SetLiteral(index, std::to_string(value));
        else
            SetLiteral(index,
                       std::to_string(static_cast<std::conditional_t<std::is_signed_v<T>, int64, uint64>>(value)));
    }

    [[nodiscard]] std::string Build() const;

private:
    void SetLiteral(std::size_t index, std::string value);

    PlayerbotSocialStatementId _id;
    std::vector<std::string> _parameters;
};

[[nodiscard]] PlayerbotSocialPreparedStatement* NewPlayerbotSocialStatement(PlayerbotSocialStatementId id);
void PlayerbotSocialExecute(PlayerbotSocialPreparedStatement* statement);
[[nodiscard]] QueryResult PlayerbotSocialQuery(PlayerbotSocialPreparedStatement* statement);
[[nodiscard]] QueryCallback PlayerbotSocialAsyncQuery(PlayerbotSocialPreparedStatement* statement);
[[nodiscard]] std::string ConsumePlayerbotSocialSql(PlayerbotSocialPreparedStatement* statement);

#endif
