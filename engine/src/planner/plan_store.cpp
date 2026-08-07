#include "getnative/plan_store.hpp"

#include "axis_plan_key.hpp"
#include "plan_serialize.hpp"

#include <lz4.h>
#include <xxhash.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace getnative {
namespace {

// ---------------------------------------------------------------------------
// GNPK v2 layout (little-endian throughout)
// ---------------------------------------------------------------------------
//
//   magic "GNPK" u32 | format_version u32 (=3)
//   grid_hash u64 | build_fingerprint u64
//   plan_count u32 | chunk_size u32 | chunk_count u32 | codec u32
//   plan index[plan_count]: {key_hash u64, chunk_ordinal u32,
//                            cooked_offset u32, cooked_len u32}  (sorted)
//   chunk directory[chunk_count]: {file_offset u64, compressed_len u32,
//                                  decompressed_len u32, content_fnv u64}
//   header_checksum u64   (FNV-1a over everything above)
//   chunk data: one compressed block per chunk, plans in grid order;
//               codec 0 = zstd-1 frame, codec 1 = LZ4 block (default)
//
// Chunks are independently decompressable: sparse readers fetch a single
// plan by binary-searching the index, whole-pack preheat streams in
// directory order. LZ4 is the default codec: measured fetch latency is
// dominated by decompression, and LZ4's ~13 GB/s decode beats zstd-1's
// ~3 GB/s at a ~40% size cost (docs/cold-plan-cache-evaluation.md §3.1
// named exactly this escape hatch).

constexpr std::uint32_t kMagic = 0x4B504E47U; // "GNPK" little-endian
constexpr std::uint32_t kFormatVersion = 3;
constexpr std::uint32_t kCodecZstd = 0;
constexpr std::uint32_t kCodecLz4 = 1;
constexpr std::uint32_t kDefaultCodec = kCodecLz4;
constexpr std::size_t kChunkSize = 64;
// magic + version + grid_hash + fingerprint + plan_count + chunk_size
// + chunk_count + codec = 4+4+8+8+4+4+4+4
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kIndexEntryBytes = 20;
constexpr std::size_t kDirectoryEntryBytes = 24;
constexpr std::size_t kMaxPlanCount = 1U << 20;
constexpr std::size_t kMaxPackBytes = 4ULL * 1024U * 1024U * 1024U;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void fnv_mix(std::uint64_t &hash, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= kFnvPrime;
    }
}

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

class ByteReader {
public:
    ByteReader(const std::byte *data, std::size_t size) : data_(data), size_(size) {}
    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(data_[offset_++]) << shift;
        }
        return value;
    }
    std::uint64_t u64() {
        require(8);
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(data_[offset_++]) << shift;
        }
        return value;
    }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    void require(std::size_t bytes) {
        if (size_ - offset_ < bytes) {
            throw detail::PlanStoreError("GNPK header is truncated");
        }
    }
    const std::byte *data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

// Canonical key serialization: fixed little-endian fields, no struct
// padding. Used for both grid hashing and index key hashes.
struct KeyBytes {
    std::array<std::byte, 46> bytes{};
    friend bool operator<(const KeyBytes &lhs, const KeyBytes &rhs) {
        return lhs.bytes < rhs.bytes;
    }
};

