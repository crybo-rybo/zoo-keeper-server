#include <zoo/zoo.hpp>

#include <iostream>
#include <string_view>

namespace {

void print_error(const zoo::Error& error) {
    switch (error.code) {
    case zoo::ErrorCode::TemplateRenderFailed:
        std::cerr << "The selected model does not expose a chat template: " << error.message
                  << '\n';
        break;
    case zoo::ErrorCode::RequestCancelled:
        std::cerr << "The request was cancelled before completion." << '\n';
        break;
    case zoo::ErrorCode::InferenceFailed:
        std::cerr << "Inference failed: " << error.to_string() << '\n';
        break;
    default:
        std::cerr << error.to_string() << '\n';
        break;
    }
}

} // namespace

int main(int argc, char** argv) {
    constexpr std::string_view kDefaultPrompt = "Reply with the single word: zoo.";

    if (argc > 3 || argc < 2) {
        std::cerr << "Usage: zoo_keeper_live_smoke <model.gguf> [prompt]" << '\n';
        return 1;
    }

    zoo::Config config;
    config.model_path = argv[1];
    config.context_size = 4096;
    config.max_tokens = 32;
    config.n_gpu_layers = 0;
    config.system_prompt = "You are a concise assistant.";

    auto agent_result = zoo::Agent::create(config);
    if (!agent_result) {
        print_error(agent_result.error());
        return 1;
    }

    std::string_view prompt = (argc == 3) ? std::string_view(argv[2]) : kDefaultPrompt;
    auto& agent = *agent_result;
    auto handle = agent->chat(zoo::Message::user(std::string(prompt)));
    auto response = handle.future.get();
    if (!response) {
        print_error(response.error());
        return 1;
    }

    if (response->text.empty()) {
        std::cerr << "Live smoke received an empty response." << '\n';
        return 1;
    }

    std::cout << "Live smoke succeeded." << '\n';
    std::cout << response->text << '\n';
    return 0;
}
