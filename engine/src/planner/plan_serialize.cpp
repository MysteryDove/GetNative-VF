#include "plan_serialize.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace getnative::detail {
namespace {

// Sanity bounds mirrored from the capability envelope and AxisPlan::valid().
constexpr std::int32_t kMaxAxisLength = 65536;
constexpr std::int32_t kMaxHalfBandwidth = 29;
constexpr std::int32_t kMaxForwardWidth = 30;

void put_u8(std::vector<std::byte> &out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
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
void put_i32(std::vector<std::byte> &out, std::int32_t value) {
    put_u32(out, static_cast<std::uint32_t>(value));
}
void put_f64(std::vector<std::byte> &out, double value) {
    put_u64(out, std::bit_cast<std::uint64_t>(value));
}
template <typename T>
void put_span_raw(std::vector<std::byte> &out, const std::vector<T> &values) {
    const auto *bytes = reinterpret_cast<const std::byte *>(values.data());
    out.insert(out.end(), bytes, bytes + values.size() * sizeof(T));
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> payload) : payload_(payload) {}

    std::uint8_t u8() {
        require(1);
        return static_cast<std::uint8_t>(payload_[offset_++]);
    }
    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(payload_[offset_++]) << shift;
        }
        return value;
    }
    std::uint64_t u64() {
        require(8);
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(payload_[offset_++]) << shift;
        }
        return value;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }
    double f64() { return std::bit_cast<double>(u64()); }
    template <typename T>
    void raw_into(std::vector<T> &out, std::size_t count) {
        require(count * sizeof(T));
        out.resize(count);
        std::memcpy(out.data(), payload_.data() + offset_, count * sizeof(T));
        offset_ += count * sizeof(T);
    }

    [[nodiscard]] bool exhausted() const noexcept { return offset_ == payload_.size(); }

private:
    void require(std::size_t bytes) {
        if (payload_.size() - offset_ < bytes) {
            throw PlanStoreError("cooked plan payload is truncated");
        }
    }

    std::span<const std::byte> payload_;
    std::size_t offset_ = 0;
};

void require_finite(const std::vector<float> &values, const char *label) {
    for (const float value : values) {
        if (!std::isfinite(value)) {
            throw PlanStoreError(std::string{"cooked plan has non-finite "} + label);
        }
    }
}

} // namespace

std::vector<std::byte> serialize_plan_cooked(
    const AxisPlan &plan, const AxisPlanRequest &request) {
    if (!plan.valid()) {
        throw PlanStoreError("cannot serialize an invalid plan");
    }
    if (request.source_size != plan.source_size
        || request.destination_size != plan.destination_size) {
        throw PlanStoreError("cooked header/request geometry mismatch");
    }

    std::vector<std::byte> out;
    out.reserve(axis_plan_storage_bytes(plan));

    // Header: plan scalars plus the plan-key fields that make the blob
    // self-describing (kernel/border/parameter bits).
    put_i32(out, plan.source_size);
    put_i32(out, plan.destination_size);
    put_i32(out, plan.support);
    put_i32(out, plan.half_bandwidth);
    put_i32(out, plan.forward_width);
    put_i32(out, request.filter.taps);
    put_u8(out, static_cast<std::uint8_t>(request.filter.type));
    put_u8(out, static_cast<std::uint8_t>(request.border));
    put_u8(out, 0U); // reserved
    put_u8(out, 0U); // reserved
    put_f64(out, plan.active_length);
    put_f64(out, plan.shift);
    put_f64(out, request.filter.b);
    put_f64(out, request.filter.c);
    put_f64(out, request.filter.blur);

    // Structural invariants verified against every plan the planner emits;
    // the flags keep the format honest if a future planner breaks them.
    bool offsets_uniform = true;
    for (std::int32_t row = 0; row <= plan.source_size && offsets_uniform; ++row) {
        if (plan.forward_offsets[static_cast<std::size_t>(row)]
            != static_cast<std::uint32_t>(
                static_cast<std::size_t>(row)
                * static_cast<std::size_t>(plan.forward_width))) {
            offsets_uniform = false;
        }
    }
    bool indices_runs = true;
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

    put_u8(out, offsets_uniform ? 1U : 0U);
    if (!offsets_uniform) put_span_raw(out, plan.forward_offsets);

    put_u8(out, indices_runs ? 1U : 0U);
    if (indices_runs) {
        for (std::int32_t row = 0; row < plan.source_size; ++row) {
            put_i32(out, plan.forward_indices[static_cast<std::size_t>(row)
                * static_cast<std::size_t>(plan.forward_width)]);
        }
    } else {
        put_span_raw(out, plan.forward_indices);
    }

    put_span_raw(out, plan.forward_weights);

    // v4: transpose offsets/indices are stored raw. The v3 varint/delta
    // cooking made them smaller but the decode loops dominated fetch
    // latency (~60% of a 200 ms/1000-plan fetch; io+codec+hash is the
    // rest), and LZ4 recovers most of the size delta on its own.
    put_span_raw(out, plan.transpose_offsets);
    put_span_raw(out, plan.transpose_indices);
    put_span_raw(out, plan.transpose_weights);
    put_span_raw(out, plan.lower_ld);
    put_span_raw(out, plan.upper_l);
    put_span_raw(out, plan.inverse_diagonal);
    return out;
}

