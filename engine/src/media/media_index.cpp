#include "getnative/media_decode.hpp"
#include "getnative/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#if defined(GETNATIVE_USE_XXHASH) && __has_include(<xxhash.h>)
#include <xxhash.h>
#define GETNATIVE_HAS_XXHASH 1
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace getnative::media {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'G', 'N', 'V', 'F', 'L', 'W', 'I', 0};
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::int64_t kMissingTimestamp = std::numeric_limits<std::int64_t>::min();

class Bytes {
public:
    template <class Value>
    void integer(Value value) {
        using Unsigned = std::make_unsigned_t<Value>;
        Unsigned bits = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(Value); ++index) {
            data.push_back(static_cast<std::uint8_t>(bits >> (index * 8U)));
        }
    }

    void floating(double value) { integer(std::bit_cast<std::uint64_t>(value)); }

    void string(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("media index string is too large");
        }
        integer(static_cast<std::uint32_t>(value.size()));
        data.insert(data.end(), value.begin(), value.end());
    }

    std::vector<std::uint8_t> data;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    template <class Value>
    Value integer() {
        require(sizeof(Value));
        using Unsigned = std::make_unsigned_t<Value>;
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < sizeof(Value); ++index) {
            value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8U);
        }
        offset_ += sizeof(Value);
        return static_cast<Value>(static_cast<Unsigned>(value));
    }

    double floating() { return std::bit_cast<double>(integer<std::uint64_t>()); }

    std::string string() {
        const std::uint32_t size = integer<std::uint32_t>();
        require(size);
        std::string value(reinterpret_cast<const char *>(bytes_.data() + offset_), size);
        offset_ += size;
        return value;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

private:
    void require(std::size_t size) const {
        if (size > bytes_.size() - offset_) {
            throw std::runtime_error("media index is truncated");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

std::uint64_t checksum(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = kFnvOffset;
    for (const std::uint8_t byte : bytes) {
        value ^= byte;
        value *= kFnvPrime;
    }
    return value;
}

std::int64_t file_mtime_ns(const std::filesystem::path &path) {
    const auto value = std::filesystem::last_write_time(path).time_since_epoch();
#if defined(_WIN32)
    // MSVC's file clock counts 100 ns ticks since 1601-01-01 (FILETIME).
    // Rebase onto the Unix epoch so stored mtimes match indexes written on
    // other platforms and no longer overflow int64 into negative values.
    constexpr std::int64_t filetime_unix_offset = 116444736000000000LL;
    const auto ticks = std::chrono::duration_cast<
        std::chrono::duration<std::int64_t, std::ratio<1, 10000000LL>>>(value).count();
    return (ticks - filetime_unix_offset) * 100LL;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
#endif
}

std::uint8_t picture_code(const std::optional<std::string> &value) {
    if (!value) return 0U;
    if (*value == "I") return 1U;
    if (*value == "P") return 2U;
    if (*value == "B") return 3U;
    return 4U;
}

std::uint8_t field_code(std::string_view value) {
    if (value == "tt") return 1U;
    if (value == "bb") return 2U;
    if (value == "tb") return 3U;
    if (value == "bt") return 4U;
    if (value == "progressive") return 5U;
    return 0U;
}

std::string field_name(std::uint8_t value) {
    switch (value) {
    case 1U: return "tt";
    case 2U: return "bb";
    case 3U: return "tb";
    case 4U: return "bt";
    case 5U: return "progressive";
    default: return "unknown";
    }
}

std::optional<std::string> picture_name(std::uint8_t value) {
    switch (value) {
    case 0U: return std::nullopt;
    case 1U: return "I";
    case 2U: return "P";
    case 3U: return "B";
    case 4U: return "other";
    default: throw std::runtime_error("media index has an invalid picture type");
    }
}

std::vector<std::uint8_t> frame_payload(const MediaIndex &index) {
    Bytes output;
    for (const FrameIdentity &frame : index.frames) {
        output.integer(frame.frame_index);
        output.integer(frame.pts.value_or(kMissingTimestamp));
        output.integer(frame.best_effort_timestamp.value_or(kMissingTimestamp));
        output.floating(frame.timestamp_seconds.value_or(
            std::numeric_limits<double>::quiet_NaN()));
        output.integer(picture_code(frame.picture_type));
        output.integer(static_cast<std::uint8_t>(frame.key_frame ? 1U : 0U));
        output.integer(static_cast<std::uint16_t>(0U));
        output.integer(frame.keyframe_anchor);
        output.integer(frame.keyframe_timestamp.value_or(kMissingTimestamp));
        output.integer(frame.decode_index);
        output.integer(frame.dts.value_or(kMissingTimestamp));
        output.integer(frame.file_position.value_or(-1));
        output.integer(frame.packet_size);
        std::uint8_t flags = 0U;
        if (frame.rap) flags |= 1U;
        if (frame.leading_frame) flags |= 2U;
        if (frame.vp8_invisible_frame) flags |= 4U;
        if (frame.vp9_superframe) flags |= 8U;
        output.integer(flags);
        output.integer(field_code(frame.field_order));
        output.integer(frame.poc.value_or(std::numeric_limits<std::int32_t>::min()));
        output.integer(frame.repeat_pict);
        output.integer(frame.extradata_index);
        output.integer(static_cast<std::uint16_t>(0U));
        output.integer(frame.packet_duration.value_or(kMissingTimestamp));
        output.integer(frame.color_range);
        output.integer(frame.color_space);
        output.integer(frame.color_primaries);
        output.integer(frame.color_transfer);
        output.integer(frame.chroma_location);
        output.integer(static_cast<std::uint32_t>(0U));
    }
    return std::move(output.data);
}

std::vector<std::uint8_t> serialize(const MediaIndex &index) {
    const std::vector<std::uint8_t> payload = frame_payload(index);
    Bytes output;
    output.data.insert(output.data.end(), kMagic.begin(), kMagic.end());
    output.integer(MediaIndex::format_version);
    output.integer(static_cast<std::uint32_t>(0U));
    output.integer(index.source_size);
    output.integer(index.source_mtime_ns);
    output.integer(index.stream_index);
    output.integer(index.width);
    output.integer(index.height);
    output.integer(index.bit_depth);
    output.integer(index.duration_ticks);
    output.integer(index.time_base_num);
    output.integer(index.time_base_den);
    output.integer(static_cast<std::uint64_t>(index.frames.size()));
    output.integer(checksum(payload));
    output.string(index.fingerprint);
    output.string(index.codec);
    output.string(index.profile);
    output.string(index.pixel_format);
    output.string(index.range);
    output.integer(index.color_range);
    output.integer(index.color_space);
    output.integer(index.color_primaries);
    output.integer(index.color_transfer);
    output.integer(index.chroma_location);
    output.string(index.decoder);
    output.string(index.format_name);
    output.string(index.index_mode);
    output.string(index.seek_method);
    output.integer(index.format_flags);
    output.integer(static_cast<std::uint8_t>(index.raw_demuxer ? 1U : 0U));
    output.integer(index.packet_count);
    output.integer(index.selective_decodes);
    output.integer(static_cast<std::uint64_t>(index.stream_index_entries.size()));
    for (const StreamIndexEntry &entry : index.stream_index_entries) {
        output.integer(entry.file_position);
        output.integer(entry.timestamp);
        output.integer(entry.flags);
        output.integer(entry.size);
        output.integer(entry.distance);
    }
    output.integer(static_cast<std::uint64_t>(index.extradata.size()));
    for (const ExtraDataInfo &extra : index.extradata) {
        output.integer(extra.index);
        output.integer(extra.codec_id);
        output.integer(extra.fourcc);
        output.integer(extra.width);
        output.integer(extra.height);
        output.string(extra.pixel_format);
        output.integer(extra.bit_rate);
        output.integer(static_cast<std::uint64_t>(extra.data.size()));
        output.data.insert(output.data.end(), extra.data.begin(), extra.data.end());
    }
    output.data.insert(output.data.end(), payload.begin(), payload.end());
    return std::move(output.data);
}

void sync_file(const std::filesystem::path &path) {
#if defined(_WIN32)
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE || !FlushFileBuffers(file)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        throw std::runtime_error("failed to flush media index temporary file");
    }
    CloseHandle(file);
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ::fsync(fd) != 0) {
        if (fd >= 0) ::close(fd);
        throw std::runtime_error("failed to fsync media index temporary file");
    }
    ::close(fd);
#endif
}

std::string cache_index_path(const std::string &cache_directory,
                             const MediaIndex &index) {
    std::string key = index.fingerprint;
    std::replace_if(key.begin(), key.end(),
                    [](unsigned char value) { return !std::isalnum(value); }, '_');
    if (key.size() > 96U) key.resize(96U);
    return path_to_utf8(path_from_utf8(cache_directory)
                        / (key + ".stream-" + std::to_string(index.stream_index) + ".vf.lwi"));
}

// LSMASH hashes the first mebibyte and, for files larger than 2 MiB, the
// last one. The accepted FileHash is XXH3-64 (current LSMASH) or XXH32
// (legacy LSMASH); both are computed here so either validates.
std::pair<std::string, std::string> lwi_hashes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open media for LWI hash");
    const std::uint64_t size = std::filesystem::file_size(path);
    constexpr std::size_t edge = 1024U * 1024U;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(size, edge)));
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (size > 2U * edge) {
        input.clear();
        input.seekg(-static_cast<std::streamoff>(edge), std::ios::end);
        const std::size_t old = bytes.size();
        bytes.resize(old + edge);
        input.read(reinterpret_cast<char *>(bytes.data() + old), static_cast<std::streamsize>(edge));
    }
