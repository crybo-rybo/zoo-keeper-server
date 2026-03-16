#pragma once

#include "server/result.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zks::server {

/// Clock function type for injectable time sources. Defaults to `now_seconds`.
using Clock = std::function<std::int64_t()>;

/// Transparent hash and equality for std::unordered_map<std::string, ...> that
/// allows lookup by std::string_view without allocating a temporary std::string.
struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

struct TransparentStringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

inline std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string combine_system_prompts(std::string_view base_prompt,
                                          std::string_view request_prompt) {
    if (base_prompt.empty()) {
        return std::string(request_prompt);
    }
    if (request_prompt.empty()) {
        return std::string(base_prompt);
    }
    return std::string(base_prompt) + "\n\n" + std::string(request_prompt);
}

inline Result<void> reject_unknown_keys(const nlohmann::json& json, std::string_view context,
                                        std::span<const std::string_view> allowed_keys) {
    if (!json.is_object()) {
        return std::unexpected(std::string(context) + " must be a JSON object");
    }

    for (auto it = json.begin(); it != json.end(); ++it) {
        if (std::find(allowed_keys.begin(), allowed_keys.end(), it.key()) == allowed_keys.end()) {
            return std::unexpected("Unknown " + std::string(context) + " key: " + it.key());
        }
    }

    return {};
}

} // namespace zks::server
