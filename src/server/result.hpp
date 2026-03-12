#pragma once

#include <expected>
#include <string>

namespace zks::server {

template <typename T> using Result = std::expected<T, std::string>;

} // namespace zks::server
