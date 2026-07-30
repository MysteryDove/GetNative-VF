#include "benchmark_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to read test file");
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_file(const std::filesystem::path &path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to create test file");
    output << contents;
    if (!output) throw std::runtime_error("failed to write test file");
}

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("getnative-benchmark-support-" + std::to_string(nonce));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("failed to create temporary test directory");
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main() {
    try {
        TemporaryDirectory directory;
        const auto final_path = directory.path() / "result.json";
        const auto temporary_path = directory.path() / "result.json.tmp";

        getnative::benchmark::atomic_write_json(final_path, "{\"value\":1}\n");
        require(read_file(final_path) == "{\"value\":1}\n",
                "atomic write changed the JSON contents");
        require(!std::filesystem::exists(temporary_path),
                "successful atomic write left a temporary file");

        bool preexisting_failed = false;
        try {
            getnative::benchmark::atomic_write_json(final_path, "{\"value\":2}\n");
        } catch (const std::exception &) {
            preexisting_failed = true;
        }
        require(preexisting_failed, "preexisting final path did not fail");
        require(read_file(final_path) == "{\"value\":1}\n",
                "preexisting final path was overwritten");

        std::filesystem::remove(final_path);
        write_file(temporary_path, "{\"writer\":\"benchmark\"}\n");
        write_file(final_path, "{\"writer\":\"competitor\"}\n");
        bool publication_race_failed = false;
        try {
            getnative::benchmark::publish_json_no_replace(temporary_path, final_path);
        } catch (const std::exception &) {
            publication_race_failed = true;
        }
        require(publication_race_failed,
                "publication race replaced a final path created after validation");
        require(read_file(final_path) == "{\"writer\":\"competitor\"}\n",
                "publication race overwrote the competing final path");

        std::filesystem::remove(temporary_path);
        std::cout << "benchmark support tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "benchmark support test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
