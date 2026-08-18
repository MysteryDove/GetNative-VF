#include "getnative/media_decode.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using getnative::media::ExtraDataInfo;
using getnative::media::FrameIdentity;
using getnative::media::MediaIndex;
using getnative::media::StreamIndexEntry;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path()
            / ("getnative-lwi-compat-"
               + std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path &path,
                 const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    assert(output);
}

std::string external_lwi(int version) {
    std::string text =
        "<LSMASHWorksIndexVersion=0.0.3.0>\n"
        "<LibavReaderIndexFile=" + std::to_string(version) + ">\n"
        "<InputFilePath>fixture.bin</InputFilePath>\n"
        "<FileSize=16>\n"
        "<FileLastModificationTime=0>\n"
        "<LibavReaderIndex=0x208,1,mpegts>\n"
        "<ActiveVideoStreamIndex>+0000000009</ActiveVideoStreamIndex>\n"
        "<ActiveAudioStreamIndex>-0000000001</ActiveAudioStreamIndex>\n"
        "<DefaultAudioStreamIndex>-0000000001</DefaultAudioStreamIndex>\n"
        "<StreamInfo=1,1>\n"
        "Codec=86018,TimeBase=1/48000,Channels=2:0x3,Rate=48000,Format=fltp,BPS=32\n"
        "</StreamInfo>\n"
        "<StreamInfo=9,0>\n"
        "Codec=27,TimeBase=1/1000,Width=1920,Height=1080,Format=yuv420p10le,ColorSpace=9\n"
        "</StreamInfo>\n"
        "Index=9,POS=100,PTS=20,DTS=10,EDI=0\n"
        "Key=1,Pic=1,POC=0,Repeat=1,Field=1,Super=0\n"
        "Index=1,POS=150,PTS=15,DTS=15,EDI=0\n"
        "Key=1,Pic=1,POC=0,Repeat=0,Field=0,Super=0\n"
        "Index=9,POS=200,PTS=10,DTS=20,EDI=1\n"
        "Key=0,Pic=3,POC=2,Repeat=2,Field=2,Super=1\n"
        "</LibavReaderIndex>\n"
        "<VideoConsistentFieldRepeatPict>0</VideoConsistentFieldRepeatPict>\n"
        "<StreamDuration=9,0>3000</StreamDuration>\n"
        "<StreamIndexEntries=1,1,1>\n"
        "POS=50,TS=5,Flags=1,Size=10,Distance=0\n"
        "</StreamIndexEntries>\n"
        "<StreamIndexEntries=9,0,1>\n"
        "POS=100,TS=20,Flags=a,Size=123,Distance=4\n"
        "</StreamIndexEntries>\n"
        "<ExtraDataList=1,1,1>\n"
        "Size=0,Codec=86018,4CC=0x0,Layout=0x3,Rate=48000,Format=fltp,BPS=32,Align=0\n"
        "\n</ExtraDataList>\n"
        "<ExtraDataList=9,0,2>\n"
        "Size=3,Codec=27,4CC=0x31637661,Width=1920,Height=1080,Format=yuv420p10le,BPS=10\n";
    text.push_back('\0');
    text.push_back('\n');
    text.push_back(static_cast<char>(0xff));
    text += "\nSize=0,Codec=27,4CC=0x31637661,Width=1280,Height=720,Format=yuv420p,BPS=8\n"
            "\n</ExtraDataList>\n"
            "</LibavReaderIndexFile>\n";
    return text;
}

