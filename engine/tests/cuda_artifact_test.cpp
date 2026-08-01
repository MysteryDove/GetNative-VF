#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Config {
    std::filesystem::path fatbin;
    std::filesystem::path cuobjdump = L"cuobjdump.exe";
    std::vector<std::filesystem::path> pe_files;
    std::vector<std::string> expected_sms;
    std::vector<std::string> expected_ptx_targets;
};

struct ProcessResult {
    unsigned long exit_code = 0;
    std::string output;
};

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

[[nodiscard]] std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument) {
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring{argument};
    }
    std::wstring result(1U, L'\"');
    std::size_t backslashes = 0U;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(character);
            backslashes = 0U;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0U;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'\"');
    return result;
}

[[nodiscard]] std::string windows_error(DWORD code) {
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0U, reinterpret_cast<wchar_t *>(&buffer), 0U, nullptr);
    std::string result = "Windows error " + std::to_string(code);
    if (length != 0U && buffer != nullptr) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0U, buffer,
                                               static_cast<int>(length), nullptr,
                                               0, nullptr, nullptr);
        if (bytes > 0) {
            std::string description(static_cast<std::size_t>(bytes), '\0');
            (void)WideCharToMultiByte(CP_UTF8, 0U, buffer, static_cast<int>(length),
                                      description.data(), bytes, nullptr, nullptr);
            result += ": " + description;
        }
        LocalFree(buffer);
    }
    return result;
}

[[nodiscard]] ProcessResult run_process(const std::filesystem::path &executable,
                                        std::span<const std::wstring> arguments) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (CreatePipe(&read_pipe, &write_pipe, &security, 0U) == 0) {
        throw std::runtime_error("CreatePipe failed: " + windows_error(GetLastError()));
    }
    const auto close_pipe = [](HANDLE handle) noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    };
    if (SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0U) == 0) {
        const DWORD error = GetLastError();
        close_pipe(read_pipe);
        close_pipe(write_pipe);
        throw std::runtime_error("SetHandleInformation failed: " + windows_error(error));
    }

    std::wstring command_line = quote_windows_argument(executable.wstring());
    for (const auto &argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_windows_argument(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                       TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) == 0) {
        const DWORD error = GetLastError();
        close_pipe(read_pipe);
        close_pipe(write_pipe);
        throw std::runtime_error("failed to launch cuobjdump: " + windows_error(error));
    }
    close_pipe(write_pipe);
    write_pipe = nullptr;

    ProcessResult result;
    std::array<char, 16U * 1024U> buffer{};
    for (;;) {
        DWORD count = 0U;
        if (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                     &count, nullptr) == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                close_pipe(read_pipe);
                close_pipe(process.hThread);
                close_pipe(process.hProcess);
                throw std::runtime_error("failed to read cuobjdump output: "
                                         + windows_error(error));
            }
            break;
        }
        result.output.append(buffer.data(), count);
    }
    close_pipe(read_pipe);
    expect(WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0,
           "waiting for cuobjdump failed");
    if (GetExitCodeProcess(process.hProcess, &result.exit_code) == 0) {
        const DWORD error = GetLastError();
        close_pipe(process.hThread);
        close_pipe(process.hProcess);
        throw std::runtime_error("failed to read cuobjdump exit code: "
                                 + windows_error(error));
    }
    close_pipe(process.hThread);
    close_pipe(process.hProcess);
    return result;
}

[[nodiscard]] std::string inspect_fatbin(const Config &config,
                                         std::wstring_view operation) {
    const std::array<std::wstring, 2> arguments{
        std::wstring{operation}, config.fatbin.wstring(),
    };
    ProcessResult result = run_process(config.cuobjdump, arguments);
    if (result.exit_code != 0U) {
        std::string operation_name;
        operation_name.reserve(operation.size());
        for (const wchar_t character : operation) {
            operation_name.push_back(static_cast<char>(character));
        }
        throw std::runtime_error("cuobjdump failed for "
                                 + operation_name
                                 + ": " + result.output);
    }
    expect(!result.output.empty(), "cuobjdump returned empty output");
    return result.output;
}

