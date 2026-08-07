// Measures compression strategies for serialized AxisPlan data to inform the
// cold plan cache format (docs/cold-plan-cache-evaluation.md).
//
// Candidates:
//   raw      - naive layout (every array stored verbatim)
//   cooked   - structural pre-transform: uniform forward offsets dropped,
//              forward indices reduced to per-row left anchors, CSR offset
//              and index arrays delta-varint encoded
// Each payload is compressed with zstd (levels 1/3/9) and lz4 (default/HC9),
// reporting ratio and throughput on a representative plan corpus.

#include "getnative/axis_plan.hpp"

#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

void put_u32(std::vector<std::byte> &out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}
void put_u64(std::vector<std::byte> &out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}
void put_f32(std::vector<std::byte> &out, float value) {
    put_u32(out, std::bit_cast<std::uint32_t>(value));
}
void put_varint(std::vector<std::byte> &out, std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::byte>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::byte>(value));
}
void put_i32(std::vector<std::byte> &out, std::int32_t value) {
    put_u32(out, static_cast<std::uint32_t>(value));
}

template <typename T>
void put_span_raw(std::vector<std::byte> &out, const std::vector<T> &values) {
    const auto *bytes = reinterpret_cast<const std::byte *>(values.data());
    out.insert(out.end(), bytes, bytes + values.size() * sizeof(T));
}

void put_header(std::vector<std::byte> &out, const getnative::AxisPlan &plan) {
    put_i32(out, plan.source_size);
    put_i32(out, plan.destination_size);
    put_i32(out, plan.support);
    put_i32(out, plan.half_bandwidth);
    put_i32(out, plan.forward_width);
    put_u64(out, std::bit_cast<std::uint64_t>(plan.active_length));
    put_u64(out, std::bit_cast<std::uint64_t>(plan.shift));
}

std::vector<std::byte> serialize_raw(const getnative::AxisPlan &plan) {
    std::vector<std::byte> out;
    out.reserve(getnative::axis_plan_storage_bytes(plan));
    put_header(out, plan);
    put_span_raw(out, plan.forward_offsets);
    put_span_raw(out, plan.forward_indices);
    put_span_raw(out, plan.forward_weights);
    put_span_raw(out, plan.transpose_offsets);
    put_span_raw(out, plan.transpose_indices);
    put_span_raw(out, plan.transpose_weights);
    put_span_raw(out, plan.lower_ld);
    put_span_raw(out, plan.upper_l);
    put_span_raw(out, plan.inverse_diagonal);
    return out;
}

// Structural pre-transform. Lossless: the original arrays are reconstructable
// from (source_size, forward_width) plus the stored anchors/deltas.
std::vector<std::byte> serialize_cooked(const getnative::AxisPlan &plan,
                                        bool &offsets_uniform, bool &indices_runs) {
    std::vector<std::byte> out;
    put_header(out, plan);

    offsets_uniform = true;
    for (std::int32_t row = 0; row <= plan.source_size; ++row) {
        if (plan.forward_offsets[static_cast<std::size_t>(row)]
            != static_cast<std::uint32_t>(
                static_cast<std::size_t>(row)
                * static_cast<std::size_t>(plan.forward_width))) {
            offsets_uniform = false;
            break;
        }
    }
    indices_runs = true;
    for (std::int32_t row = 0; row < plan.source_size && indices_runs; ++row) {
        const std::size_t begin = static_cast<std::size_t>(row)
            * static_cast<std::size_t>(plan.forward_width);
        for (std::int32_t tap = 1; tap < plan.forward_width; ++tap) {
            if (plan.forward_indices[begin + static_cast<std::size_t>(tap)]
                != plan.forward_indices[begin] + tap) {
                indices_runs = false;
                break;
            }
        }
    }

    out.push_back(std::byte{offsets_uniform ? 1U : 0U});
    if (!offsets_uniform) put_span_raw(out, plan.forward_offsets);

    out.push_back(std::byte{indices_runs ? 1U : 0U});
    if (indices_runs) {
        for (std::int32_t row = 0; row < plan.source_size; ++row) {
            put_i32(out, plan.forward_indices[static_cast<std::size_t>(row)
                * static_cast<std::size_t>(plan.forward_width)]);
        }
    } else {
        put_span_raw(out, plan.forward_indices);
    }

    put_span_raw(out, plan.forward_weights);

    std::uint32_t previous = 0;
    for (const std::uint32_t offset : plan.transpose_offsets) {
        put_varint(out, offset - previous);
        previous = offset;
    }
    std::int32_t previous_index = 0;
    for (const std::int32_t index : plan.transpose_indices) {
        put_varint(out, static_cast<std::uint64_t>(
            static_cast<std::int64_t>(index) - previous_index + (1LL << 31)));
        previous_index = index;
    }
    put_span_raw(out, plan.transpose_weights);
    put_span_raw(out, plan.lower_ld);
    put_span_raw(out, plan.upper_l);
    put_span_raw(out, plan.inverse_diagonal);
    return out;
}