#if defined(GETNATIVE_HAS_XXHASH)
    const auto hex64 = [](std::uint64_t value) {
        char buffer[17]{};
        std::snprintf(buffer, sizeof(buffer), "%016llx",
                      static_cast<unsigned long long>(value));
        return "0x" + std::string{buffer};
    };
    const auto hex32 = [](std::uint32_t value) {
        char buffer[9]{};
        std::snprintf(buffer, sizeof(buffer), "%08x", value);
        return "0x" + std::string{buffer};
    };
    return {hex64(XXH3_64bits(bytes.data(), bytes.size())),
            hex32(XXH32(bytes.data(), bytes.size(), 0))};
#else
    // Keep a deterministic fallback for builds without xxHash. Such builds
    // still read generated .vf.lwi files, but cannot validate external hashes.
    return {};
#endif
}

std::string lwi_hash(const std::filesystem::path &path) {
    return lwi_hashes(path).first;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::unordered_map<std::string, std::string> comma_fields(std::string_view value) {
    std::unordered_map<std::string, std::string> fields;
    std::size_t begin = 0U;
    while (begin <= value.size()) {
        const std::size_t end = value.find(',', begin);
        std::string part{value.substr(begin, end == std::string_view::npos
                                               ? value.size() - begin : end - begin)};
        const std::size_t equals = part.find('=');
        if (equals != std::string::npos) {
            fields[trim(part.substr(0U, equals))] = trim(part.substr(equals + 1U));
        } else if (!trim(part).empty()) {
            fields["_"] = trim(part);
        }
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return fields;
}

std::int64_t parse_integer(std::string_view value, std::int64_t fallback = 0, int base = 0) {
    try {
        std::string text{trim(std::string{value})};
        std::size_t consumed = 0U;
        const std::int64_t result = std::stoll(text, &consumed, base);
        return consumed == text.size() ? result : fallback;
    } catch (...) {
        return fallback;
    }
}

std::optional<std::string> tag_value(std::string_view source, std::string_view name) {
    const std::string open = "<" + std::string{name} + ">";
    const std::string close = "</" + std::string{name} + ">";
    const std::size_t begin = source.find(open);
    if (begin == std::string_view::npos) return std::nullopt;
    const std::size_t value_begin = begin + open.size();
    const std::size_t end = source.find(close, value_begin);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string{source.substr(value_begin, end - value_begin)};
}

// Header scalars appear as <Name=value> in genuine LSMASH indexes and as
// <Name>value</Name> in indexes written by older revisions of this project;
// accept both forms.
std::optional<std::string> header_field(std::string_view source, std::string_view name) {
    if (std::optional<std::string> tagged = tag_value(source, name)) return tagged;
    const std::string open = "<" + std::string{name} + "=";
    const std::size_t begin = source.find(open);
    if (begin == std::string_view::npos) return std::nullopt;
    const std::size_t value_begin = begin + open.size();
    const std::size_t end = source.find('>', value_begin);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string{source.substr(value_begin, end - value_begin)};
}

MediaIndex read_lwi_text(const std::string &index_path, const std::string &media_path,
                         std::optional<std::string_view> expected_fingerprint,
                         std::optional<std::uint32_t> expected_stream) {
    std::ifstream input(path_from_utf8(index_path), std::ios::binary);
    if (!input) throw std::runtime_error("LWI index does not exist");
    const std::string text{std::istreambuf_iterator<char>{input}, {}};
    if (text.find("LSMASHWorksIndexVersion") == std::string::npos
        || text.find("LibavReaderIndexFile") == std::string::npos
        || text.find("</LibavReaderIndexFile>") == std::string::npos
        || text.find("</LibavReaderIndex>") == std::string::npos) {
        throw std::runtime_error("LWI header is invalid");
    }
    const std::size_t file_tag = text.find("<LibavReaderIndexFile=");
    if (file_tag == std::string::npos) {
        throw std::runtime_error("LWI index version is missing");
    }
    {
        const std::size_t value_begin = file_tag + std::string_view{"<LibavReaderIndexFile="}.size();
        const std::size_t value_end = text.find('>', value_begin);
        if (value_end == std::string::npos) throw std::runtime_error("truncated LWI header");
        const auto version = parse_integer(text.substr(value_begin, value_end - value_begin), 0);
        if (version < 1 || version > 19) throw std::runtime_error("unsupported LWI index version");
    }
    MediaIndex result;
    result.source_path = media_path;
    const auto scalar = [&](std::string_view key) -> std::optional<std::string> {
        return header_field(text, key);
    };
    if (!scalar("FileSize") || !scalar("FileLastModificationTime")
        || !scalar("ActiveVideoStreamIndex")
        || text.find("<StreamInfo=") == std::string::npos) {
        throw std::runtime_error("LWI is missing required stream metadata");
    }
    result.source_size = scalar("FileSize").has_value()
        ? static_cast<std::uint64_t>(parse_integer(*scalar("FileSize"))) : 0U;
    result.source_mtime_ns = scalar("FileLastModificationTime").has_value()
        ? parse_integer(*scalar("FileLastModificationTime")) * 1000000000LL : 0LL;
    // LSMASH zero-pads this field in decimal (%+011d), which a base-0 parse
    // would misread as octal for stream indices above 7.
    result.stream_index = scalar("ActiveVideoStreamIndex").has_value()
        ? static_cast<std::uint32_t>(std::max<std::int64_t>(
            0, parse_integer(*scalar("ActiveVideoStreamIndex"), 0, 10))) : 0U;
    const std::string active_duration = "<StreamDuration="
        + std::to_string(result.stream_index) + ",0>";
    const std::size_t duration_tag = text.find(active_duration);
    if (duration_tag != std::string::npos) {
        const std::size_t value_begin = text.find('>', duration_tag);
        const std::size_t value_end = text.find("</StreamDuration>", value_begin);
        if (value_begin != std::string::npos && value_end != std::string::npos) {
            result.duration_ticks = parse_integer(text.substr(value_begin + 1U,
                                                              value_end - value_begin - 1U),
                                                  AV_NOPTS_VALUE);
        }
    }
    const std::size_t reader_tag = text.find("<LibavReaderIndex=");
    if (reader_tag != std::string::npos) {
        const std::size_t value_begin = reader_tag
            + std::string_view{"<LibavReaderIndex="}.size();
        const std::size_t value_end = text.find('>', value_begin);
        if (value_end != std::string::npos) {
            const std::string value = text.substr(value_begin, value_end - value_begin);
            const std::size_t first_comma = value.find(',');
            const std::size_t second_comma = first_comma == std::string::npos
                ? std::string::npos : value.find(',', first_comma + 1U);
            if (first_comma != std::string::npos) {
                result.format_flags = parse_integer(value.substr(0U, first_comma), 0, 0);
            }
            if (second_comma != std::string::npos) {
                result.raw_demuxer = parse_integer(value.substr(
                    first_comma + 1U, second_comma - first_comma - 1U)) != 0;
                result.format_name = trim(value.substr(second_comma + 1U));
            }
        }
    }
    if (expected_stream && result.stream_index != *expected_stream) {
        throw std::runtime_error("LWI active video stream does not match request");
    }
    {
        const std::string active_info = "<StreamInfo="
            + std::to_string(result.stream_index) + ",0>";
        const std::size_t info_begin = text.find(active_info);
        const std::size_t info_end = info_begin == std::string::npos
            ? std::string::npos : text.find("</StreamInfo>", info_begin);
        const std::string info_body = info_end == std::string::npos ? std::string{}
            : text.substr(info_begin, info_end - info_begin);
        const std::size_t newline = info_body.find('\n');
        const auto fields = newline == std::string::npos
            ? std::unordered_map<std::string, std::string>{}
            : comma_fields(info_body.substr(newline + 1U));
        if (fields.count("Codec")) {
            const auto codec_id = parse_integer(fields.at("Codec"), -1);
            result.codec = codec_id >= 0 ? avcodec_get_name(static_cast<AVCodecID>(codec_id)) : fields.at("Codec");
        }
        if (fields.count("TimeBase")) {
            const std::string &base = fields.at("TimeBase");
            const std::size_t slash = base.find('/');
            if (slash != std::string::npos) {
                result.time_base_num = static_cast<std::int32_t>(parse_integer(base.substr(0U, slash)));
                result.time_base_den = static_cast<std::int32_t>(parse_integer(base.substr(slash + 1U), 1));
            }
        }
        if (fields.count("Width")) result.width = static_cast<std::int32_t>(parse_integer(fields.at("Width")));
        if (fields.count("Height")) result.height = static_cast<std::int32_t>(parse_integer(fields.at("Height")));
        if (fields.count("Format")) {
            result.pixel_format = fields.at("Format");
            if (result.pixel_format.find("p016") != std::string::npos) result.bit_depth = 16;
            else if (result.pixel_format.find("p010") != std::string::npos
                     || result.pixel_format.find("10") != std::string::npos) result.bit_depth = 10;
        }
        if (fields.count("ColorSpace")) {
            result.color_space = static_cast<std::int32_t>(
                parse_integer(fields.at("ColorSpace"), 2));
        }
        result.index_mode = "lwi_external";
    }
    std::istringstream lines{text};
    std::string line;
    FrameIdentity pending;
    bool have_pending = false;
    bool in_stream_entries = false;
    bool in_extra = false;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line == "</StreamIndexEntries>") { in_stream_entries = false; continue; }
        if (line == "</ExtraDataList>") { in_extra = false; continue; }
        if (line.rfind("<StreamIndexEntries=", 0U) == 0U) {
            const std::size_t value_begin = std::string_view{"<StreamIndexEntries="}.size();
            const std::size_t comma = line.find(',', value_begin);
            in_stream_entries = comma != std::string::npos
                && parse_integer(line.substr(value_begin, comma - value_begin), -1, 10)
                    == static_cast<std::int64_t>(result.stream_index);
            continue;
        }
        if (line.rfind("<ExtraDataList=", 0U) == 0U) {
            const std::size_t value_begin = std::string_view{"<ExtraDataList="}.size();
            const std::size_t comma = line.find(',', value_begin);
            in_extra = comma != std::string::npos
                && parse_integer(line.substr(value_begin, comma - value_begin), -1, 10)
                    == static_cast<std::int64_t>(result.stream_index);
            continue;
        }
        if (line.rfind("Index=", 0U) == 0U) {
            if (have_pending) result.frames.push_back(pending);
            pending = FrameIdentity{};
            pending.color_range = result.color_range;
            pending.color_space = result.color_space;
            pending.color_primaries = result.color_primaries;
            pending.color_transfer = result.color_transfer;
            pending.chroma_location = result.chroma_location;
            pending.decode_index = result.frames.size();
            const auto fields = comma_fields(line.substr(6U));
            if (fields.count("_") && parse_integer(fields.at("_"), result.stream_index)
                    != static_cast<std::int64_t>(result.stream_index)) {
                have_pending = false;
                continue;
            }
            if (fields.count("POS") && parse_integer(fields.at("POS"), -1) >= 0) pending.file_position = parse_integer(fields.at("POS"));
            if (fields.count("PTS") && parse_integer(fields.at("PTS"), AV_NOPTS_VALUE) != AV_NOPTS_VALUE) pending.pts = parse_integer(fields.at("PTS"));
            if (fields.count("DTS") && parse_integer(fields.at("DTS"), AV_NOPTS_VALUE) != AV_NOPTS_VALUE) pending.dts = parse_integer(fields.at("DTS"));
            if (fields.count("EDI")) pending.extradata_index = static_cast<std::uint32_t>(std::max<std::int64_t>(0, parse_integer(fields.at("EDI"))));
            have_pending = true;
            continue;
        }
        if (line.rfind("Key=", 0U) == 0U && have_pending) {
            const auto fields = comma_fields(line.substr(4U));
            const std::size_t comma = line.find(',');
            pending.key_frame = parse_integer(comma == std::string::npos
                                                  ? line.substr(4U) : line.substr(4U, comma - 4U)) != 0;
            if (fields.count("Pic")) {
                const auto pic = parse_integer(fields.at("Pic"));
                if (pic == 1) pending.picture_type = "I";
                else if (pic == 2) pending.picture_type = "P";
                else if (pic == 3) pending.picture_type = "B";
            }
            if (fields.count("POC")) {
                pending.poc = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    parse_integer(fields.at("POC")),
                    std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
            if (fields.count("Repeat")) pending.repeat_pict = static_cast<std::int32_t>(parse_integer(fields.at("Repeat")));
            if (fields.count("Field")) {
                const std::int64_t field = parse_integer(fields.at("Field"));
                pending.field_order = field == 1 ? "tt" : field == 2 ? "bb" : "unknown";
            }
            if (fields.count("Super")) pending.vp9_superframe = parse_integer(fields.at("Super")) != 0;
            pending.rap = pending.key_frame;
            continue;
        }
        if (in_stream_entries && line.rfind("POS=", 0U) == 0U) {
            const auto fields = comma_fields(line.substr(4U));
            StreamIndexEntry entry;
            const std::size_t comma = line.find(',');
            entry.file_position = parse_integer(comma == std::string::npos
                                                    ? line.substr(4U) : line.substr(4U, comma - 4U), -1);
            entry.timestamp = parse_integer(fields.count("TS") ? fields.at("TS") : "0");
            // LSMASH writes Flags in hex without a 0x prefix.
            entry.flags = static_cast<std::uint32_t>(parse_integer(
                fields.count("Flags") ? fields.at("Flags") : "0", 0, 16));
            entry.size = static_cast<std::uint32_t>(parse_integer(fields.count("Size") ? fields.at("Size") : "0"));
            entry.distance = static_cast<std::uint32_t>(parse_integer(fields.count("Distance") ? fields.at("Distance") : "0"));
            result.stream_index_entries.push_back(entry);
        }
        if (in_extra && line.rfind("Size=", 0U) == 0U) {
            const auto fields = comma_fields(line);
            ExtraDataInfo extra;
            extra.index = static_cast<std::uint32_t>(result.extradata.size());
            extra.codec_id = static_cast<std::uint32_t>(std::max<std::int64_t>(
                0, parse_integer(fields.count("Codec") ? fields.at("Codec") : "0")));
            extra.fourcc = static_cast<std::uint32_t>(std::max<std::int64_t>(
                0, parse_integer(fields.count("4CC") ? fields.at("4CC") : "0")));
            extra.width = static_cast<std::int32_t>(parse_integer(fields.count("Width") ? fields.at("Width") : "0"));
            extra.height = static_cast<std::int32_t>(parse_integer(fields.count("Height") ? fields.at("Height") : "0"));
            extra.pixel_format = fields.count("Format") ? fields.at("Format") : "";
            extra.bit_rate = static_cast<std::uint32_t>(std::max<std::int64_t>(
                0, parse_integer(fields.count("BPS") ? fields.at("BPS") : "0")));
            // LSMASH appends the raw extradata blob plus one newline right
            // after this line; consume those bytes so they are never mistaken
            // for frame records.
            const std::int64_t blob_size = parse_integer(fields.count("Size") ? fields.at("Size") : "0");
            if (blob_size > 0) {
                extra.data.resize(static_cast<std::size_t>(blob_size));
                lines.read(reinterpret_cast<char *>(extra.data.data()),
                           static_cast<std::streamsize>(extra.data.size()));
                if (static_cast<std::int64_t>(lines.gcount()) != blob_size) {
                    throw std::runtime_error("LWI extradata blob is truncated");
                }
            }
            if (lines.peek() == '\r') lines.ignore();
            if (lines.peek() == '\n') lines.ignore();
            result.extradata.push_back(std::move(extra));
            continue;
        }
    }
    if (have_pending) result.frames.push_back(pending);
    if (result.frames.empty()) throw std::runtime_error("LWI contains no video index records");
    // LWI records are listed in decoding order (decode_index was assigned in
    // file order while parsing). LSMASH derives its public frame numbering by
    // sorting on timestamps, so mirror that here: frame numbers then agree
    // with the VapourSynth plugin instead of following packet order.
    const std::size_t record_count = result.frames.size();
    bool unique_pts = true;
    bool complete_pts = true;
    {
        std::unordered_set<std::int64_t> seen_pts;
        seen_pts.reserve(record_count);
        for (const FrameIdentity &frame : result.frames) {
            if (!frame.pts) {
                complete_pts = false;
                unique_pts = false;
            } else if (!seen_pts.insert(*frame.pts).second) {
                unique_pts = false;
            }
        }
    }
    std::vector<std::size_t> order(record_count);
    std::iota(order.begin(), order.end(), 0U);
    if (complete_pts) {
        std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
            const std::int64_t left_pts = *result.frames[left].pts;
            const std::int64_t right_pts = *result.frames[right].pts;
            return left_pts != right_pts ? left_pts < right_pts : left < right;
        });
    } else {
        const bool poc_codec = result.codec == "h264" || result.codec == "hevc";
        const bool usable_poc = poc_codec
            && std::all_of(result.frames.begin(), result.frames.end(),
                           [](const FrameIdentity &frame) {
                               return frame.poc.has_value();
                           })
            && std::any_of(result.frames.begin(), result.frames.end(),
                           [](const FrameIdentity &frame) {
                               return frame.poc.value_or(0) != 0;
                           });
        if (usable_poc) {
            order.clear();
            for (std::size_t begin = 0U; begin < record_count;) {
                std::size_t end = begin + 1U;
                while (end < record_count && !result.frames[end].rap) ++end;
                std::vector<std::size_t> run(end - begin);
                std::iota(run.begin(), run.end(), begin);
                std::stable_sort(run.begin(), run.end(), [&](std::size_t left,
                                                             std::size_t right) {
                    const std::int32_t left_poc = *result.frames[left].poc;
                    const std::int32_t right_poc = *result.frames[right].poc;
                    return left_poc != right_poc ? left_poc < right_poc
                                                 : left < right;
                });
                order.insert(order.end(), run.begin(), run.end());
                begin = end;
            }
        } else {
            std::vector<std::size_t> pts_slots;
            std::vector<std::size_t> timestamped;
            for (std::size_t decode = 0U; decode < record_count; ++decode) {
                if (!result.frames[decode].pts) continue;
                pts_slots.push_back(decode);
                timestamped.push_back(decode);
            }
            std::stable_sort(timestamped.begin(), timestamped.end(),
                             [&](std::size_t left, std::size_t right) {
                const std::int64_t left_pts = *result.frames[left].pts;
                const std::int64_t right_pts = *result.frames[right].pts;
                return left_pts != right_pts ? left_pts < right_pts : left < right;
            });
            for (std::size_t slot = 0U; slot < pts_slots.size(); ++slot) {
                order[pts_slots[slot]] = timestamped[slot];
            }
        }
    }
    std::vector<FrameIdentity> presentation;
    presentation.reserve(record_count);
    result.decode_to_presentation.resize(record_count);
    result.presentation_to_decode.resize(record_count);
    for (std::size_t presentation_index = 0U; presentation_index < record_count; ++presentation_index) {
        FrameIdentity frame = std::move(result.frames[order[presentation_index]]);
        result.decode_to_presentation[frame.decode_index] = presentation_index;
        result.presentation_to_decode[presentation_index] = frame.decode_index;
        frame.frame_index = presentation_index;
        if (frame.pts && result.time_base_den != 0) {
            frame.timestamp_seconds = static_cast<double>(*frame.pts)
                * result.time_base_num / result.time_base_den;
        }
        presentation.push_back(std::move(frame));
    }
    result.frames = std::move(presentation);
    // Keyframe anchors walk in decoding order: each frame's anchor is the
    // most recent random-access picture at or before its decode position.
    std::size_t current_anchor = 0U;
    for (std::size_t decode = 0U; decode < record_count; ++decode) {
        FrameIdentity &frame = result.frames[result.decode_to_presentation[decode]];
        if (frame.rap) current_anchor = frame.frame_index;
        frame.keyframe_anchor = current_anchor;
        frame.leading_frame = frame.frame_index < current_anchor;
        frame.keyframe_timestamp = result.frames[current_anchor].pts;
    }
    const std::filesystem::path source_path = path_from_utf8(media_path);
    result.source_size = result.source_size != 0U
        ? result.source_size : std::filesystem::file_size(source_path);
    const std::uint64_t actual_size = std::filesystem::file_size(source_path);
    if (result.source_size != actual_size) throw std::runtime_error("LWI source size mismatch");
    const std::int64_t actual_mtime = file_mtime_ns(source_path);
    if (result.source_mtime_ns != 0LL && result.source_mtime_ns / 1000000000LL != actual_mtime / 1000000000LL) {
        throw std::runtime_error("LWI source mtime mismatch");
    }
    if (const auto hash = header_field(text, "FileHash"); hash && !hash->empty()) {
        std::string expected_hash = *hash;
        std::transform(expected_hash.begin(), expected_hash.end(), expected_hash.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        // Same policy as the LSMASH loader: accept either the XXH3-64 or the
        // legacy XXH32 hash of the source. When xxHash is unavailable the
        // quick-fingerprint check below still validates the media.
        const auto hashes = lwi_hashes(source_path);
        if (!hashes.first.empty() && expected_hash != hashes.first
            && expected_hash != hashes.second) {
            throw std::runtime_error("LWI source hash mismatch");
        }
    }
    result.fingerprint = quick_fingerprint(media_path);
    if (expected_fingerprint && result.fingerprint != *expected_fingerprint) {
        throw std::runtime_error("media fingerprint does not match the LWI");
    }
    result.index_mode = "lwi_external";
    const auto monotonic_decode_value = [&](bool dts) {
        std::optional<std::int64_t> previous;
        for (std::size_t decode = 0U; decode < record_count; ++decode) {
            const FrameIdentity &frame = result.frames[result.decode_to_presentation[decode]];
            if (dts && frame.vp8_invisible_frame) continue;
            const auto value = dts ? frame.dts : frame.file_position;
            if (!value || (previous && *value <= *previous)) return false;
            previous = value;
        }
        return previous.has_value();
    };
    if (unique_pts) {
        result.seek_method = "pts";
    } else if (monotonic_decode_value(true)) {
        result.seek_method = "dts";
    } else if ((result.raw_demuxer || result.format_name == "mpeg"
                || result.format_name == "mpegts")
               && monotonic_decode_value(false)) {
        result.seek_method = "file_position";
    } else {
        result.seek_method = "sample_order";
    }
    result.packet_count = result.frames.size();
    result.decoder = "software";
    return result;
}

