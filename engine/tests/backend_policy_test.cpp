#include "backend_policy.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

} // namespace

int main() {
    using getnative::cli::AutomaticBackend;
    using getnative::cli::choose_automatic_backend;
    try {
        expect(choose_automatic_backend(1U, true, true, true)
                   == AutomaticBackend::cuda,
               "CUDA must have first priority");
        expect(choose_automatic_backend(1U, false, true, true)
                   == AutomaticBackend::vulkan,
               "discrete Vulkan must follow failed CUDA");
        expect(choose_automatic_backend(1U, false, true, false)
                   == AutomaticBackend::cpu,
               "integrated or software Vulkan must not enter Auto");
        expect(choose_automatic_backend(2U, false, true, true)
                   == AutomaticBackend::vulkan,
               "Vulkan must handle p=2..4");
        expect(choose_automatic_backend(4U, true, true, true)
                   == AutomaticBackend::cuda,
               "CUDA must handle p=2..4");
        expect(choose_automatic_backend(5U, true, true, true)
                   == AutomaticBackend::cpu,
               "p>4 must use CPU");
        std::cout << "Backend policy tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Backend policy test failed: " << error.what() << '\n';
        return 1;
    }
}
