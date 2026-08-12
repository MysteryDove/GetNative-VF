#include "getnative/media_decode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <vector>

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
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
}

std::uint8_t picture_code(const std::optional<std::string> &value) {
    if (!value) return 0U;
    if (*value == "I") return 1U;
    if (*value == "P") return 2U;
    if (*value == "B") return 3U;
    return 4U;
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
    output.string(index.decoder);
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
    return (std::filesystem::path{cache_directory}
            / (key + ".stream-" + std::to_string(index.stream_index) + ".gnvf.lwi"))
        .string();
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
    return path + (stream_index == default_stream_index
        ? ".gnvf.lwi"
        : ".stream-" + std::to_string(stream_index) + ".lwi");
}

MediaIndex load_index(const std::string &index_path, const std::string &media_path,
                      std::optional<std::string_view> expected_fingerprint,
                      std::optional<std::uint32_t> expected_stream) {
    std::ifstream input(index_path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("media index does not exist");
    const std::streamoff raw_size = input.tellg();
    if (raw_size < 0 || raw_size > static_cast<std::streamoff>(2ULL * 1024ULL * 1024ULL * 1024ULL)) {
        throw std::runtime_error("media index size is invalid");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(raw_size));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), raw_size);
    if (!input) throw std::runtime_error("failed to read media index");
    Reader reader(bytes);
    for (const std::uint8_t byte : kMagic) {
        if (reader.integer<std::uint8_t>() != byte) {
            throw std::runtime_error("media index magic is invalid");
        }
    }
    const std::uint32_t version = reader.integer<std::uint32_t>();
    if (version != MediaIndex::format_version) {
        throw std::runtime_error("media index version is incompatible");
    }
    (void)reader.integer<std::uint32_t>();
    MediaIndex index;
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
    index.decoder = reader.string();
    constexpr std::size_t frame_bytes = 52U;
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
        if (frame.frame_index != ordinal || frame.keyframe_anchor > frame.frame_index) {
            throw std::runtime_error("media index frame identity is invalid");
        }
        index.frames.push_back(std::move(frame));
    }

    const std::filesystem::path source{media_path};
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
    const std::filesystem::path destination{index_path};
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path());
    }
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    try {
        const std::vector<std::uint8_t> bytes = serialize(index);
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
    const std::uint32_t default_stream = default_video_stream(path);
    const std::uint32_t stream = requested_stream.value_or(default_stream);
    const std::string fingerprint = quick_fingerprint(path);
    const std::string preferred = preferred_index_path(path, stream, default_stream);
    MediaIndex shell;
    shell.fingerprint = fingerprint;
    shell.stream_index = stream;
    const std::string fallback = cache_index_path(cache_directory, shell);
    const std::string lock_key = path + '#' + fingerprint + '#' + std::to_string(stream);
    const std::shared_ptr<std::mutex> request_lock = build_lock(lock_key);
    const std::scoped_lock lock(*request_lock);

    for (const std::string *candidate : {&preferred, &fallback}) {
        try {
            return {load_index(*candidate, path, fingerprint, stream), *candidate, false};
        } catch (const std::exception &) {
        }
    }
    if (stop.stop_requested()) throw std::runtime_error("cancelled");
    MediaIndex index = index_media(path, stream, options, stop, std::move(progress));
    index.source_size = std::filesystem::file_size(path);
    index.source_mtime_ns = file_mtime_ns(path);
    std::string written_path;
    try {
        write_index_atomic(preferred, index, stop);
        written_path = preferred;
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