std::string serialize_lwi(const MediaIndex &index, const std::string &media_path) {
    const std::filesystem::path source_path = path_from_utf8(media_path);
    std::ostringstream output;
    const AVCodec *codec = avcodec_find_decoder_by_name(index.codec.c_str());
    const int codec_id = codec != nullptr ? static_cast<int>(codec->id) : 0;
    output << "<LSMASHWorksIndexVersion=0.0.3.0>\n<LibavReaderIndexFile=19>\n";
    output << "<InputFilePath>" << media_path << "</InputFilePath>\n";
    // Header scalars use LSMASH's <Name=value> form so the VapourSynth
    // plugin can load this index as well as read_lwi_text.
    output << "<FileSize=" << index.source_size << ">\n";
    output << "<FileLastModificationTime=" << index.source_mtime_ns / 1000000000LL << ">\n";
    output << "<FileHash=" << lwi_hash(source_path) << ">\n";
    const std::size_t format_separator = index.format_name.find(',');
    const std::string format_name = format_separator == std::string::npos
        ? index.format_name : index.format_name.substr(0U, format_separator);
    output << "<LibavReaderIndex=0x" << std::hex << index.format_flags << std::dec
           << "," << (index.raw_demuxer ? 1 : 0) << "," << format_name << ">\n";
    output << "<ActiveVideoStreamIndex>+" << std::setw(10) << std::setfill('0')
           << index.stream_index << std::setfill(' ') << "</ActiveVideoStreamIndex>\n";
    output << "<ActiveAudioStreamIndex>-0000000002</ActiveAudioStreamIndex>\n<DefaultAudioStreamIndex>-0000000001</DefaultAudioStreamIndex>\n";
    output << "<StreamInfo=" << index.stream_index << ",0>\nCodec=" << codec_id
           << ",TimeBase=" << index.time_base_num << "/" << index.time_base_den
           << ",Width=" << index.width << ",Height=" << index.height
           << ",Format=" << index.pixel_format << ",ColorSpace=" << index.color_space
           << "\n</StreamInfo>\n";
    // LSMASH lists packets in decoding order; readers (including the
    // VapourSynth plugin and read_lwi_text above) derive presentation frame
    // numbers by sorting on the timestamps stored here.
    std::vector<const FrameIdentity *> decode_order;
    decode_order.reserve(index.frames.size());
    for (const FrameIdentity &frame : index.frames) decode_order.push_back(&frame);
    std::sort(decode_order.begin(), decode_order.end(),
              [](const FrameIdentity *left, const FrameIdentity *right) {
                  return left->decode_index < right->decode_index;
              });
    for (const FrameIdentity *record : decode_order) {
        const FrameIdentity &frame = *record;
        output << "Index=" << index.stream_index << ",POS=" << frame.file_position.value_or(-1)
               << ",PTS=" << frame.pts.value_or(AV_NOPTS_VALUE) << ",DTS=" << frame.dts.value_or(AV_NOPTS_VALUE)
               << ",EDI=" << frame.extradata_index << "\nKey=" << (frame.rap ? 1 : 0)
               << ",Pic=" << static_cast<int>(picture_code(frame.picture_type)) << ",POC=" << frame.poc.value_or(0)
               << ",Repeat=" << frame.repeat_pict
               << ",Field=" << (frame.field_order == "tt" || frame.field_order == "tb" ? 1
                                   : frame.field_order == "bb" || frame.field_order == "bt" ? 2 : 0)
               << ",Super=" << (frame.vp9_superframe ? 1 : 0) << "\n";
    }
    output << "</LibavReaderIndex>\n<VideoConsistentFieldRepeatPict>0</VideoConsistentFieldRepeatPict>\n"
           << "<StreamDuration=" << index.stream_index << ",0>" << index.duration_ticks
           << "</StreamDuration>\n<StreamIndexEntries=" << index.stream_index << ",0,"
           << index.stream_index_entries.size() << ">\n";
    for (const StreamIndexEntry &entry : index.stream_index_entries) {
        output << "POS=" << entry.file_position << ",TS=" << entry.timestamp
               << ",Flags=" << std::hex << entry.flags << std::dec
               << ",Size=" << entry.size << ",Distance=" << entry.distance << "\n";
    }
    output << "</StreamIndexEntries>\n<ExtraDataList=" << index.stream_index << ",0," << index.extradata.size() << ">\n";
    for (const ExtraDataInfo &extra : index.extradata) {
        output << "Size=" << extra.data.size() << ",Codec=" << extra.codec_id
               << ",4CC=0x" << std::hex << extra.fourcc << std::dec
               << ",Width=" << extra.width << ",Height=" << extra.height
               << ",Format=" << extra.pixel_format << ",BPS=" << extra.bit_rate << "\n";
        if (!extra.data.empty()) {
            output.write(reinterpret_cast<const char *>(extra.data.data()),
                         static_cast<std::streamsize>(extra.data.size()));
        }
        output << "\n";
    }
    output << "</ExtraDataList>\n</LibavReaderIndexFile>\n";
    return output.str();
}