[[nodiscard]] std::vector<std::byte> read_binary(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("failed to open artifact: " + path.string());
    const std::streamoff size = input.tellg();
    if (size <= 0 || static_cast<std::uintmax_t>(size)
            > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("artifact has an invalid size: " + path.string());
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char *>(bytes.data()), size);
    if (!input) throw std::runtime_error("failed to read artifact: " + path.string());
    return bytes;
}

template <class Value>
[[nodiscard]] Value read_struct(std::span<const std::byte> bytes,
                                std::size_t offset,
                                std::string_view label) {
    if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) {
        throw std::runtime_error(std::string{label} + " lies outside the PE file");
    }
    Value result{};
    std::memcpy(&result, bytes.data() + offset, sizeof(Value));
    return result;
}

[[nodiscard]] std::size_t checked_add(std::size_t left, std::size_t right,
                                      std::string_view label) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::runtime_error(std::string{label} + " overflows");
    }
    return left + right;
}

struct PeImage {
    std::vector<std::byte> bytes;
    std::vector<IMAGE_SECTION_HEADER> sections;
    DWORD size_of_headers = 0U;
    IMAGE_DATA_DIRECTORY imports{};

    [[nodiscard]] std::size_t rva_offset(DWORD rva, std::string_view label) const {
        if (rva < size_of_headers) {
            if (rva >= bytes.size()) throw std::runtime_error(std::string{label} + " RVA is invalid");
            return rva;
        }
        for (const auto &section : sections) {
            const DWORD span = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva >= section.VirtualAddress
                && static_cast<std::uint64_t>(rva) - section.VirtualAddress < span) {
                const std::uint64_t offset = static_cast<std::uint64_t>(section.PointerToRawData)
                    + (static_cast<std::uint64_t>(rva) - section.VirtualAddress);
                if (offset >= bytes.size()) {
                    throw std::runtime_error(std::string{label} + " file offset is invalid");
                }
                return static_cast<std::size_t>(offset);
            }
        }
        throw std::runtime_error(std::string{label} + " RVA has no PE section");
    }

    [[nodiscard]] std::string rva_string(DWORD rva, std::string_view label) const {
        const std::size_t offset = rva_offset(rva, label);
        const auto begin = reinterpret_cast<const char *>(bytes.data() + offset);
        const auto end = reinterpret_cast<const char *>(bytes.data() + bytes.size());
        const auto terminator = std::find(begin, end, '\0');
        if (terminator == end) throw std::runtime_error(std::string{label} + " is unterminated");
        return std::string(begin, terminator);
    }
};

[[nodiscard]] PeImage parse_pe(const std::filesystem::path &path) {
    PeImage image;
    image.bytes = read_binary(path);
    const std::span<const std::byte> bytes{image.bytes};
    const IMAGE_DOS_HEADER dos = read_struct<IMAGE_DOS_HEADER>(bytes, 0U, "DOS header");
    expect(dos.e_magic == IMAGE_DOS_SIGNATURE, "artifact is not an MZ executable");
    expect(dos.e_lfanew >= 0, "PE header offset is negative");
    const std::size_t nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    const DWORD signature = read_struct<DWORD>(bytes, nt_offset, "PE signature");
    expect(signature == IMAGE_NT_SIGNATURE, "artifact has no PE signature");
    const std::size_t file_header_offset = checked_add(nt_offset, sizeof(DWORD), "PE header");
    const IMAGE_FILE_HEADER file_header = read_struct<IMAGE_FILE_HEADER>(
        bytes, file_header_offset, "COFF header");
    const std::size_t optional_offset = checked_add(
        file_header_offset, sizeof(IMAGE_FILE_HEADER), "optional header");
    const WORD magic = read_struct<WORD>(bytes, optional_offset, "optional-header magic");
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const IMAGE_OPTIONAL_HEADER64 optional = read_struct<IMAGE_OPTIONAL_HEADER64>(
            bytes, optional_offset, "PE32+ optional header");
        image.size_of_headers = optional.SizeOfHeaders;
        if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
            image.imports = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        }
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const IMAGE_OPTIONAL_HEADER32 optional = read_struct<IMAGE_OPTIONAL_HEADER32>(
            bytes, optional_offset, "PE32 optional header");
        image.size_of_headers = optional.SizeOfHeaders;
        if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
            image.imports = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        }
    } else {
        throw std::runtime_error("unsupported PE optional-header magic");
    }
    const std::size_t section_offset = checked_add(
        optional_offset, file_header.SizeOfOptionalHeader, "section table");
    image.sections.reserve(file_header.NumberOfSections);
    for (std::size_t index = 0; index < file_header.NumberOfSections; ++index) {
        const std::size_t offset = checked_add(
            section_offset, index * sizeof(IMAGE_SECTION_HEADER), "section header");
        image.sections.push_back(read_struct<IMAGE_SECTION_HEADER>(
            bytes, offset, "section header"));
    }
    return image;
}

