# Vulkan Video bitstream padding corruption

Linux RTX 5080, NVIDIA 595.84, FFmpeg 8.0.1: a formal automatic-versus-single
34072-frame Verify comparison failed at frames 18510, 18511, and 18513-18520
of the local `00001.m2ts` fixture. Frame identities and coverage were correct.
The failure reproduced with one decoder, starting the selected interval at
18303, and with analysis concurrency one. Starting at 17051 produced the older
full-job values. Neither full Vulkan validation nor its parameter-lifetime
workaround reported an error for the short reproducer.

## Cause and repair

`libavcodec/vulkan_decode.c` aligns `srcBufferRange` beyond the last slice.
FFmpeg 8.0.1 and the project's pinned 8.1.2 leave that tail uninitialized in
recycled buffers. On the tested driver, prior buffer contents change decoded
pixels. Zeroing `[slices_size, data_size)` before submission fixes the observed
corruption. Noncoherent mapped-memory flushing must include that padding too.

The retained patch is `scripts/patches/ffmpeg-vulkan-bitstream-padding.patch`.
Linux and Windows SDK build scripts apply it with a checked context, include it
alongside the upstream source archive, and record
`GETNATIVE_VULKAN_BITSTREAM_PADDING=zero` in `BUILD_INFO.txt`. Their CI cache keys
include the patch. This does not modify the system FFmpeg or GPU driver.
An external/system FFmpeg without this patch remains affected on this host;
the project build option alone cannot repair a separately loaded runtime.

## Evidence

The isolated root is `/home/owen/tmp/gnvf-tune-20260905` on the Linux host.

- `adaptive-evidence/paired-vulkan-tail-34072`: formal comparison stopped on
  exact parity failure. Do not use its partial timing as an accepted result.
- `vulkan-seek-18303.json`, `vulkan-seek-serial.json`, `vulkan-seek-vvl.json`:
  the same ten error-value differences recur without adaptive concurrency.
- `vulkan-seek-stock-ffmpeg.json` and `vulkan-seek-padding.json`: same isolated
  FFmpeg 8.0.1 build recipe, with only the padding repair in the latter library.
- `pixels-full-0.txt`: standalone software decode from the beginning, luma MD5,
  extrema and sums for the 11 frames at PTS 447481912 through 447519450.
- `pixels-vulkan-padding.txt`: standalone Vulkan seek to byte 4309946880,
  then hardware download. Every recorded pixel statistic and MD5 equals the
  software reference. Both `pixels-vulkan-seek.txt` and `pixels-full-1.txt`
  from unpatched Vulkan differ from that reference.

The pixel probe does not use GetNative's Vulkan conversion or analysis kernels.
Thus the old Vulkan single-session numerical result is itself affected;
restoring those old error values is not a correctness criterion. Formal
single/automatic parity must be rerun with the same repaired FFmpeg runtime.

The project Linux build script subsequently produced the pinned 8.1.2 SDK with
the patch, and the same raw-pixel check passed. The repaired 34072-frame
interleaved three-pair comparison passed strict frame parity for all warmup and
measured jobs. Fixed-one median was 42151.348342 ms (808.325269 fps); automatic
median was 42211.916329 ms (807.165440 fps), a 0.143692% elapsed-time increase.
Automatic tested two sessions then reverted to one in every measured job.
Elapsed-time spreads were 0.0310% fixed and 0.0505% automatic. Whole-card sampled
memory peaks including warmup were 855 MiB and 947 MiB respectively.
`paired-vulkan-padding-34072/summary.json` records engine and resolved FFmpeg
library SHA-256 hashes. This comparison supersedes the failed unpatched parity
run; it does not waive other platform or mode gates.

The long-reference-slot search-bound hypothesis and a dedicated-DPB diagnostic
change did not remove the corruption and are not included in the patch.
The validation-layer crash has a separate investigation in
[vulkan-validation-lifetime.md](vulkan-validation-lifetime.md).

Windows execution, other vendors, the complete 8/10-bit mode matrix and release
default enablement remain unverified at this checkpoint.

## FFmpeg 8.1.2 view contract

The first full 8.1.2 validation run had exact frame parity but reported
`VUID-VkVideoBeginCodingInfoKHR-slotIndex-07245`. In coincident output/DPB mode,
FFmpeg created the output view with only `VIDEO_DECODE_DST` usage and later
reused that same view as a DPB reference. The underlying image had both usages;
the narrowed view did not. This is not a validation-layer false positive.

`ffmpeg-vulkan-coincident-view-usage.patch` selects DPB view usage based on
`!alloc_dpb`, rather than whether the picture happens to be current at creation.
Both SDK scripts also retain this patch and record
`GETNATIVE_VULKAN_COINCIDENT_VIEW_USAGE=decode_dst_dpb`. The performance numbers
above were captured before this additional view-contract repair; final-runtime
validation and performance evidence must identify both patches.
