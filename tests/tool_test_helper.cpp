#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

std::string read_stdin() {
    std::string input;
    std::string line;
    while (std::getline(std::cin, line)) {
        input += line;
    }
    return input;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "expected mode argument" << '\n';
        return 2;
    }

    const std::string mode = argv[1];

    if (mode == "echo") {
        const auto input = read_stdin();
        auto parsed = nlohmann::json::parse(input, nullptr, false);
        if (parsed.is_discarded()) {
            std::cerr << "stdin was not valid JSON" << '\n';
            return 3;
        }
        std::cout << nlohmann::json{{"received", parsed}}.dump();
        return 0;
    }

    if (mode == "invalid-json") {
        std::cout << "not-json";
        return 0;
    }

    if (mode == "non-object") {
        std::cout << R"(["array-output"])";
        return 0;
    }

    if (mode == "stderr-fail") {
        std::cerr << "tool helper failed on purpose";
        return 17;
    }

    if (mode == "timeout") {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::cout << R"({"late":true})";
        return 0;
    }

    if (mode == "large-stdout") {
        std::string payload(70 * 1024, 'x');
        std::cout << nlohmann::json{{"payload", payload}}.dump();
        return 0;
    }

    std::cerr << "unknown mode: " << mode << '\n';
    return 4;
}
