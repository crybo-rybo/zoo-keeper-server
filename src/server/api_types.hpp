#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <zoo/core/types.hpp>

namespace zks::server {

struct ApiError {
    int http_status = 400;
    std::string message;
    std::string type = "invalid_request_error";
    std::optional<std::string> param;
    std::optional<std::string> code;
};

template <typename T> using ApiResult = std::expected<T, ApiError>;

inline ApiError invalid_request_error(std::string message,
                                      std::optional<std::string> param = std::nullopt,
                                      std::optional<std::string> code = std::nullopt) {
    return ApiError{400, std::move(message), "invalid_request_error", std::move(param),
                    std::move(code)};
}

inline ApiError service_unavailable_error(std::string message,
                                          std::optional<std::string> code = std::nullopt) {
    return ApiError{503, std::move(message), "service_unavailable_error", std::nullopt,
                    std::move(code)};
}

inline ApiError server_error(std::string message, std::optional<std::string> code = std::nullopt) {
    return ApiError{500, std::move(message), "server_error", std::nullopt, std::move(code)};
}

struct ChatCompletionRequest {
    std::string model;
    std::vector<zoo::Message> messages;
    bool stream = false;
};

} // namespace zks::server
