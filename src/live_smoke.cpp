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

    zoo::ModelConfig model_config;
    model_config.model_path = argv[1];
    model_config.context_size = 4096;
    model_config.n_gpu_layers = 0;

    zoo::AgentConfig agent_config;

    zoo::GenerationOptions gen_options;
    gen_options.max_tokens = 32;

    auto agent_result = zoo::Agent::create(model_config, agent_config, gen_options);
    if (!agent_result) {
        print_error(agent_result.error());
        return 1;
    }

    (*agent_result)->set_system_prompt("You are a concise assistant.");

    std::string_view prompt = (argc == 3) ? std::string_view(argv[2]) : kDefaultPrompt;
    auto& agent = *agent_result;
    std::vector<zoo::Message> messages = {zoo::Message::user(std::string(prompt))};
    auto view = zoo::ConversationView(std::span<const zoo::Message>(messages));
    auto handle = agent->complete(view, gen_options);
    auto response = handle.await_result();
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