std::string external_lwi_path(const std::string &path, std::uint32_t stream_index,
                              std::uint32_t default_stream_index) {
    if (stream_index == default_stream_index) return path + ".lwi";
    return path + ".stream-" + std::to_string(stream_index) + ".lwi";
}

std::string generated_lwi_path(const std::string &path, std::uint32_t stream_index,
                               std::uint32_t default_stream_index) {
    if (stream_index == default_stream_index) return path + ".vf.lwi";
    return path + ".stream-" + std::to_string(stream_index) + ".vf.lwi";
}

std::string legacy_lwi_path(const std::string &path, std::uint32_t stream_index,
                            std::uint32_t default_stream_index) {
    return path + (stream_index == default_stream_index
        ? ".gnvf.lwi" : ".stream-" + std::to_string(stream_index) + ".lwi");
}

std::shared_ptr<std::mutex> build_lock(const std::string &key) {
    static std::mutex map_mutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> locks;
    const std::scoped_lock lock(map_mutex);
    std::shared_ptr<std::mutex> result = locks[key].lock();
    if (!result) {
        result = std::make_shared<std::mutex>();
        locks[key] = result;
    }
    return result;
}

} // namespace

std::string preferred_index_path(const std::string &path,
                                 std::uint32_t stream_index,
                                 std::uint32_t default_stream_index) {
    return generated_lwi_path(path, stream_index, default_stream_index);
}