std::string partial_timestamp_lwi() {
    return
        "<LSMASHWorksIndexVersion=0.0.3.0>\n"
        "<LibavReaderIndexFile=19>\n"
        "<InputFilePath>fixture.bin</InputFilePath>\n"
        "<FileSize=16>\n"
        "<FileLastModificationTime=0>\n"
        "<LibavReaderIndex=0x208,1,h264>\n"
        "<ActiveVideoStreamIndex>+0000000000</ActiveVideoStreamIndex>\n"
        "<ActiveAudioStreamIndex>-0000000001</ActiveAudioStreamIndex>\n"
        "<DefaultAudioStreamIndex>-0000000001</DefaultAudioStreamIndex>\n"
        "<StreamInfo=0,0>\n"
        "Codec=27,TimeBase=1/1000,Width=1920,Height=1080,Format=yuv420p\n"
        "</StreamInfo>\n"
        "Index=0,POS=100,PTS=100,DTS=90,EDI=0\n"
        "Key=1,Pic=1,POC=0,Repeat=1,Field=0,Super=0\n"
        "Index=0,POS=200,PTS=-9223372036854775808,DTS=100,EDI=0\n"
        "Key=0,Pic=3,POC=-2,Repeat=1,Field=0,Super=0\n"
        "Index=0,POS=300,PTS=110,DTS=110,EDI=0\n"
        "Key=0,Pic=3,POC=2,Repeat=1,Field=0,Super=0\n"
        "</LibavReaderIndex>\n"
        "<VideoConsistentFieldRepeatPict>0</VideoConsistentFieldRepeatPict>\n"
        "<StreamDuration=0,0>120</StreamDuration>\n"
        "</LibavReaderIndexFile>\n";
}

void verify_external_lwi(const std::filesystem::path &media,
                         const std::filesystem::path &index_path,
                         int version) {
    const std::string contents = external_lwi(version);
    write_bytes(index_path, {contents.begin(), contents.end()});
    const MediaIndex index = getnative::media::load_index(
        index_path.string(), media.string(), std::nullopt, 9U);
    assert(index.format_flags == 0x208);
    assert(index.raw_demuxer);
    assert(index.format_name == "mpegts");
    assert(index.stream_index == 9U);
    assert(index.width == 1920 && index.height == 1080);
    assert(index.bit_depth == 10 && index.color_space == 9);
    assert(index.duration_ticks == 3000);
    assert(index.frames.size() == 2U);
    assert(index.frames[0].decode_index == 1U);
    assert(index.frames[0].picture_type == "B");
    assert(index.frames[0].repeat_pict == 2);
    assert(index.frames[0].field_order == "bb");
    assert(index.frames[0].vp9_superframe);
    assert(index.frames[0].extradata_index == 1U);
    assert(index.frames[0].leading_frame);
    assert(index.frames[1].decode_index == 0U && index.frames[1].rap);
    assert(index.stream_index_entries.size() == 1U);
    assert(index.stream_index_entries[0].flags == 0x0aU);
    assert(index.stream_index_entries[0].distance == 4U);
    assert(index.extradata.size() == 2U);
    assert((index.extradata[0].data
            == std::vector<std::uint8_t>{0x00U, 0x0aU, 0xffU}));
    assert(index.extradata[1].width == 1280);
}

void verify_partial_timestamp_lwi(const std::filesystem::path &media,
                                  const std::filesystem::path &index_path) {
    const std::string contents = partial_timestamp_lwi();
    write_bytes(index_path, {contents.begin(), contents.end()});
    const MediaIndex index = getnative::media::load_index(
        index_path.string(), media.string(), std::nullopt, 0U);
    assert(index.frames.size() == 3U);
    assert(index.presentation_to_decode
           == std::vector<std::uint64_t>({1U, 0U, 2U}));
    assert(index.frames[0].decode_index == 1U);
    assert(index.frames[0].leading_frame);
    assert(index.frames[0].keyframe_anchor == 1U);
    assert(index.frames[1].rap);
}