// Sortable key serialization: BIG-ENDIAN bytes with sign transforms, so
// lexicographic byte order equals numeric field order (little-endian bytes
// scramble it — that bug scattered job chunks across 2-4 pack chunks and
// multiplied fetch I/O). Signed integers flip the sign bit; IEEE doubles map
// to order-preserving magnitudes (positive: sign bit set; negative: all bits
// flipped). Grid hashes change with this fix by design — old packs miss and
// are rebuilt (and were written for at most one day of builds).
KeyBytes key_bytes(const detail::PlanKey &key) {
    KeyBytes result;
    std::size_t offset = 0;
    const auto put32 = [&](std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            result.bytes[offset++] = static_cast<std::byte>((value >> shift) & 0xFFU);
        }
    };
    const auto put64 = [&](std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            result.bytes[offset++] = static_cast<std::byte>((value >> shift) & 0xFFU);
        }
    };
    const auto put_i32 = [&](std::int32_t value) {
        put32(static_cast<std::uint32_t>(value) ^ 0x80000000U);
    };
    const auto put_f64 = [&](std::uint64_t bits) {
        put64((bits & 0x8000000000000000ULL) != 0U ? ~bits
                                                 : bits | 0x8000000000000000ULL);
    };
    put_i32(key.source_size);
    put_i32(key.destination_size);
    put_f64(key.active_length);
    put_f64(key.shift);
    result.bytes[offset++] = static_cast<std::byte>(key.type);
    put_f64(key.b);
    put_f64(key.c);
    put_i32(key.taps);
    result.bytes[offset++] = static_cast<std::byte>(key.border);
    return result;
}