MediaIndex load_index(const std::string &index_path, const std::string &media_path,
                      std::optional<std::string_view> expected_fingerprint,
                      std::optional<std::uint32_t> expected_stream) {
    std::ifstream input(path_from_utf8(index_path), std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("media index does not exist");
    const std::streamoff raw_size = input.tellg();
    if (raw_size < 0 || raw_size > static_cast<std::streamoff>(2ULL * 1024ULL * 1024ULL * 1024ULL)) {
        throw std::runtime_error("media index size is invalid");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(raw_size));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), raw_size);
    if (!input) throw std::runtime_error("failed to read media index");
    if (bytes.size() >= 1U && bytes.front() == static_cast<std::uint8_t>('<')) {
        return read_lwi_text(index_path, media_path, expected_fingerprint, expected_stream);
    }
    Reader reader(bytes);
    for (const std::uint8_t byte : kMagic) {
        if (reader.integer<std::uint8_t>() != byte) {
            throw std::runtime_error("media index magic is invalid");
        }
    }
    const std::uint32_t version = reader.integer<std::uint32_t>();
    if (version < 1U || version > MediaIndex::format_version) {
        throw std::runtime_error("media index version is incompatible");
    }
    (void)reader.integer<std::uint32_t>();
    MediaIndex index;
    index.source_path = media_path;
    index.source_size = reader.integer<std::uint64_t>();
    index.source_mtime_ns = reader.integer<std::int64_t>();
    index.stream_index = reader.integer<std::uint32_t>();
    index.width = reader.integer<std::int32_t>();
    index.height = reader.integer<std::int32_t>();
    index.bit_depth = reader.integer<std::int32_t>();
    index.duration_ticks = reader.integer<std::int64_t>();
    index.time_base_num = reader.integer<std::int32_t>();
    index.time_base_den = reader.integer<std::int32_t>();
    const std::uint64_t frame_count = reader.integer<std::uint64_t>();
    const std::uint64_t expected_checksum = reader.integer<std::uint64_t>();
    index.fingerprint = reader.string();
    index.codec = reader.string();
    index.profile = reader.string();
    index.pixel_format = reader.string();
    index.range = reader.string();
    if (version >= 3U) {
        index.color_range = reader.integer<std::int32_t>();
        index.color_space = reader.integer<std::int32_t>();
        index.color_primaries = reader.integer<std::int32_t>();
        index.color_transfer = reader.integer<std::int32_t>();
        index.chroma_location = reader.integer<std::int32_t>();
    }
    index.decoder = reader.string();
    if (version >= 2U) {
        index.format_name = reader.string();
        index.index_mode = reader.string();
        index.seek_method = reader.string();
        index.format_flags = reader.integer<std::int64_t>();
        index.raw_demuxer = reader.integer<std::uint8_t>() != 0U;
        index.packet_count = reader.integer<std::uint64_t>();
        index.selective_decodes = reader.integer<std::uint64_t>();
        const std::uint64_t entry_count = reader.integer<std::uint64_t>();
        if (entry_count > 100000000U) throw std::runtime_error("media index entry table is invalid");
        index.stream_index_entries.reserve(static_cast<std::size_t>(entry_count));
        for (std::uint64_t ordinal = 0; ordinal < entry_count; ++ordinal) {
            StreamIndexEntry entry;
            entry.file_position = reader.integer<std::int64_t>();
            entry.timestamp = reader.integer<std::int64_t>();
            entry.flags = reader.integer<std::uint32_t>();
            entry.size = reader.integer<std::uint32_t>();
            entry.distance = reader.integer<std::uint32_t>();
            index.stream_index_entries.push_back(entry);
        }
        const std::uint64_t extra_count = reader.integer<std::uint64_t>();
        if (extra_count > 100000U) throw std::runtime_error("media index extradata table is invalid");
        index.extradata.reserve(static_cast<std::size_t>(extra_count));
        for (std::uint64_t ordinal = 0; ordinal < extra_count; ++ordinal) {
            ExtraDataInfo extra;
            extra.index = reader.integer<std::uint32_t>();
            extra.codec_id = reader.integer<std::uint32_t>();
            extra.fourcc = reader.integer<std::uint32_t>();
            extra.width = reader.integer<std::int32_t>();
            extra.height = reader.integer<std::int32_t>();
            extra.pixel_format = reader.string();
            extra.bit_rate = reader.integer<std::uint32_t>();
            const std::uint64_t data_size = reader.integer<std::uint64_t>();
            if (data_size > reader.remaining()) throw std::runtime_error("media index extradata is truncated");
            extra.data.resize(static_cast<std::size_t>(data_size));
            for (std::uint8_t &value : extra.data) value = reader.integer<std::uint8_t>();
            index.extradata.push_back(std::move(extra));
        }
    }
    if (version == 1U) index.index_mode = "legacy_native";
    constexpr std::size_t old_frame_bytes = 52U;
    constexpr std::size_t version_2_frame_bytes = 96U;
    constexpr std::size_t version_3_frame_bytes = 128U;
    const std::size_t frame_bytes = version >= 3U ? version_3_frame_bytes
        : version >= 2U ? version_2_frame_bytes : old_frame_bytes;
    if (frame_count == 0U || frame_count > 1000000000ULL
        || reader.remaining() != frame_count * frame_bytes) {
        throw std::runtime_error("media index frame table size is invalid");
    }
    const std::span<const std::uint8_t> payload{
        bytes.data() + (bytes.size() - reader.remaining()), reader.remaining()};
    if (checksum(payload) != expected_checksum) {
        throw std::runtime_error("media index checksum mismatch");
    }
    index.frames.reserve(static_cast<std::size_t>(frame_count));
    for (std::uint64_t ordinal = 0; ordinal < frame_count; ++ordinal) {
        FrameIdentity frame;
        frame.frame_index = reader.integer<std::uint64_t>();
        const auto pts = reader.integer<std::int64_t>();
        const auto best = reader.integer<std::int64_t>();
        const double seconds = reader.floating();
        frame.picture_type = picture_name(reader.integer<std::uint8_t>());
        frame.key_frame = reader.integer<std::uint8_t>() != 0U;
        (void)reader.integer<std::uint16_t>();
        frame.keyframe_anchor = reader.integer<std::uint64_t>();
        const auto anchor_timestamp = reader.integer<std::int64_t>();
        if (pts != kMissingTimestamp) frame.pts = pts;
        if (best != kMissingTimestamp) frame.best_effort_timestamp = best;
        if (std::isfinite(seconds)) frame.timestamp_seconds = seconds;
        if (anchor_timestamp != kMissingTimestamp) {
            frame.keyframe_timestamp = anchor_timestamp;
        }
        if (version >= 2U) {
            frame.decode_index = reader.integer<std::uint64_t>();
            const auto dts = reader.integer<std::int64_t>();
            const auto position = reader.integer<std::int64_t>();
            frame.packet_size = reader.integer<std::uint32_t>();
            const auto flags = reader.integer<std::uint8_t>();
            frame.rap = (flags & 1U) != 0U;
            frame.leading_frame = (flags & 2U) != 0U;
            frame.vp8_invisible_frame = (flags & 4U) != 0U;
            frame.vp9_superframe = (flags & 8U) != 0U;
            frame.field_order = field_name(reader.integer<std::uint8_t>());
            const auto poc = reader.integer<std::int32_t>();
            frame.repeat_pict = reader.integer<std::int32_t>();
            frame.extradata_index = reader.integer<std::uint32_t>();
            (void)reader.integer<std::uint16_t>();
            if (dts != kMissingTimestamp) frame.dts = dts;
            if (position >= 0) frame.file_position = position;
            if (!frame.rap) frame.rap = frame.key_frame;
            if (poc != std::numeric_limits<std::int32_t>::min()) frame.poc = poc;
            if (version >= 3U) {
                const auto packet_duration = reader.integer<std::int64_t>();
                if (packet_duration != kMissingTimestamp) {
                    frame.packet_duration = packet_duration;
                }
                frame.color_range = reader.integer<std::int32_t>();
                frame.color_space = reader.integer<std::int32_t>();
                frame.color_primaries = reader.integer<std::int32_t>();
                frame.color_transfer = reader.integer<std::int32_t>();
                frame.chroma_location = reader.integer<std::int32_t>();
                (void)reader.integer<std::uint32_t>();
            }
        } else {
            frame.decode_index = ordinal;
            frame.rap = frame.key_frame;
        }
        // Open-GOP leading pictures are presented before the RAP that starts
        // their decode run, so their anchor may legitimately have a larger
        // presentation index than the frame itself.
        if (frame.frame_index != ordinal || frame.keyframe_anchor >= frame_count) {
            throw std::runtime_error("media index frame identity is invalid");
        }
        index.frames.push_back(std::move(frame));
    }

    index.decode_to_presentation.resize(static_cast<std::size_t>(frame_count));
    index.presentation_to_decode.resize(static_cast<std::size_t>(frame_count));
    std::vector<char> seen_decode_index(static_cast<std::size_t>(frame_count), 0);
    for (std::size_t presentation = 0U;
         presentation < index.frames.size(); ++presentation) {
        const std::uint64_t decode = index.frames[presentation].decode_index;
        if (decode >= frame_count || seen_decode_index[decode]) {
            throw std::runtime_error(
                "media index decode/presentation mapping is invalid");
        }
        seen_decode_index[decode] = 1;
        index.decode_to_presentation[decode] = presentation;
        index.presentation_to_decode[presentation] = decode;
    }

    const std::filesystem::path source = path_from_utf8(media_path);
    const std::uint64_t actual_size = std::filesystem::file_size(source);
    const std::int64_t actual_mtime = file_mtime_ns(source);
    if (index.source_size != actual_size || index.source_mtime_ns != actual_mtime) {
        throw std::runtime_error("media source changed after indexing");
    }
    if (expected_fingerprint && index.fingerprint != *expected_fingerprint) {
        throw std::runtime_error("media fingerprint does not match the index");
    }
    if (expected_stream && index.stream_index != *expected_stream) {
        throw std::runtime_error("media stream does not match the index");
    }
    return index;
}