struct CodecResult {
    std::string name;
    std::size_t compressed_bytes = 0;
    double compress_ms = 0.0;
    double decompress_ms = 0.0;
    bool verified = false;
};

using CompressFn = std::size_t (*)(void *, const void *, std::size_t, std::size_t);
using DecompressFn = std::size_t (*)(void *, const void *, std::size_t, std::size_t);

CodecResult measure(const std::string &name, const std::vector<std::byte> &payload,
                    CompressFn compress, DecompressFn decompress, int rounds) {
    std::vector<std::byte> scratch(payload.size() * 2U + 64U);
    std::vector<std::byte> restored(payload.size());
    CodecResult result;
    result.name = name;

    const std::size_t compressed = compress(
        scratch.data(), payload.data(), payload.size(), scratch.size());
    result.compressed_bytes = compressed;

    const auto t0 = Clock::now();
    for (int round = 0; round < rounds; ++round) {
        (void)compress(scratch.data(), payload.data(), payload.size(), scratch.size());
    }
    result.compress_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count()
        / static_cast<double>(rounds);

    const auto t1 = Clock::now();
    for (int round = 0; round < rounds; ++round) {
        (void)decompress(restored.data(), scratch.data(), compressed, restored.size());
    }
    result.decompress_ms = std::chrono::duration<double, std::milli>(Clock::now() - t1).count()
        / static_cast<double>(rounds);

    const std::size_t check = decompress(
        restored.data(), scratch.data(), compressed, restored.size());
    result.verified =
        check == payload.size()
        && std::memcmp(restored.data(), payload.data(), payload.size()) == 0;
    return result;
}