void verify_generated_round_trip(const std::filesystem::path &media,
                                 const std::filesystem::path &index_path) {
    MediaIndex source;
    source.source_path = media.string();
    source.source_size = std::filesystem::file_size(media);
    source.source_mtime_ns = getnative::media::source_mtime_unix_ns(media.string());
    source.fingerprint = getnative::media::quick_fingerprint(media.string());
    source.stream_index = 9U;
    source.width = 1920;
    source.height = 1080;
    source.codec = "h264";
    source.pixel_format = "yuv420p";
    source.bit_depth = 8;
    source.duration_ticks = 3000;
    source.time_base_num = 1;
    source.time_base_den = 1000;
    source.format_name = "mpegts";
    source.format_flags = 0x208;
    source.raw_demuxer = true;
    source.stream_index_entries.push_back(StreamIndexEntry{100, 20, 1, 123, 4});
    source.extradata.push_back(ExtraDataInfo{
        0, 27, 0x31637661U, 1920, 1080, "yuv420p", 8,
        {0x01U, 0x64U, 0x00U, 0x1fU}});

    FrameIdentity leading;
    leading.frame_index = 0;
    leading.decode_index = 1;
    leading.pts = 10;
    leading.dts = 20;
    leading.picture_type = "B";
    leading.repeat_pict = 2;
    leading.field_order = "bb";
    leading.vp9_superframe = true;
    leading.leading_frame = true;
    leading.keyframe_anchor = 1;
    leading.keyframe_timestamp = 20;
    FrameIdentity rap;
    rap.frame_index = 1;
    rap.decode_index = 0;
    rap.pts = 20;
    rap.dts = 10;
    rap.picture_type = "I";
    rap.key_frame = true;
    rap.rap = true;
    rap.repeat_pict = 1;
    rap.field_order = "tt";
    rap.keyframe_anchor = 1;
    rap.keyframe_timestamp = 20;
    source.frames = {leading, rap};
    source.decode_to_presentation = {1, 0};
    source.presentation_to_decode = {1, 0};

    getnative::media::write_index_atomic(index_path.string(), source);
    const MediaIndex loaded = getnative::media::load_index(
        index_path.string(), media.string(), source.fingerprint, 9U);
    assert(loaded.frames.size() == 2U);
    assert(loaded.frames[0].field_order == "bb");
    assert(loaded.frames[0].repeat_pict == 2);
    assert(loaded.frames[0].vp9_superframe);
    assert(loaded.frames[1].rap);
    assert(loaded.stream_index_entries.size() == 1U);
    assert(loaded.extradata.size() == 1U);
    assert(loaded.extradata[0].data == source.extradata[0].data);

    std::filesystem::path binary_path = index_path;
    binary_path.replace_extension("bin");
    source.frames[0].packet_duration = 40;
    source.frames[0].best_effort_timestamp = 10;
    source.frames[0].color_range = 1;
    source.frames[0].color_space = 9;
    source.frames[0].color_primaries = 9;
    source.frames[0].color_transfer = 16;
    source.frames[0].chroma_location = 1;
    getnative::media::write_index_atomic(binary_path.string(), source);
    const MediaIndex binary = getnative::media::load_index(
        binary_path.string(), media.string(), source.fingerprint, 9U);
    assert(binary.frames[0].keyframe_anchor == 1U);
    assert(binary.frames[0].packet_duration == 40);
    assert(binary.frames[0].best_effort_timestamp == 10);
    assert(binary.frames[0].color_space == 9);
    assert(binary.frames[0].color_transfer == 16);
    assert(binary.decode_to_presentation == source.decode_to_presentation);
    assert(binary.presentation_to_decode == source.presentation_to_decode);
}

void verify_indexed_matcher_and_streaming(const std::string &media_path) {
    const std::uint32_t stream = getnative::media::default_video_stream(media_path);
    const MediaIndex index = getnative::media::index_media(media_path, stream);
    assert(index.frames.size() >= 24U);
    assert(index.index_mode == "packet_fast");
    getnative::media::DecoderOptions options;
    options.output_luma = false;
    options.output_rgb = false;

    getnative::media::DecodeTelemetry streaming_telemetry;
    std::size_t streamed = 0U;
    bool stopped = false;
    try {
        getnative::media::decode_selected_indexed(
            media_path, index, index.frames, options, {},
            [&](getnative::media::HostFrame) {
                if (++streamed == 3U) throw std::runtime_error("streaming-stop");
            },
            &streaming_telemetry);
    } catch (const std::runtime_error &error) {
        stopped = std::string_view{error.what()} == "streaming-stop";
    }
    assert(stopped && streamed == 3U);
    assert(streaming_telemetry.decoded_frames < index.frames.size() / 2U);

    MediaIndex duplicate = index;
    constexpr std::size_t target_ordinal = 10U;
    constexpr std::size_t neighbor_ordinal = 11U;
    duplicate.frames[target_ordinal].pts = duplicate.frames[neighbor_ordinal].pts;
    duplicate.frames[target_ordinal].best_effort_timestamp =
        duplicate.frames[neighbor_ordinal].best_effort_timestamp;
    std::vector<FrameIdentity> duplicate_target{duplicate.frames[target_ordinal]};
    std::size_t duplicate_outputs = 0U;
    bool duplicate_rejected = false;
    try {
        getnative::media::decode_selected_indexed(
            media_path, duplicate, duplicate_target, options, {},
            [&](getnative::media::HostFrame) { ++duplicate_outputs; });
    } catch (const std::runtime_error &) {
        duplicate_rejected = true;
    }
    assert(duplicate_rejected && duplicate_outputs == 0U);

    MediaIndex mixed = index;
    mixed.frames[target_ordinal].pts.reset();
    mixed.frames[target_ordinal].best_effort_timestamp.reset();
    mixed.frames[target_ordinal].dts.reset();
    const std::vector<FrameIdentity> mixed_targets{
        mixed.frames[target_ordinal - 1U], mixed.frames[target_ordinal],
        mixed.frames[target_ordinal + 1U]};
    std::vector<std::uint64_t> delivered;
    getnative::media::decode_selected_indexed(
        media_path, mixed, mixed_targets, options, {},
        [&](getnative::media::HostFrame frame) {
            delivered.push_back(frame.identity.frame_index);
        });
    assert(delivered == std::vector<std::uint64_t>(
        {target_ordinal - 1U, target_ordinal, target_ordinal + 1U}));
}