void write_index_atomic(const std::string &index_path, const MediaIndex &index,
                        std::stop_token stop) {
    const std::filesystem::path destination = path_from_utf8(index_path);
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path());
    }
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    try {
        std::vector<std::uint8_t> bytes;
        const std::string filename = path_to_utf8(destination.filename());
        if (filename.size() >= 7U && filename.ends_with(".vf.lwi")) {
            const std::string text = serialize_lwi(index, index.source_path);
            bytes.assign(text.begin(), text.end());
        } else {
            bytes = serialize(index);
        }
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("failed to create media index temporary file");
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) throw std::runtime_error("failed to write media index temporary file");
        output.close();
        sync_file(temporary);
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("failed to atomically replace media index");
        }
#else
        std::filesystem::rename(temporary, destination);
#endif
    } catch (...) {
        std::filesystem::remove(temporary, cleanup_error);
        throw;
    }
}

IndexedMedia ensure_index(const std::string &path,
                          std::optional<std::uint32_t> requested_stream,
                          const std::string &cache_directory,
                          const DecoderOptions &options, std::stop_token stop,
                          IndexProgress progress) {
    return ensure_index(path, requested_stream, cache_directory, options, {},
                        stop, std::move(progress));
}

IndexedMedia ensure_index(const std::string &path,
                          std::optional<std::uint32_t> requested_stream,
                          const std::string &cache_directory,
                          const DecoderOptions &options,
                          const IndexOptions &index_options, std::stop_token stop,
                          IndexProgress progress) {
    const std::uint32_t default_stream = default_video_stream(path);
    const std::uint32_t stream = requested_stream.value_or(default_stream);
    const std::string fingerprint = quick_fingerprint(path);
    const std::string preferred = preferred_index_path(path, stream, default_stream);
    const std::string external = external_lwi_path(path, stream, default_stream);
    const std::filesystem::path source_path = path_from_utf8(path);
    const std::filesystem::path source_stem = source_path.parent_path() / source_path.stem();
    const std::string stem_external = stream == default_stream
        ? path_to_utf8(source_stem) + ".lwi"
        : path_to_utf8(source_stem) + ".stream-" + std::to_string(stream) + ".lwi";
    const std::string legacy = legacy_lwi_path(path, stream, default_stream);
    MediaIndex shell;
    shell.fingerprint = fingerprint;
    shell.stream_index = stream;
    const std::string fallback = cache_index_path(cache_directory, shell);
    const std::string lock_key = path + '#' + fingerprint + '#' + std::to_string(stream);
    const std::shared_ptr<std::mutex> request_lock = build_lock(lock_key);
    const std::scoped_lock lock(*request_lock);

    const std::vector<const std::string *> candidates = index_options.allow_lwi
        ? std::vector<const std::string *>{&external, &stem_external, &legacy, &preferred, &fallback}
        : std::vector<const std::string *>{&legacy, &preferred, &fallback};
    // LWI has no field that records whether RAP candidates were independently
    // decoded. Reusing any existing index here would silently turn a verified
    // request into an ordinary parser/demuxer index, so verification requests
    // deliberately rebuild.
    if (!index_options.rap_verification) {
        for (const std::string *candidate : candidates) {
            try {
                MediaIndex loaded = load_index(*candidate, path, fingerprint, stream);
                if (loaded.index_mode == "legacy_native") continue;
                return {std::move(loaded), *candidate, false};
            } catch (const std::exception &) {
            }
        }
    }
    if (stop.stop_requested()) throw std::runtime_error("cancelled");
    MediaIndex index = index_media(path, stream, options, index_options, stop, std::move(progress));
    index.source_size = std::filesystem::file_size(source_path);
    index.source_mtime_ns = file_mtime_ns(source_path);
    std::string written_path;
    try {
        index.source_path = path;
        const std::string destination = index_options.generate_compat_lwi ? preferred : fallback;
        write_index_atomic(destination, index, stop);
        written_path = destination;
    } catch (const std::exception &) {
        if (stop.stop_requested()) throw;
        write_index_atomic(fallback, index, stop);
        written_path = fallback;
    }
    return {std::move(index), std::move(written_path), true};
}

std::vector<FrameIdentity> frame_window(const MediaIndex &index,
                                        std::uint64_t target_frame,
                                        std::uint32_t radius) {
    if (index.frames.empty()) throw std::invalid_argument("media index contains no frames");
    target_frame = std::min(target_frame, index.frames.back().frame_index);
    const std::uint64_t first = target_frame > radius ? target_frame - radius : 0U;
    const std::uint64_t last = std::min(
        index.frames.back().frame_index, target_frame + static_cast<std::uint64_t>(radius));
    return {index.frames.begin() + static_cast<std::ptrdiff_t>(first),
            index.frames.begin() + static_cast<std::ptrdiff_t>(last + 1U)};
}

} // namespace getnative::media
