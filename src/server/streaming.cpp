#include "server/streaming.hpp"

#include <string>

namespace zks::server {
namespace {

// Escape a string_view for embedding in a JSON string value. This avoids
// constructing an nlohmann::json object just to get proper escaping for the
// content field, which is the hot path during token streaming.
void append_json_escaped(std::string& out, std::string_view sv) {
    for (const char c : sv) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                // Control characters: output as \u00XX
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(static_cast<unsigned char>(c) >> 4) & 0xf];
                out += hex[static_cast<unsigned char>(c) & 0xf];
            } else {
                out += c;
            }
            break;
        }
    }
}

std::string make_chunk_impl(std::string_view completion_id, std::int64_t created,
                            std::string_view model_id, std::optional<std::string_view> content,
                            bool include_role, std::optional<std::string_view> finish_reason) {
    // Build the SSE frame directly as a string to avoid nlohmann::json overhead.
    // This is the hottest allocation path during streaming -- called once per token.
    std::string result;
    result.reserve(256); // typical chunk is ~150-200 bytes

    result += R"(data: {"id":")";
    append_json_escaped(result, completion_id);
    result += R"(","object":"chat.completion.chunk","created":)";
    result += std::to_string(created);
    result += R"(,"model":")";
    append_json_escaped(result, model_id);
    result += R"(","choices":[{"index":0,"delta":{)";

    bool has_delta_field = false;
    if (include_role) {
        result += R"("role":"assistant")";
        has_delta_field = true;
    }
    if (content.has_value()) {
        if (has_delta_field) {
            result += ',';
        }
        result += R"("content":")";
        append_json_escaped(result, *content);
        result += '"';
    }

    result += R"(},"finish_reason":)";
    if (finish_reason.has_value()) {
        result += '"';
        append_json_escaped(result, *finish_reason);
        result += '"';
    } else {
        result += "null";
    }
    result += "}]}";
    result += "\n\n";

    return result;
}

} // namespace

std::string make_sse_data(const nlohmann::json& payload) {
    return "data: " + payload.dump() + "\n\n";
}

std::string make_sse_done() {
    return "data: [DONE]\n\n";
}

std::string make_first_streaming_chunk(std::string_view completion_id, std::int64_t created,
                                       std::string_view model_id,
                                       std::optional<std::string_view> content,
                                       std::optional<std::string_view> finish_reason) {
    return make_chunk_impl(completion_id, created, model_id, content, true, finish_reason);
}

std::string make_streaming_chunk(std::string_view completion_id, std::int64_t created,
                                 std::string_view model_id,
                                 std::optional<std::string_view> content,
                                 std::optional<std::string_view> finish_reason) {
    return make_chunk_impl(completion_id, created, model_id, content, false, finish_reason);
}

} // namespace zks::server