void verify_i_picture_selection() {
    MediaIndex index;
    index.frames.resize(4U);
    for (std::size_t ordinal = 0U; ordinal < index.frames.size(); ++ordinal) {
        index.frames[ordinal].frame_index = ordinal;
        index.frames[ordinal].keyframe_anchor = 0U;
    }
    index.frames[0].key_frame = true;
    index.frames[0].rap = true;
    index.frames[0].picture_type = "I";
    // An intra picture is not necessarily a random-access point. It still
    // belongs in decoded_i_picture and must retain its preceding RAP anchor.
    index.frames[1].picture_type = "I";
    index.frames[2].key_frame = true;
    index.frames[2].picture_type = "P";
    // Legacy indexes without Pic metadata retain the old key-frame fallback.
    index.frames[3].key_frame = true;

    getnative::media::ScanScope scope;
    scope.selection = getnative::media::ScanSelection::decoded_i_picture;
    const std::vector<FrameIdentity> selected =
        getnative::media::select_frames(index, scope);
    assert(selected.size() == 3U);
    assert(selected[0].frame_index == 0U);
    assert(selected[1].frame_index == 1U);
    assert(selected[1].keyframe_anchor == 0U);
    assert(selected[2].frame_index == 3U);
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string_view{argv[1]} == "--verify-decode-planner") {
        verify_indexed_matcher_and_streaming(argv[2]);
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--verify-rap") {
        const std::string media_path = argv[2];
        const std::uint32_t stream = getnative::media::default_video_stream(media_path);
        getnative::media::IndexOptions options;
        options.rap_verification = true;
        const MediaIndex index = getnative::media::index_media(
            media_path, stream, options);
        assert(index.index_mode == "packet_rebuilt");
        std::size_t rap_count = 0U;
        for (const FrameIdentity &frame : index.frames) {
            if (!frame.rap) continue;
            ++rap_count;
            assert(frame.key_frame);
            assert(frame.picture_type == "I");
        }
        assert(rap_count > 0U);

        TemporaryDirectory cache;
        getnative::media::DecoderOptions decoder_options;
        options.generate_compat_lwi = false;
        const auto first = getnative::media::ensure_index(
            media_path, stream, cache.path().string(), decoder_options, options);
        const auto second = getnative::media::ensure_index(
            media_path, stream, cache.path().string(), decoder_options, options);
        assert(first.rebuilt && second.rebuilt);
        return 0;
    }
    TemporaryDirectory temporary;
    const std::filesystem::path media = temporary.path() / "fixture.bin";
    write_bytes(media, std::vector<std::uint8_t>(16U, 0x5aU));
    verify_external_lwi(media, temporary.path() / "external-v18.lwi", 18);
    verify_external_lwi(media, temporary.path() / "external-v19.lwi", 19);
    verify_partial_timestamp_lwi(media, temporary.path() / "partial-v19.lwi");
    verify_generated_round_trip(media, temporary.path() / "generated.vf.lwi");
    verify_i_picture_selection();
    return 0;
}