[[nodiscard]] std::vector<std::string> pe_imports(const std::filesystem::path &path) {
    const PeImage image = parse_pe(path);
    std::vector<std::string> result;
    if (image.imports.VirtualAddress == 0U || image.imports.Size == 0U) return result;
    std::size_t offset = image.rva_offset(image.imports.VirtualAddress, "import directory");
    const std::size_t maximum_descriptors = image.imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    expect(maximum_descriptors > 0U, "PE import directory is smaller than one descriptor");
    for (std::size_t index = 0; index < maximum_descriptors; ++index) {
        const IMAGE_IMPORT_DESCRIPTOR descriptor = read_struct<IMAGE_IMPORT_DESCRIPTOR>(
            image.bytes, offset, "import descriptor");
        offset = checked_add(offset, sizeof(descriptor), "import descriptor table");
        if (descriptor.OriginalFirstThunk == 0U && descriptor.TimeDateStamp == 0U
            && descriptor.ForwarderChain == 0U && descriptor.Name == 0U
            && descriptor.FirstThunk == 0U) {
            return result;
        }
        expect(descriptor.Name != 0U, "PE import descriptor has no DLL name");
        result.push_back(lowercase(image.rva_string(descriptor.Name, "import DLL name")));
    }
    throw std::runtime_error("PE import directory has no terminating descriptor");
}

void verify_pe_imports(const std::filesystem::path &path) {
    for (const std::string &name : pe_imports(path)) {
        if (name == "nvcuda.dll" || name == "vulkan-1.dll"
            || (name.starts_with("cudart64_") && name.ends_with(".dll"))) {
            throw std::runtime_error(path.string() + " has forbidden runtime import " + name);
        }
    }
}

