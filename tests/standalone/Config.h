#ifndef PLAYERBOTS_SOCIAL_STANDALONE_CONFIG_H
#define PLAYERBOTS_SOCIAL_STANDALONE_CONFIG_H

#include <any>
#include <string>
#include <unordered_map>

class ConfigMgr
{
public:
    template <typename T>
    void SetOption(std::string const& name, T const& value)
    {
        _options[name] = value;
    }

    template <typename T>
    [[nodiscard]] T GetOption(std::string const& name, T const& fallback) const
    {
        auto const option = _options.find(name);
        if (option == _options.end())
            return fallback;

        T const* value = std::any_cast<T>(&option->second);
        return value ? *value : fallback;
    }

private:
    std::unordered_map<std::string, std::any> _options;
};

inline ConfigMgr sStandaloneConfigMgr;
inline ConfigMgr* sConfigMgr = &sStandaloneConfigMgr;

#endif