std::uint64_t hash_bytes(std::span<const std::byte> bytes) {
    std::uint64_t hash = kFnvOffset;
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

struct CanonicalGrid {
    std::vector<AxisPlanRequest> requests;   // sorted by key bytes, deduplicated
    std::vector<std::uint64_t> key_hashes;   // parallel to requests
};

CanonicalGrid canonicalize(std::span<const AxisPlanRequest> requests) {
    std::vector<std::pair<KeyBytes, AxisPlanRequest>> tagged;
    tagged.reserve(requests.size());
    for (const AxisPlanRequest &request : requests) {
        tagged.emplace_back(key_bytes(detail::plan_key(request)), request);
    }
    std::sort(tagged.begin(), tagged.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
    CanonicalGrid grid;
    for (const auto &[bytes, request] : tagged) {
        if (!grid.key_hashes.empty()) {
            const KeyBytes previous = key_bytes(detail::plan_key(grid.requests.back()));
            if (previous.bytes == bytes.bytes) continue;
        }
        grid.key_hashes.push_back(hash_bytes(bytes.bytes));
        grid.requests.push_back(request);
    }
    return grid;
}

struct IndexEntry {
    std::uint64_t key_hash = 0;
    std::uint32_t chunk_ordinal = 0;
    std::uint32_t cooked_offset = 0;
    std::uint32_t cooked_len = 0;
};

struct ChunkRef {
    std::uint64_t file_offset = 0;
    std::uint32_t compressed_len = 0;
    std::uint32_t decompressed_len = 0;
    std::uint64_t content_fnv = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Fingerprint
// ---------------------------------------------------------------------------

std::uint64_t PlanStore::build_fingerprint() {
    std::uint64_t hash = kFnvOffset;
#if defined(_MSC_VER)
    fnv_mix(hash, 0x4D534300000000ULL | static_cast<std::uint64_t>(_MSC_VER));
#elif defined(__clang__)
    fnv_mix(hash, 0x434C4E47000000ULL
        | static_cast<std::uint64_t>(__clang_major__ * 10000
            + __clang_minor__ * 100 + __clang_patchlevel__));
#elif defined(__GNUC__)
    fnv_mix(hash, 0x47434300000000ULL
        | static_cast<std::uint64_t>(__GNUC__ * 10000
            + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__));
#else
    fnv_mix(hash, 0x554E4B4E000000ULL);
#endif
    fnv_mix(hash, static_cast<std::uint64_t>(GETNATIVE_PLANNER_FP_MODE_VALUE));
    fnv_mix(hash, static_cast<std::uint64_t>(
        (GETNATIVE_PLANNER_REUSE_TAPS != 0 ? 1U : 0U)
        | (GETNATIVE_PLANNER_FAST_INTERIOR != 0 ? 2U : 0U)
        | (GETNATIVE_PLANNER_REUSE_SCRATCH != 0 ? 4U : 0U)
        | (GETNATIVE_PLANNER_DIRECT_TRANSPOSE != 0 ? 8U : 0U)
        | (GETNATIVE_PLANNER_PERSISTENT_WORKERS != 0 ? 16U : 0U)));
#if defined(NDEBUG)
    fnv_mix(hash, 1U);
#else
    fnv_mix(hash, 0U);
#endif
    // Bump when planner output semantics change without a flag change.
    fnv_mix(hash, 1U);
    return hash;
}

std::uint64_t PlanStore::grid_hash(std::span<const AxisPlanRequest> requests) {
    const CanonicalGrid grid = canonicalize(requests);
    std::uint64_t hash = kFnvOffset;
    for (const std::uint64_t key_hash : grid.key_hashes) {
        fnv_mix(hash, key_hash);
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

PlanStore::PlanStore(std::filesystem::path directory, Limits limits)
    : directory_(std::move(directory)), limits_(limits) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        throw detail::PlanStoreError("cannot create plan store directory: " + error.message());
    }
    sweep();
}

PlanStore::PlanStore(std::filesystem::path directory)
    : PlanStore(std::move(directory), Limits{}) {}

PlanStore::~PlanStore() = default;

std::filesystem::path PlanStore::pack_path(std::uint64_t grid_hash) const {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx-%016llx.gnpk",
                  static_cast<unsigned long long>(grid_hash),
                  static_cast<unsigned long long>(build_fingerprint()));
    return directory_ / name;
}

namespace {

struct PackFile {
    mutable std::ifstream input;
    std::vector<IndexEntry> index; // sorted by key_hash
    std::vector<ChunkRef> chunks;
    std::uint32_t codec = kCodecLz4;
};

// Reads and validates everything except the chunk payloads: magic, format,
// grid and fingerprint identity, plan/chunk invariants, header checksum,
// index sortedness, and chunk region containment. Only the header region is
// read — chunk payloads are fetched lazily at their directory offsets, so a
// sparse read never touches the rest of the pack. Throws PlanStoreError on
// any violation; returns nullopt only when the file does not exist.
std::optional<PackFile> load_pack_header(
    const std::filesystem::path &path, std::uint64_t grid_hash) {
    PackFile pack;
    pack.input.open(path, std::ios::binary);
    if (!pack.input) return std::nullopt;

    std::array<std::byte, kHeaderBytes> fixed{};
    pack.input.read(reinterpret_cast<char *>(fixed.data()),
                    static_cast<std::streamsize>(fixed.size()));
    if (!pack.input) throw detail::PlanStoreError("GNPK header is truncated");
    ByteReader header(fixed.data(), fixed.size());
    if (header.u32() != kMagic || header.u32() != kFormatVersion) {
        throw detail::PlanStoreError("GNPK magic/version mismatch");
    }
    if (header.u64() != grid_hash) {
        throw detail::PlanStoreError("GNPK grid hash mismatch");
    }
    if (header.u64() != PlanStore::build_fingerprint()) {
        throw detail::PlanStoreError("GNPK build fingerprint mismatch");
    }
    const std::uint32_t plan_count = header.u32();
    const std::uint32_t chunk_size = header.u32();
    const std::uint32_t chunk_count = header.u32();
    const std::uint32_t codec = header.u32();
    if (plan_count == 0 || plan_count > kMaxPlanCount || chunk_size == 0
        || chunk_size > kChunkSize * 4U
        || chunk_count != (plan_count + chunk_size - 1U) / chunk_size
        || codec > kCodecLz4) {
        throw detail::PlanStoreError("GNPK plan/chunk/codec fields are invalid");
    }
    pack.codec = codec;
    const std::uint64_t index_bytes =
        static_cast<std::uint64_t>(plan_count) * kIndexEntryBytes;
    const std::uint64_t directory_bytes =
        static_cast<std::uint64_t>(chunk_count) * kDirectoryEntryBytes;
    const std::uint64_t header_total =
        kHeaderBytes + index_bytes + directory_bytes + 8U;
    if (header_total > kMaxPackBytes) {
        throw detail::PlanStoreError("GNPK header is out of bounds");
    }

    std::vector<std::byte> header_region(static_cast<std::size_t>(header_total));
    std::copy(fixed.begin(), fixed.end(), header_region.begin());
    pack.input.read(reinterpret_cast<char *>(header_region.data() + kHeaderBytes),
                    static_cast<std::streamsize>(header_total - kHeaderBytes));
    if (!pack.input) throw detail::PlanStoreError("GNPK header is truncated");

    ByteReader body(header_region.data() + kHeaderBytes,
                    static_cast<std::size_t>(index_bytes + directory_bytes + 8U));
    pack.index.resize(plan_count);
    for (IndexEntry &entry : pack.index) {
        entry.key_hash = body.u64();
        entry.chunk_ordinal = body.u32();
        entry.cooked_offset = body.u32();
        entry.cooked_len = body.u32();
    }
    pack.chunks.resize(chunk_count);
    for (ChunkRef &chunk : pack.chunks) {
        chunk.file_offset = body.u64();
        chunk.compressed_len = body.u32();
        chunk.decompressed_len = body.u32();
        chunk.content_fnv = body.u64();
    }
    const std::uint64_t stored_checksum = body.u64();
    if (hash_bytes({header_region.data(), static_cast<std::size_t>(header_total - 8U)})
        != stored_checksum) {
        throw detail::PlanStoreError("GNPK header checksum mismatch");
    }
    for (std::size_t position = 1; position < pack.index.size(); ++position) {
        if (pack.index[position].key_hash <= pack.index[position - 1].key_hash) {
            throw detail::PlanStoreError("GNPK index is not sorted");
        }
    }
    std::uint64_t expected_offset = header_total;
    for (const ChunkRef &chunk : pack.chunks) {
        if (chunk.file_offset != expected_offset || chunk.compressed_len == 0) {
            throw detail::PlanStoreError("GNPK chunk regions are invalid");
        }
        expected_offset += chunk.compressed_len;
    }
    for (const IndexEntry &entry : pack.index) {
        if (entry.chunk_ordinal >= chunk_count) {
            throw detail::PlanStoreError("GNPK index chunk ordinal is invalid");
        }
    }
    return pack;
}

// Fetches and decompresses one chunk; the directory's content length and
// XXH64 authenticate content (zstd frames additionally self-check). Each
// call opens its own stream so chunk decodes can run in parallel.
void decode_chunk(const std::filesystem::path &path, const ChunkRef &chunk,
                  std::uint32_t codec, std::vector<std::byte> &out) {
    if (chunk.decompressed_len == 0
        || chunk.decompressed_len > kChunkSize * 4U * 1024U * 1024U) {
        throw detail::PlanStoreError("GNPK chunk content length is invalid");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw detail::PlanStoreError("GNPK chunk open failed");
    std::vector<std::byte> compressed(chunk.compressed_len);
    input.seekg(static_cast<std::streamoff>(chunk.file_offset));
    input.read(reinterpret_cast<char *>(compressed.data()),
               static_cast<std::streamsize>(compressed.size()));
    if (!input) throw detail::PlanStoreError("GNPK chunk read failed");
    out.resize(chunk.decompressed_len);
    if (codec == kCodecLz4) {
        const int decoded = LZ4_decompress_safe(
            reinterpret_cast<const char *>(compressed.data()),
            reinterpret_cast<char *>(out.data()),
            static_cast<int>(compressed.size()), static_cast<int>(out.size()));
        if (decoded < 0 || static_cast<std::size_t>(decoded) != out.size()) {
            throw detail::PlanStoreError("GNPK chunk decompression failed");
        }
    } else {
        const std::size_t decoded = ZSTD_decompress(
            out.data(), out.size(), compressed.data(), compressed.size());
        if (ZSTD_isError(decoded) || decoded != out.size()) {
            throw detail::PlanStoreError("GNPK chunk decompression failed");
        }
    }
    if (XXH64(out.data(), out.size(), 0) != chunk.content_fnv) {
        throw detail::PlanStoreError("GNPK chunk content checksum mismatch");
    }
}

std::shared_ptr<const AxisPlan> decode_plan_entry(
    const IndexEntry &entry, const std::vector<std::byte> &cooked,
    AxisPlanRequest &request) {
    if (static_cast<std::uint64_t>(entry.cooked_offset) + entry.cooked_len
        > cooked.size()) {
        throw detail::PlanStoreError("GNPK cooked offsets are out of range");
    }
    auto plan = std::make_shared<const AxisPlan>(
        detail::deserialize_plan_cooked(
            {cooked.data() + entry.cooked_offset, entry.cooked_len}, &request));
    if (hash_bytes(key_bytes(detail::plan_key(request)).bytes) != entry.key_hash) {
        throw detail::PlanStoreError("GNPK plan key hash mismatch");
    }
    return plan;
}

} // namespace

std::optional<StoredGrid> PlanStore::read_grid(std::uint64_t grid_hash) {
    const std::filesystem::path path = pack_path(grid_hash);
    const auto fail = [&]() -> std::optional<StoredGrid> {
        std::error_code error;
        std::filesystem::remove(path, error);
        return std::nullopt;
    };
    try {
        std::optional<PackFile> pack = load_pack_header(path, grid_hash);
        if (!pack) return std::nullopt; // plain miss: never existed
        // Index entries are hash-sorted (not grid-ordered), so each entry is
        // located within its chunk via the stored offsets.
        std::vector<std::optional<std::vector<std::byte>>> decoded_chunks(
            pack->chunks.size());
        StoredGrid grid;
        grid.requests.reserve(pack->index.size());
        grid.plans.reserve(pack->index.size());
        for (const IndexEntry &entry : pack->index) {
            auto &cooked = decoded_chunks[entry.chunk_ordinal];
            if (!cooked.has_value()) {
                cooked.emplace();
                decode_chunk(path, pack->chunks[entry.chunk_ordinal],
                             pack->codec, *cooked);
            }
            AxisPlanRequest request;
            auto plan = decode_plan_entry(entry, *cooked, request);
            grid.requests.push_back(request);
            grid.plans.push_back(std::move(plan));
        }
        return grid;
    } catch (const detail::PlanStoreError &) {
        return fail();
    } catch (const std::exception &) {
        return fail();
    }
}

std::optional<std::vector<std::shared_ptr<const AxisPlan>>>
PlanStore::read_plans(std::uint64_t grid_hash,
                      std::span<const AxisPlanRequest> requests) {
    const std::filesystem::path path = pack_path(grid_hash);
    const auto fail = [&]() -> std::optional<std::vector<std::shared_ptr<const AxisPlan>>> {
        std::error_code error;
        std::filesystem::remove(path, error);
        return std::nullopt;
    };
    try {
        std::optional<PackFile> pack = load_pack_header(path, grid_hash);
        if (!pack) return std::nullopt;

        // Phase 1: resolve every request through the sorted index
        // (serial; binary search is nanoseconds), then decompress the
        // touched chunks in parallel — each decode opens its own stream.
        std::vector<const IndexEntry *> entries;
        entries.reserve(requests.size());
        std::vector<std::uint32_t> ordinals;
        {
            std::uint32_t last_ordinal = UINT32_MAX;
            for (const AxisPlanRequest &request : requests) {
                const std::uint64_t wanted =
                    hash_bytes(key_bytes(detail::plan_key(request)).bytes);
                const auto found = std::lower_bound(
                    pack->index.begin(), pack->index.end(), wanted,
                    [](const IndexEntry &entry, std::uint64_t hash) {
                        return entry.key_hash < hash;
                    });
                if (found == pack->index.end() || found->key_hash != wanted) {
                    return std::nullopt; // the grid does not contain this plan
                }
                entries.push_back(&*found);
                // Requests arrive in job order; ordinals repeat in runs, so
                // a last-value check collects the distinct set cheaply.
                if (found->chunk_ordinal != last_ordinal) {
                    last_ordinal = found->chunk_ordinal;
                    if (std::find(ordinals.begin(), ordinals.end(), last_ordinal)
                        == ordinals.end()) {
                        ordinals.push_back(last_ordinal);
                    }
                }
            }
        }
        std::unordered_map<std::uint32_t, std::vector<std::byte>> decoded_chunks;
        if (!ordinals.empty()) {
            const std::size_t codec_workers =
                std::min<std::size_t>(8U, ordinals.size());
            if (codec_workers < 2U) {
                for (const std::uint32_t ordinal : ordinals) {
                    decode_chunk(path, pack->chunks[ordinal], pack->codec,
                                 decoded_chunks[ordinal]);
                }
            } else {
                std::exception_ptr failure;
                std::mutex failure_mutex;
                std::atomic_size_t cursor{0U};
                std::vector<std::thread> workers;
                workers.reserve(codec_workers);
                for (std::size_t worker = 0; worker < codec_workers; ++worker) {
                    workers.emplace_back([&] {
                        try {
                            while (true) {
                                const std::size_t position = cursor.fetch_add(
                                    1U, std::memory_order_relaxed);
                                if (position >= ordinals.size()) break;
                                const std::uint32_t ordinal = ordinals[position];
                                std::vector<std::byte> cooked;
                                decode_chunk(path, pack->chunks[ordinal],
                                             pack->codec, cooked);
                                if (cooked.size()
                                    != static_cast<std::size_t>(
                                        pack->chunks[ordinal].decompressed_len)) {
                                    throw detail::PlanStoreError(
                                        "GNPK chunk length mismatch");
                                }
                                const std::scoped_lock lock(failure_mutex);
                                decoded_chunks.emplace(ordinal,
                                                       std::move(cooked));
                            }
                        } catch (...) {
                            const std::scoped_lock lock(failure_mutex);
                            if (!failure) failure = std::current_exception();
                            cursor.store(ordinals.size(),
                                         std::memory_order_relaxed);
                        }
                    });
                }
                for (std::thread &worker : workers) worker.join();
                if (failure) std::rethrow_exception(failure);
            }
        }

        // Phase 2: cooked-blob decode + structural validation dominates the
        // fetch cost and is independent per plan, so it fans out over a
        // small thread batch (the same reason cold builds parallelize).
        std::vector<std::shared_ptr<const AxisPlan>> plans(requests.size());
        std::vector<AxisPlanRequest> decoded_requests(requests.size());
        const std::size_t worker_target =
            std::min<std::size_t>(8U, requests.size());
        if (worker_target < 4U) {
            for (std::size_t position = 0; position < requests.size(); ++position) {
                plans[position] = decode_plan_entry(
                    *entries[position],
                    decoded_chunks.at(entries[position]->chunk_ordinal),
                    decoded_requests[position]);
            }
            return plans;
        }
        std::exception_ptr failure;
        std::mutex failure_mutex;
        {
            std::vector<std::thread> workers;
            workers.reserve(worker_target);
            for (std::size_t worker = 0; worker < worker_target; ++worker) {
                workers.emplace_back([&, worker] {
                    try {
                        for (std::size_t position = worker; position < requests.size();
                             position += worker_target) {
                            plans[position] = decode_plan_entry(
                                *entries[position],
                                decoded_chunks.at(entries[position]->chunk_ordinal),
                                decoded_requests[position]);
                        }
                    } catch (...) {
                        const std::scoped_lock lock(failure_mutex);
                        if (!failure) failure = std::current_exception();
                    }
                });
            }
            for (std::thread &worker : workers) worker.join();
        }
        if (failure) std::rethrow_exception(failure);
        return plans;
    } catch (const detail::PlanStoreError &) {
        return fail();
    } catch (const std::exception &) {
        return fail();
    }
}

bool PlanStore::publish_grid(
    std::uint64_t grid_hash,
    std::span<const AxisPlanRequest> requests,
    std::span<const std::shared_ptr<const AxisPlan>> plans) {
    if (requests.empty() || requests.size() != plans.size()
        || requests.size() > kMaxPlanCount) {
        return false;
    }
    const std::filesystem::path path = pack_path(grid_hash);
    if (std::filesystem::exists(path)) return false; // never replace

    try {
        // Order plans canonically alongside their requests.
        std::vector<std::pair<KeyBytes, std::size_t>> order;
        order.reserve(requests.size());
        for (std::size_t position = 0; position < requests.size(); ++position) {
            order.emplace_back(key_bytes(detail::plan_key(requests[position])), position);
        }
        std::sort(order.begin(), order.end(),
                  [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

        std::vector<std::vector<std::byte>> cooked(requests.size());
        std::vector<IndexEntry> index(requests.size());
        const std::uint32_t chunk_count = static_cast<std::uint32_t>(
            (requests.size() + kChunkSize - 1U) / kChunkSize);
        std::vector<std::vector<std::byte>> compressed_chunks(chunk_count);

        ZSTD_CCtx *context = ZSTD_createCCtx();
        if (context == nullptr) return false;
        struct ContextGuard {
            ZSTD_CCtx *pointer;
            ~ContextGuard() { ZSTD_freeCCtx(pointer); }
        } guard{context};
        ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, 1);
        ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);

        std::vector<std::byte> chunk_buffer;
        std::vector<ChunkRef> chunk_refs(chunk_count);
        for (std::uint32_t ordinal = 0; ordinal < chunk_count; ++ordinal) {
            chunk_buffer.clear();
            const std::size_t begin = static_cast<std::size_t>(ordinal) * kChunkSize;
            const std::size_t end = std::min(begin + kChunkSize, requests.size());
            for (std::size_t position = begin; position < end; ++position) {
                auto blob = detail::serialize_plan_cooked(
                    *plans[order[position].second],
                    requests[order[position].second]);
                index[position] = {
                    hash_bytes(order[position].first.bytes),
                    ordinal,
                    static_cast<std::uint32_t>(chunk_buffer.size()),
                    static_cast<std::uint32_t>(blob.size()),
                };
                chunk_buffer.insert(chunk_buffer.end(), blob.begin(), blob.end());
            }
            std::vector<std::byte> compressed;
            if (kDefaultCodec == kCodecLz4) {
                compressed.resize(static_cast<std::size_t>(
                    LZ4_compressBound(static_cast<int>(chunk_buffer.size()))));
                const int written = LZ4_compress_default(
                    reinterpret_cast<const char *>(chunk_buffer.data()),
                    reinterpret_cast<char *>(compressed.data()),
                    static_cast<int>(chunk_buffer.size()),
                    static_cast<int>(compressed.size()));
                if (written <= 0) return false;
                compressed.resize(static_cast<std::size_t>(written));
            } else {
                compressed.resize(ZSTD_compressBound(chunk_buffer.size()));
                const std::size_t written = ZSTD_compress2(
                    context, compressed.data(), compressed.size(),
                    chunk_buffer.data(), chunk_buffer.size());
                if (ZSTD_isError(written)) return false;
                compressed.resize(written);
            }
            chunk_refs[ordinal].compressed_len =
                static_cast<std::uint32_t>(compressed.size());
            chunk_refs[ordinal].decompressed_len =
                static_cast<std::uint32_t>(chunk_buffer.size());
            chunk_refs[ordinal].content_fnv =
                XXH64(chunk_buffer.data(), chunk_buffer.size(), 0);
            compressed_chunks[ordinal] = std::move(compressed);
        }

        // The on-disk index is sorted by key_hash (binary-searchable);
        // chunk payloads stay in canonical grid order.
        std::sort(index.begin(), index.end(),
                  [](const IndexEntry &lhs, const IndexEntry &rhs) {
                      return lhs.key_hash < rhs.key_hash;
                  });

        // Assemble header + index + directory + checksum + chunk data.
        std::vector<std::byte> file;
        file.reserve(kHeaderBytes + requests.size() * kIndexEntryBytes
                     + chunk_count * kDirectoryEntryBytes + 8U
                     + chunk_buffer.size());
        put_u32(file, kMagic);
        put_u32(file, kFormatVersion);
        put_u64(file, grid_hash);
        put_u64(file, build_fingerprint());
        put_u32(file, static_cast<std::uint32_t>(requests.size()));
        put_u32(file, static_cast<std::uint32_t>(kChunkSize));
        put_u32(file, chunk_count);
        put_u32(file, kDefaultCodec);
        for (const IndexEntry &entry : index) {
            put_u64(file, entry.key_hash);
            put_u32(file, entry.chunk_ordinal);
            put_u32(file, entry.cooked_offset);
            put_u32(file, entry.cooked_len);
        }
        std::uint64_t chunk_offset = kHeaderBytes
            + requests.size() * kIndexEntryBytes
            + chunk_count * kDirectoryEntryBytes + 8U;
        for (std::uint32_t ordinal = 0; ordinal < chunk_count; ++ordinal) {
            put_u64(file, chunk_offset);
            put_u32(file, chunk_refs[ordinal].compressed_len);
            put_u32(file, chunk_refs[ordinal].decompressed_len);
            put_u64(file, chunk_refs[ordinal].content_fnv);
            chunk_offset += chunk_refs[ordinal].compressed_len;
        }
        put_u64(file, hash_bytes(file));
        for (const auto &compressed : compressed_chunks) {
            file.insert(file.end(), compressed.begin(), compressed.end());
        }

        // Atomic publish: write tmp in the same directory, then rename.
        const std::filesystem::path tmp_path =
            path.string() + ".tmp-" + std::to_string(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
        {
            std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(reinterpret_cast<const char *>(file.data()),
                         static_cast<std::streamsize>(file.size()));
            if (!output) {
                std::error_code error;
                std::filesystem::remove(tmp_path, error);
                return false;
            }
        }
        std::error_code error;
        std::filesystem::rename(tmp_path, path, error);
        if (error) {
            // Target appeared concurrently (no-replace policy) or rename
            // failed; either way the store stays consistent without us.
            std::filesystem::remove(tmp_path, error);
            return false;
        }
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

void PlanStore::sweep() {
    std::error_code error;
    struct PackFile {
        std::filesystem::path path;
        std::uintmax_t bytes;
        std::filesystem::file_time_type modified;
    };
    std::vector<PackFile> packs;
    std::uintmax_t total = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory_, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".gnpk") {
            continue;
        }
        const std::uintmax_t bytes = entry.file_size(error);
        if (error) continue;
        packs.push_back({entry.path(), bytes, entry.last_write_time(error)});
        total += bytes;
    }
    if (total <= limits_.maximum_bytes) return;
    std::sort(packs.begin(), packs.end(),
              [](const PackFile &lhs, const PackFile &rhs) {
                  return lhs.modified < rhs.modified;
              });
    for (const PackFile &pack : packs) {
        if (total <= limits_.maximum_bytes) break;
        std::filesystem::remove(pack.path, error);
        if (!error) total -= pack.bytes;
    }
}

} // namespace getnative
