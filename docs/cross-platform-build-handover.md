# Cross-platform build handover

## Scope

The supported native CI targets are macOS ARM64, Linux x86_64, and Linux
ARM64. Windows x86_64 remains covered by the existing package/remote workflow.
This handover does not add MoltenVK, Lavapipe device testing, Raspberry Pi
v3dv evidence, Tauri ARM64 packaging, or NEON hw=9 specialization.

## Linux Vulkan toolchain

Linux Vulkan CI installs `glslang-tools`, `spirv-tools`, and `libvulkan-dev`
from the distribution. LunarG does not provide a Linux ARM64 SDK. CMake first
checks `Vulkan::Vulkan` and falls back to explicit loader discovery under
`/usr/lib/aarch64-linux-gnu`, `/usr/lib/x86_64-linux-gnu`, `/usr/lib64`, and
`/usr/lib`. Shader compilation accepts `glslc` or `glslangValidator`; SPIR-V
outputs are architecture-independent.

The macOS upstream descale/zimg conformance job sets
`GETNATIVE_PLANNER_FP_MODE=strict` so bit-level reference comparisons are not
affected by the production planner's optional `-ffast-math` mode.

CPU-only configure explicitly disables Metal, CUDA, Vulkan, and media so it
does not inspect any GPU SDK. The evaluator adds `--required-simd` when
`CMAKE_SYSTEM_PROCESSOR` is `arm64` or `aarch64`; its PNG fixture benchmark is
local-only and is not part of ARM CI.

## Capability contract

The engine and Tauri validator remain locked to capability schema v2. Vulkan is
appended as a fourth backend entry. This project does not promise compatibility
with an older consumer expecting exactly three entries; a separately versioned
schema v3 is deferred until independent consumer compatibility is required.

## CI failure history

Historical Vulkan package failures were in the SDK provisioning layer, not the
GetNative shader/compiler path: one Linux run resolved a relative config path
inside the action and got `Vulkan-Headers url=NOTFOUND`; Windows runs failed in
the external Vulkan-Loader MASM custom step. The current native Linux workflow
avoids that action and validates the distro toolchain directly.
