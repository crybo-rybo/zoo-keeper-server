#include "doctest.h"

#include "server/streaming.hpp"

#include <nlohmann/json.hpp>

TEST_CASE("make_sse_data wraps JSON in SSE frame") {
    nlohmann::json payload = {{"key", "value"}};
    auto result = zks::server::make_sse_data(payload);
    CHECK(result == "data: {\"key\":\"value\"}\n\n");
}

TEST_CASE("make_sse_done returns [DONE] frame") {
    CHECK(zks::server::make_sse_done() == "data: [DONE]\n\n");
}

TEST_CASE("make_chat_completion_chunk") {
    const auto id = "chatcmpl-test";
    const int64_t created = 1700000000;
    const auto model = "test-model";

    SUBCASE("content token without role") {
        auto chunk = zks::server::make_chat_completion_chunk(
            id, created, model, "hello", false, std::nullopt);
        // Strip "data: " prefix and trailing "\n\n"
        auto json_str = chunk.substr(6, chunk.size() - 8);
        auto json = nlohmann::json::parse(json_str);

        auto& delta = json["choices"][0]["delta"];
        CHECK(delta.contains("content"));
        CHECK(delta["content"] == "hello");
        CHECK_FALSE(delta.contains("role"));
        CHECK(json["choices"][0]["finish_reason"].is_null());
    }

    SUBCASE("first chunk includes assistant role") {
        auto chunk = zks::server::make_chat_completion_chunk(
            id, created, model, "hello", true, std::nullopt);
        auto json_str = chunk.substr(6, chunk.size() - 8);
        auto json = nlohmann::json::parse(json_str);

        auto& delta = json["choices"][0]["delta"];
        CHECK(delta["role"] == "assistant");
        CHECK(delta["content"] == "hello");
    }

    SUBCASE("role-only chunk (no content)") {
        auto chunk = zks::server::make_chat_completion_chunk(
            id, created, model, std::nullopt, true, std::nullopt);
        auto json_str = chunk.substr(6, chunk.size() - 8);
        auto json = nlohmann::json::parse(json_str);

        auto& delta = json["choices"][0]["delta"];
        CHECK(delta["role"] == "assistant");
        CHECK_FALSE(delta.contains("content"));
    }

    SUBCASE("finish reason chunk") {
        auto chunk = zks::server::make_chat_completion_chunk(
            id, created, model, std::nullopt, false, "stop");
        auto json_str = chunk.substr(6, chunk.size() - 8);
        auto json = nlohmann::json::parse(json_str);

        CHECK(json["choices"][0]["finish_reason"] == "stop");
    }

    SUBCASE("JSON special characters are escaped") {
        std::string content = "he said \"hello\"\nnew\\line\ttab";
        auto chunk = zks::server::make_chat_completion_chunk(
            id, created, model, content, false, std::nullopt);
        auto json_str = chunk.substr(6, chunk.size() - 8);
        auto json = nlohmann::json::parse(json_str);

        CHECK(json["choices"][0]["delta"]["content"] == content);
    }

    SUBCASE("control characters below 0x20 are \\u-escaped") {
        std::string content;
        content += '\x01';
        content += '\x1f';
        auto chunk = zks::server::make_chat_completion_chunk(
            id, created, model, content, false, std::nullopt);

        CHECK(chunk.find("\\u0001") != std::string::npos);
        CHECK(chunk.find("\\u001f") != std::string::npos);

        // Verify it still parses correctly
        auto json_str = chunk.substr(6, chunk.size() - 8);
        auto json = nlohmann::json::parse(json_str);
        CHECK(json["choices"][0]["delta"]["content"] == content);
    }

    SUBCASE("chunk structure matches OpenAI spec") {
        auto chunk = zks::server::make_chat_completion_chunk(
            id, created, model, "test", true, std::nullopt);
        auto json_str = chunk.substr(6, chunk.size() - 8);
        auto json = nlohmann::json::parse(json_str);

        CHECK(json.contains("id"));
        CHECK(json.contains("object"));
        CHECK(json.contains("created"));
        CHECK(json.contains("model"));
        CHECK(json.contains("choices"));
        CHECK(json["object"] == "chat.completion.chunk");
        CHECK(json["choices"][0].contains("index"));
        CHECK(json["choices"][0].contains("delta"));
        CHECK(json["choices"][0].contains("finish_reason"));
    }
}
