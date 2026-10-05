#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace getnative {

// Transfer curve assumed for the encoded samples when testing a linear-light
// hypothesis: the frame is decoded to linear light with the chosen curve
// before the descale/rescale round trip, and the error is measured there.
// `none` keeps the samples exactly as encoded (the default behaviour).
//
// The numeric values are the wire/kernel ids shared with the GPU backends.
enum class TransferCurve : std::uint32_t {
    none = 0,
    gamma22 = 1,
    bt1886 = 2, // pure 2.4 power (BT.1886 with zero black level)
    srgb = 3,
    bt709 = 4, // inverse of the BT.709 / BT.601 camera OETF
};

// How the F32 samples relate to nominal black and white.
//  - limited: studio-range video on the engine's code * 2^(16-depth) / 65535
//    scale, i.e. black sits at 16/256 and white at 235/256. Those must be
//    stretched to 0..1 before a transfer curve means anything.
//  - full: samples already span 0..1 (stills, full-range video).
enum class SampleRange : std::uint32_t {
    full = 0,
    limited = 1,
};

struct TransferSpec {
    TransferCurve curve = TransferCurve::none;
    SampleRange range = SampleRange::full;

    [[nodiscard]] constexpr bool active() const noexcept {
        return curve != TransferCurve::none;
    }
    friend constexpr bool operator==(const TransferSpec &, const TransferSpec &) = default;
};

[[nodiscard]] constexpr std::string_view transfer_curve_name(TransferCurve curve) noexcept {
    switch (curve) {
    case TransferCurve::none: return "none";
    case TransferCurve::gamma22: return "gamma22";
    case TransferCurve::bt1886: return "bt1886";
    case TransferCurve::srgb: return "srgb";
    case TransferCurve::bt709: return "bt709";
    }
    return "none";
}

[[nodiscard]] constexpr std::optional<TransferCurve> parse_transfer_curve(
    std::string_view name) noexcept {
    for (const TransferCurve curve : {TransferCurve::none, TransferCurve::gamma22,
                                      TransferCurve::bt1886, TransferCurve::srgb,
                                      TransferCurve::bt709}) {
        if (transfer_curve_name(curve) == name) return curve;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view sample_range_name(SampleRange range) noexcept {
    return range == SampleRange::limited ? "limited" : "full";
}

// Video range metadata as reported by the decoder ("limited", "full",
// "unknown"). Unspecified video is studio range by convention.
[[nodiscard]] constexpr SampleRange video_sample_range(std::string_view range) noexcept {
    return range == "full" ? SampleRange::full : SampleRange::limited;
}

// Stretch studio-range samples to nominal 0..1 (8-bit codes 16..235).
[[nodiscard]] inline float expand_sample_range(SampleRange range, float sample) noexcept {
    return range == SampleRange::limited
        ? (sample * (65535.0F / 256.0F) - 16.0F) / 219.0F
        : sample;
}

// Nominal 0..1 encoded value -> linear light. Negative input clamps to zero;
// input above one extends the curve, so over-range samples stay ordered.
[[nodiscard]] inline float transfer_eotf(TransferCurve curve, float encoded) noexcept {
    const float value = encoded > 0.0F ? encoded : 0.0F;
    switch (curve) {
    case TransferCurve::none: return encoded;
    case TransferCurve::gamma22: return std::pow(value, 2.2F);
    case TransferCurve::bt1886: return std::pow(value, 2.4F);
    case TransferCurve::srgb:
        return value <= 0.04045F ? value / 12.92F
                                 : std::pow((value + 0.055F) / 1.055F, 2.4F);
    case TransferCurve::bt709:
        return value < 0.081F ? value / 4.5F
                              : std::pow((value + 0.099F) / 1.099F, 1.0F / 0.45F);
    }
    return encoded;
}

// Decoded F32 sample -> linear light under `spec`; identity when no curve is set.
[[nodiscard]] inline float transfer_to_linear(TransferSpec spec, float sample) noexcept {
    if (!spec.active()) return sample;
    return transfer_eotf(spec.curve, expand_sample_range(spec.range, sample));
}

inline void transfer_to_linear(TransferSpec spec, std::span<float> samples) noexcept {
    if (!spec.active()) return;
    for (float &sample : samples) sample = transfer_to_linear(spec, sample);
}

} // namespace getnative
