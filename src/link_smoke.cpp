#include <zoo/zoo.hpp>

#include <iostream>

int main() {
    auto agent_result = zoo::Agent::create(zoo::Config{});
    if (!agent_result) {
        const zoo::Error& error = agent_result.error();
        if (error.code == zoo::ErrorCode::InvalidModelPath) {
            std::cout << "Observed expected InvalidModelPath validation failure." << '\n';
            return 0;
        }

        std::cerr << "Unexpected zoo-keeper error: " << error.to_string() << '\n';
        return 1;
    }

    std::cerr << "Unexpected success creating zoo::Agent with an empty config." << '\n';
    return 1;
}