getnative::Filter parse_filter(const std::string &id) {
    if (id == "bilinear") return getnative::Filter::bilinear();
    if (id == "bicubic") return getnative::Filter::bicubic(0.0, 0.5);
    if (id == "lanczos3") return getnative::Filter::lanczos(3);
    if (id == "lanczos6") return getnative::Filter::lanczos(6);
    if (id == "spline36") return getnative::Filter::spline36();
    if (id == "spline64") return getnative::Filter::spline64();
    throw std::invalid_argument("unknown filter: " + id);
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::string filter_id = argc > 1 ? argv[1] : "bicubic";
        const std::int32_t source = argc > 2 ? std::stoi(argv[2]) : 1080;
        const std::int32_t start = argc > 3 ? std::stoi(argv[3]) : 700;
        const int count = argc > 4 ? std::stoi(argv[4]) : 400;
        const getnative::Filter filter = parse_filter(filter_id);

        std::vector<std::byte> raw;
        std::vector<std::byte> cooked;
        bool offsets_uniform = true;
        bool indices_runs = true;
        std::size_t logical_bytes = 0;
        for (int index = 0; index < count; ++index) {
            const std::int32_t destination = start + index;
            getnative::AxisPlanRequest request;
            request.source_size = source;
            request.destination_size = destination;
            request.active_length = static_cast<double>(destination);
            request.shift = 0.0;
            request.filter = filter;
            request.border = getnative::BorderMode::mirror;
            const getnative::AxisPlan plan = getnative::build_axis_plan(request);
            if (!plan.valid()) throw std::runtime_error("invalid plan");
            logical_bytes += getnative::axis_plan_storage_bytes(plan);
            const std::vector<std::byte> raw_plan = serialize_raw(plan);
            raw.insert(raw.end(), raw_plan.begin(), raw_plan.end());
            bool plan_offsets = true;
            bool plan_runs = true;
            const std::vector<std::byte> cooked_plan =
                serialize_cooked(plan, plan_offsets, plan_runs);
            cooked.insert(cooked.end(), cooked_plan.begin(), cooked_plan.end());
            offsets_uniform = offsets_uniform && plan_offsets;
            indices_runs = indices_runs && plan_runs;
            if (argc > 5) {
                const std::string path =
                    std::string{argv[5]} + "/" + std::to_string(destination) + ".gnpl";
                if (FILE *file = std::fopen(path.c_str(), "wb")) {
                    (void)std::fwrite(cooked_plan.data(), 1, cooked_plan.size(), file);
                    (void)std::fclose(file);
                }
            }
        }

        std::printf("corpus: %s %d->[%d..%d] x%d plans\n",
                    filter_id.c_str(), source, start, start + count - 1, count);
        std::printf("logical plan storage: %zu bytes (%.1f KiB/plan)\n",
                    logical_bytes,
                    static_cast<double>(logical_bytes) / 1024.0 / count);
        std::printf("raw payload:    %12zu bytes\n", raw.size());
        std::printf("cooked payload: %12zu bytes (offsets_uniform=%d indices_runs=%d)\n",
                    cooked.size(), offsets_uniform ? 1 : 0, indices_runs ? 1 : 0);

        auto zstd_compress = +[](void *dst, const void *src, std::size_t src_size,
                                 std::size_t dst_capacity) -> std::size_t {
            return ZSTD_compress(dst, dst_capacity, src, src_size, 1);
        };
        auto zstd3_compress = +[](void *dst, const void *src, std::size_t src_size,
                                  std::size_t dst_capacity) -> std::size_t {
            return ZSTD_compress(dst, dst_capacity, src, src_size, 3);
        };
        auto zstd9_compress = +[](void *dst, const void *src, std::size_t src_size,
                                  std::size_t dst_capacity) -> std::size_t {
            return ZSTD_compress(dst, dst_capacity, src, src_size, 9);
        };
        auto zstd_decompress = +[](void *dst, const void *src, std::size_t src_size,
                                   std::size_t dst_capacity) -> std::size_t {
            const std::size_t result = ZSTD_decompress(dst, dst_capacity, src, src_size);
            return ZSTD_isError(result) ? 0 : result;
        };
        auto lz4_compress = +[](void *dst, const void *src, std::size_t src_size,
                                std::size_t dst_capacity) -> std::size_t {
            return static_cast<std::size_t>(LZ4_compress_default(
                static_cast<const char *>(src), static_cast<char *>(dst),
                static_cast<int>(src_size), static_cast<int>(dst_capacity)));
        };
        auto lz4hc_compress = +[](void *dst, const void *src, std::size_t src_size,
                                  std::size_t dst_capacity) -> std::size_t {
            return static_cast<std::size_t>(LZ4_compress_HC(
                static_cast<const char *>(src), static_cast<char *>(dst),
                static_cast<int>(src_size), static_cast<int>(dst_capacity), 9));
        };
        auto lz4_decompress = +[](void *dst, const void *src, std::size_t src_size,
                                  std::size_t dst_capacity) -> std::size_t {
            const int result = LZ4_decompress_safe(
                static_cast<const char *>(src), static_cast<char *>(dst),
                static_cast<int>(src_size), static_cast<int>(dst_capacity));
            return result < 0 ? 0U : static_cast<std::size_t>(result);
        };

        struct Variant {
            std::string name;
            const std::vector<std::byte> *payload;
            CompressFn compress;
            DecompressFn decompress;
        };
        const std::vector<Variant> variants = {
            {"raw+zstd1", &raw, zstd_compress, zstd_decompress},
            {"raw+zstd3", &raw, zstd3_compress, zstd_decompress},
            {"raw+zstd9", &raw, zstd9_compress, zstd_decompress},
            {"raw+lz4", &raw, lz4_compress, lz4_decompress},
            {"raw+lz4hc9", &raw, lz4hc_compress, lz4_decompress},
            {"cooked+zstd1", &cooked, zstd_compress, zstd_decompress},
            {"cooked+zstd3", &cooked, zstd3_compress, zstd_decompress},
            {"cooked+zstd9", &cooked, zstd9_compress, zstd_decompress},
            {"cooked+lz4", &cooked, lz4_compress, lz4_decompress},
            {"cooked+lz4hc9", &cooked, lz4hc_compress, lz4_decompress},
        };
        std::printf("%-16s %12s %8s %10s %10s %8s\n", "variant", "bytes", "ratio",
                    "c MB/s", "d MB/s", "verify");
        for (const Variant &variant : variants) {
            const CodecResult measured = measure(
                variant.name, *variant.payload, variant.compress,
                variant.decompress, 7);
            const double ratio = static_cast<double>(raw.size())
                / static_cast<double>(measured.compressed_bytes);
            const double c_mbs = static_cast<double>(variant.payload->size())
                / (1048576.0 * measured.compress_ms / 1000.0);
            const double d_mbs = static_cast<double>(variant.payload->size())
                / (1048576.0 * measured.decompress_ms / 1000.0);
            std::printf("%-16s %12zu %7.2fx %9.0f %9.0f %8s\n",
                        variant.name.c_str(), measured.compressed_bytes, ratio,
                        c_mbs, d_mbs, measured.verified ? "ok" : "FAIL");
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "plan_compression_probe: %s\n", error.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