AxisPlan deserialize_plan_cooked(
    std::span<const std::byte> payload, AxisPlanRequest *request_out) {
    Reader reader(payload);
    AxisPlan plan;
    plan.source_size = reader.i32();
    plan.destination_size = reader.i32();
    plan.support = reader.i32();
    plan.half_bandwidth = reader.i32();
    plan.forward_width = reader.i32();
    const std::int32_t taps = reader.i32();
    const auto kernel = static_cast<std::uint8_t>(reader.u8());
    const auto border = static_cast<std::uint8_t>(reader.u8());
    (void)reader.u8();
    (void)reader.u8();
    plan.active_length = reader.f64();
    plan.shift = reader.f64();
    const double b = reader.f64();
    const double c = reader.f64();
    const double blur = reader.f64();

    if (plan.source_size < 2 || plan.source_size > kMaxAxisLength
        || plan.destination_size < 2 || plan.destination_size > kMaxAxisLength
        || plan.destination_size >= plan.source_size
        || plan.support < 1 || plan.support > kMaxAxisLength
        || plan.half_bandwidth < 1 || plan.half_bandwidth > kMaxHalfBandwidth
        || plan.forward_width < 1 || plan.forward_width > kMaxForwardWidth
        || taps < 1 || taps > 15
        || kernel > 5U || border > 2U
        || !std::isfinite(plan.active_length) || plan.active_length <= 0.0
        || !std::isfinite(plan.shift)
        || !std::isfinite(b) || !std::isfinite(c)
        || !(blur > 0.0) || !(blur <= std::numeric_limits<double>::max())) {
        throw PlanStoreError("cooked plan header is out of bounds");
    }

    const auto src = static_cast<std::size_t>(plan.source_size);
    const auto dst = static_cast<std::size_t>(plan.destination_size);
    const auto forward_width = static_cast<std::size_t>(plan.forward_width);

    const bool offsets_uniform = reader.u8() != 0;
    plan.forward_offsets.resize(src + 1U);
    if (offsets_uniform) {
        for (std::size_t row = 0; row <= src; ++row) {
            plan.forward_offsets[row] = static_cast<std::uint32_t>(row * forward_width);
        }
    } else {
        for (std::size_t row = 0; row <= src; ++row) {
            plan.forward_offsets[row] = reader.u32();
        }
    }
    if (plan.forward_offsets.back() != src * forward_width) {
        throw PlanStoreError("cooked forward offsets do not span the forward matrix");
    }

    const bool indices_runs = reader.u8() != 0;
    plan.forward_indices.resize(src * forward_width);
    if (indices_runs) {
        for (std::size_t row = 0; row < src; ++row) {
            const std::int32_t left = reader.i32();
            for (std::size_t tap = 0; tap < forward_width; ++tap) {
                plan.forward_indices[row * forward_width + tap] =
                    left + static_cast<std::int32_t>(tap);
            }
        }
    } else {
        reader.raw_into(plan.forward_indices, src * forward_width);
    }
    reader.raw_into(plan.forward_weights, src * forward_width);

    // v4: raw transpose offsets/indices with one checked pass (monotone
    // offsets, in-range indices) instead of per-element varint decoding.
    plan.transpose_offsets.resize(dst + 1U);
    reader.raw_into(plan.transpose_offsets, dst + 1U);
    if (plan.transpose_offsets.front() != 0U) {
        throw PlanStoreError("cooked transpose offsets do not start at zero");
    }
    for (std::size_t row = 1U; row <= dst; ++row) {
        if (plan.transpose_offsets[row] < plan.transpose_offsets[row - 1U]) {
            throw PlanStoreError("cooked transpose offsets are not monotone");
        }
    }
    const std::size_t nnz = plan.transpose_offsets.back();
    if (nnz > static_cast<std::size_t>(kMaxAxisLength) * kMaxForwardWidth) {
        throw PlanStoreError("cooked transpose offsets are out of bounds");
    }
    plan.transpose_indices.resize(nnz);
    reader.raw_into(plan.transpose_indices, nnz);
    for (const std::int32_t index : plan.transpose_indices) {
        // Transpose indices address source observations (inverse pass input).
        if (index < 0 || index >= plan.source_size) {
            throw PlanStoreError("cooked transpose index is out of range");
        }
    }
    reader.raw_into(plan.transpose_weights, nnz);

    const std::size_t factors = static_cast<std::size_t>(plan.half_bandwidth) * dst;
    reader.raw_into(plan.lower_ld, factors);
    reader.raw_into(plan.upper_l, factors);
    reader.raw_into(plan.inverse_diagonal, dst);

    if (!reader.exhausted()) {
        throw PlanStoreError("cooked plan payload has trailing bytes");
    }
    require_finite(plan.forward_weights, "forward weights");
    require_finite(plan.transpose_weights, "transpose weights");
    require_finite(plan.lower_ld, "lower factor band");
    require_finite(plan.upper_l, "upper factor band");
    require_finite(plan.inverse_diagonal, "inverse diagonal");
    if (!plan.valid()) {
        throw PlanStoreError("cooked plan fails structural validation");
    }

    if (request_out != nullptr) {
        Filter filter;
        filter.type = static_cast<KernelType>(kernel);
        filter.b = b;
        filter.c = c;
        filter.taps = taps;
        filter.blur = blur;
        request_out->source_size = plan.source_size;
        request_out->destination_size = plan.destination_size;
        request_out->active_length = plan.active_length;
        request_out->shift = plan.shift;
        request_out->filter = filter;
        request_out->border = static_cast<BorderMode>(border);
    }
    return plan;
}

} // namespace getnative::detail
