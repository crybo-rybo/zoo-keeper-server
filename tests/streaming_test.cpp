#include "server/streaming.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

TEST(StreamingTest, MakeSseDataWrapsJsonInSseFrame) {
    nlohmann::json payload = {{"key", "value"}};
    auto result = zks::server::make_sse_data(payload);
    EXPECT_EQ(result, "data: {\"key\":\"value\"}\n\n");
}

TEST(StreamingTest, MakeSseDoneReturnsDoneFrame) {
    EXPECT_EQ(zks::server::make_sse_done(), "data: [DONE]\n\n");
}

TEST(StreamingTest, ChunkContentTokenWithoutRole) {
    auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-test", 1700000000, "test-model", "hello", false, std::nullopt);
    auto json_str = chunk.substr(6, chunk.size() - 8);
    auto json = nlohmann::json::parse(json_str);

    auto& delta = json["choices"][0]["delta"];
    EXPECT_TRUE(delta.contains("content"));
    EXPECT_EQ(delta["content"], "hello");
    EXPECT_FALSE(delta.contains("role"));
    EXPECT_TRUE(json["choices"][0]["finish_reason"].is_null());
}

TEST(StreamingTest, ChunkFirstTokenIncludesRole) {
    auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-test", 1700000000, "test-model", "hello", true, std::nullopt);
    auto json_str = chunk.substr(6, chunk.size() - 8);
    auto json = nlohmann::json::parse(json_str);

    auto& delta = json["choices"][0]["delta"];
    EXPECT_EQ(delta["role"], "assistant");
    EXPECT_EQ(delta["content"], "hello");
}

TEST(StreamingTest, ChunkRoleOnlyNoContent) {
    auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-test", 1700000000, "test-model", std::nullopt, true, std::nullopt);
    auto json_str = chunk.substr(6, chunk.size() - 8);
    auto json = nlohmann::json::parse(json_str);

    auto& delta = json["choices"][0]["delta"];
    EXPECT_EQ(delta["role"], "assistant");
    EXPECT_FALSE(delta.contains("content"));
}

TEST(StreamingTest, ChunkFinishReason) {
    auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-test", 1700000000, "test-model", std::nullopt, false, "stop");
    auto json_str = chunk.substr(6, chunk.size() - 8);
    auto json = nlohmann::json::parse(json_str);

    EXPECT_EQ(json["choices"][0]["finish_reason"], "stop");
}

TEST(StreamingTest, ChunkJsonSpecialCharactersEscaped) {
    std::string content = "he said \"hello\"\nnew\\line\ttab";
    auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-test", 1700000000, "test-model", content, false, std::nullopt);
    auto json_str = chunk.substr(6, chunk.size() - 8);
    auto json = nlohmann::json::parse(json_str);

    EXPECT_EQ(json["choices"][0]["delta"]["content"], content);
}

TEST(StreamingTest, ChunkControlCharactersEscaped) {
    std::string content;
    content += '\x01';
    content += '\x1f';
    auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-test", 1700000000, "test-model", content, false, std::nullopt);

    EXPECT_NE(chunk.find("\\u0001"), std::string::npos);
    EXPECT_NE(chunk.find("\\u001f"), std::string::npos);

    auto json_str = chunk.substr(6, chunk.size() - 8);
    auto json = nlohmann::json::parse(json_str);
    EXPECT_EQ(json["choices"][0]["delta"]["content"], content);
}

TEST(StreamingTest, ChunkStructureMatchesOpenAiSpec) {
    auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-test", 1700000000, "test-model", "test", true, std::nullopt);
    auto json_str = chunk.substr(6, chunk.size() - 8);
    auto json = nlohmann::json::parse(json_str);

    EXPECT_TRUE(json.contains("id"));
    EXPECT_TRUE(json.contains("object"));
    EXPECT_TRUE(json.contains("created"));
    EXPECT_TRUE(json.contains("model"));
    EXPECT_TRUE(json.contains("choices"));
    EXPECT_EQ(json["object"], "chat.completion.chunk");
    EXPECT_TRUE(json["choices"][0].contains("index"));
    EXPECT_TRUE(json["choices"][0].contains("delta"));
    EXPECT_TRUE(json["choices"][0].contains("finish_reason"));
}
