#include "getnative/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        const std::string directory_name =
            "\xe5\xaa\x92\xe4\xbd\x93-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e-\xed\x95\x9c\xea\xb8\x80";
        const std::string file_name =
            "\xe8\xa7\x86\xe9\xa2\x91-\xe5\xa4\xa2-\xec\x98\x81\xec\x83\x81.bin";
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path root = std::filesystem::temp_directory_path()
            / ("getnative-utf8-path-" + std::to_string(nonce));
        const std::filesystem::path directory = root / getnative::path_from_utf8(directory_name);
        const std::filesystem::path file = directory / getnative::path_from_utf8(file_name);

        std::filesystem::create_directories(directory);
        {
            std::ofstream output(file, std::ios::binary);
            output << "utf8-path";
            require(static_cast<bool>(output), "failed to write Unicode path fixture");
        }
        require(std::filesystem::is_regular_file(file), "Unicode path fixture is missing");
        require(getnative::path_to_utf8(file.filename()) == file_name,
                "Unicode filename did not round-trip through filesystem::path");
        {
            std::ifstream input(file, std::ios::binary);
            std::string value;
            input >> value;
            require(value == "utf8-path", "failed to read Unicode path fixture");
        }

#if defined(_WIN32)
        require(::_wputenv_s(L"GETNATIVE_UTF8_PATH_TEST",
                             L"\u5a92\u4f53-\u65e5\u672c\u8a9e-\ud55c\uae00") == 0,
                "failed to set Unicode environment path fixture");
#else
        require(::setenv("GETNATIVE_UTF8_PATH_TEST", directory_name.c_str(), 1) == 0,
                "failed to set Unicode environment path fixture");
#endif
        const auto environment_path =
            getnative::path_from_environment("GETNATIVE_UTF8_PATH_TEST");
        require(environment_path.has_value(), "failed to read environment path");
        require(getnative::path_to_utf8(*environment_path) == directory_name,
                "environment path did not preserve Unicode");

        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        require(!cleanup_error, "failed to remove Unicode path fixture");
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS utf8 path\n";
    return 0;
}
