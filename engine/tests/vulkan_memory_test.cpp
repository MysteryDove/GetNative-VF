#include "vulkan_loader.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <class Exception = std::exception, class Function>
void expect_throws(Function &&function, std::string_view message) {
    try {
        function();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(std::string{message});
}

void test_noncoherent_range_alignment() {
    using getnative::vulkan_detail::aligned_noncoherent_range;
    {
        const auto range = aligned_noncoherent_range(64, 64, 64, 256);
        expect(range.offset == 64 && range.size == 64,
               "already aligned non-coherent range remains unchanged");
    }
    {
        const auto range = aligned_noncoherent_range(65, 1, 64, 256);
        expect(range.offset == 64 && range.size == 64,
               "single-byte range expands to one atom");
    }
    {
        const auto range = aligned_noncoherent_range(130, 70, 64, 256);
        expect(range.offset == 128 && range.size == 128,
               "range expands outward at both boundaries");
    }
    {
        const auto range = aligned_noncoherent_range(250, 6, 64, 256);
        expect(range.offset == 192 && range.size == 64,
               "range ending at the allocation boundary is valid");
    }
    {
        const auto range = aligned_noncoherent_range(250, 5, 64, 255);
        expect(range.offset == 192 && range.size == 63,
               "final non-coherent range is clamped to allocation size");
    }
    expect_throws<std::invalid_argument>(
        [&] { (void)aligned_noncoherent_range(0, 0, 64, 256); },
        "zero-sized non-coherent range is rejected");
    expect_throws<std::invalid_argument>(
        [&] { (void)aligned_noncoherent_range(250, 7, 64, 256); },
        "out-of-allocation non-coherent range is rejected");
    expect_throws<std::invalid_argument>(
        [&] { (void)aligned_noncoherent_range(0, 1, 0, 256); },
        "zero atom size is rejected");
}

void test_result_names() {
    expect(std::string_view{getnative::vulkan_detail::vk_result_name(
               VK_ERROR_DEVICE_LOST)} == "VK_ERROR_DEVICE_LOST",
           "device-lost result has a stable diagnostic name");
    expect_throws<std::runtime_error>(
        [] {
            getnative::vulkan_detail::check_vk(
                VK_ERROR_DEVICE_LOST, "synthetic Vulkan operation");
        },
        "Vulkan result checker rejects device loss");
}

} // namespace

int main() {
    try {
        test_noncoherent_range_alignment();
        test_result_names();
        std::cout << "Vulkan memory tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Vulkan memory tests failed: " << error.what() << '\n';
        return 1;
    }
}