void verify_fatbin(const Config &config) {
    const auto bytes = read_binary(config.fatbin);
    expect(bytes.size() >= sizeof(std::uint32_t), "CUDA fatbin is too small");
    const std::uint32_t magic = read_struct<std::uint32_t>(bytes, 0U, "fatbin magic");
    expect(magic == 0xBA55ED50U, "CUDA artifact does not have the fatbin wrapper magic");

    const std::string elf = lowercase(inspect_fatbin(config, L"--list-elf"));
    const std::string ptx = lowercase(inspect_fatbin(config, L"--list-ptx"));
    const std::string resources = lowercase(
        inspect_fatbin(config, L"--dump-resource-usage"));
    const std::string sass = inspect_fatbin(config, L"--dump-sass");
    const std::string lower_sass = lowercase(sass);
    const std::string ptx_code = lowercase(inspect_fatbin(config, L"--dump-ptx"));
    expect(elf.find("elf file") != std::string::npos
               && elf.find("sm_") != std::string::npos,
           "CUDA fatbin has no listed native cubin");
    expect(ptx.find("ptx file") != std::string::npos
               && (ptx.find("compute_") != std::string::npos
                   || ptx.find("sm_") != std::string::npos),
           "CUDA fatbin has no listed forward-compatible PTX");
    for (const std::string &target : config.expected_sms) {
        expect(elf.find(lowercase(target)) != std::string::npos,
               "CUDA fatbin is missing an expected native SM target");
    }
    for (const std::string &target : config.expected_ptx_targets) {
        std::string expected = lowercase(target);
        std::string cuobjdump_spelling = expected;
        if (cuobjdump_spelling.starts_with("compute_")) {
            cuobjdump_spelling.replace(0U, std::string_view{"compute_"}.size(), "sm_");
        }
        expect(ptx.find(expected) != std::string::npos
                   || ptx.find(cuobjdump_spelling) != std::string::npos,
               "CUDA fatbin is missing an expected PTX target");
    }
    expect(resources.find("resource usage") != std::string::npos,
           "cuobjdump did not report CUDA resource usage");
    // Production path is expected to use FP32 FMA under normal ptxas optimization.
    expect(lower_sass.find("ffma") != std::string_view::npos,
           "production CUDA SASS contains no FP32 FMA");
    expect(ptx_code.find("fma.rn.f32") != std::string::npos
               || ptx_code.find("fma.rn.ftz.f32") != std::string::npos,
           "production CUDA PTX contains no Float32 FMA");

    constexpr std::array<std::string_view, 25> kernel_names{
        "inverse_axis_b3", "inverse_axis_b7", "inverse_axis_b11",
        "inverse_axis_b15", "inverse_axis_generic",
        "inverse_axis_matrix_b3", "inverse_axis_matrix_b7",
        "inverse_axis_matrix_b11", "inverse_axis_matrix_b15",
        "inverse_axis_matrix_generic", "forward_axis_matrix_b3",
        "forward_axis_matrix_b7", "forward_axis_matrix_b11",
        "forward_axis_matrix_b15", "forward_axis_matrix_generic",
        "metric_axis_p1_b3", "metric_axis_p1_b7", "metric_axis_p1_b11",
        "metric_axis_p1_b15", "metric_axis_p1_generic",
        "metric_axis_p1_horizontal_first_b3",
        "metric_axis_p1_horizontal_first_b7",
        "metric_axis_p1_horizontal_first_b11",
        "metric_axis_p1_horizontal_first_b15",
        "metric_axis_p1_horizontal_first_generic",
    };
    for (std::string_view name : kernel_names) {
        expect(lower_sass.find(name) != std::string::npos,
               "CUDA SASS is missing an expected kernel symbol");
        expect(resources.find(name) != std::string::npos,
               "CUDA resource report is missing an expected kernel symbol");
    }
    std::size_t stack_fields = 0U;
    std::size_t local_fields = 0U;
    for (std::size_t position = resources.find("stack:");
         position != std::string::npos;
         position = resources.find("stack:", position + 1U)) {
        ++stack_fields;
        expect(resources.compare(position, 7U, "stack:0") == 0,
               "CUDA resource report contains a nonzero stack frame");
    }
    for (std::size_t position = resources.find("local:");
         position != std::string::npos;
         position = resources.find("local:", position + 1U)) {
        ++local_fields;
        expect(resources.compare(position, 7U, "local:0") == 0,
               "CUDA resource report contains local memory or compiler spill");
    }
    expect(stack_fields == kernel_names.size() && local_fields == kernel_names.size(),
           "CUDA resource report lacks per-kernel stack/local accounting");
}

[[nodiscard]] Config parse_arguments(int argc, char **argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next = [&](std::string_view name) -> std::string_view {
            if (++index >= argc) throw std::invalid_argument(std::string{name} + " needs a value");
            return argv[index];
        };
        if (argument == "--fatbin") {
            config.fatbin = std::filesystem::path{next(argument)};
        } else if (argument == "--cuobjdump") {
            config.cuobjdump = std::filesystem::path{next(argument)};
        } else if (argument == "--pe") {
            config.pe_files.emplace_back(next(argument));
        } else if (argument == "--expect-sm") {
            config.expected_sms.emplace_back(next(argument));
        } else if (argument == "--expect-ptx") {
            config.expected_ptx_targets.emplace_back(next(argument));
        } else {
            throw std::invalid_argument("unknown CUDA artifact-test argument: "
                                        + std::string{argument});
        }
    }
    if (config.fatbin.empty()) throw std::invalid_argument("--fatbin is required");
    return config;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Config config = parse_arguments(argc, argv);
        verify_fatbin(config);
        for (const auto &pe : config.pe_files) verify_pe_imports(pe);
        std::cout << "CUDA artifact tests passed: fatbin=" << config.fatbin.string()
                  << " pe_files=" << config.pe_files.size() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "CUDA artifact test failure: " << error.what() << '\n';
        return 1;
    }
}
