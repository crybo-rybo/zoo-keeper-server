#include "server/streaming.hpp"

namespace zks::server {

std::string make_sse_data(const nlohmann::json& payload) {
    return "data: " + payload.dump() + "\n\n";
}

std::string make_sse_done() {
    return "data: [DONE]\n\n";
}

std::string make_chat_completion_chunk(std::string_view completion_id, std::int64_t created,
                                       std::string_view model_id,
                                       std::optional<std::string_view> content, bool include_role,
                                       std::optional<std::string_view> finish_reason) {
    nlohmann::json delta = nlohmann::json::object();
    if (include_role) {
        delta["role"] = "assistant";
    }
    if (content.has_value()) {
        delta["content"] = *content;
    }

    nlohmann::json choice = {{"index", 0}, {"delta", std::move(delta)}, {"finish_reason", nullptr}};
    if (finish_reason.has_value()) {
        choice["finish_reason"] = *finish_reason;
    }

    return make_sse_data(nlohmann::json{{"id", completion_id},
                                        {"object", "chat.completion.chunk"},
                                        {"created", created},
                                        {"model", model_id},
                                        {"choices", nlohmann::json::array({std::move(choice)})}});
}

} // namespace zks::server
