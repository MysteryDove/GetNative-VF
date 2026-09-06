# Vulkan Video validation compatibility

This isolated VVL overlay retains unwrapped `VkVideoDecodeInfoKHR` deep copies
for the lifetime of a command-buffer recording. It works around delayed parameter
reads observed with NVIDIA RTX 5080 / Linux driver 595.84. It does not disable
handle wrapping, core validation, synchronization validation, or filter VUIDs.
It changes the validation runtime, not GetNative's production GPU math.
The stock SDK is not replaced. Generated open-GOP sparse cases require the
decoder's closed-restart checks; this workaround does not waive DPB errors.
Bitstream-padding corruption is a separate FFmpeg issue, fixed in the project's
SDK scripts; see `docs/performance/vulkan-bitstream-padding.md`. A clean validation
log alone does not prove decoded-pixel correctness.

The patch is pinned to VVL `vulkan-sdk-1.4.357.0`, commit
`f4874eee15c78d7bdb2b7e60659d539f14741500`. It modifies that version's generated
dispatch file directly; do not regenerate the dispatch source or apply it to a
different revision. The upstream Apache-2.0 license continues to apply.

## Build

Requires Linux build prerequisites for VVL, Git, CMake, Ninja, Python 3, and a
Vulkan SDK with `bin/glslang` and `bin/spirv-opt`. VVL's pinned dependencies are
downloaded by its own `UPDATE_DEPS` workflow. No root installation is needed.

```sh
python3 tools/vulkan-validation/build_layer.py \
  --source /path/to/isolated/vvl-source \
  --build /path/to/isolated/vvl-build \
  --prefix /path/to/isolated/vvl-install \
  --sdk /path/to/vulkan-sdk/x86_64
```

Set `VK_LAYER_PATH` to the printed directory and
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` only for validation runs.
Use a dedicated `vk_layer_settings.txt` with `validate_core=true`,
`validate_sync=true`, and `unique_handles=true`, each prefixed with
`khronos_validation.`. Keep validation enabled during correctness testing, but
exclude its instrumentation cost from production performance comparisons.

The helper fixes the installed manifest's library path for the isolated prefix.
Confirm loading with `VK_LOADER_DEBUG=error,layer` and run `validation_canary.cpp`
before accepting clean logs: it must report `core_errors=1 sync_errors=1` for its
two intentional invalid operations. Give the canary a separate log directory.
An empty log without a successfully loaded layer is not validation evidence.

## Lifetime Bound

The layer owns the copies through `unique_ptr`, grouped by command buffer.
Successful begin/reset discards the previous recording. Freeing a command buffer
or resetting/destroying its command pool also releases the copies. Device
destruction releases residual bookkeeping. The cache is protected across threads;
it grows with recorded video commands, not the total frames processed across
command-buffer reuse. This does not make invalid reset/free of pending GPU work
legal; ordinary Vulkan synchronization rules still apply and remain checked.

This is a local compatibility workaround, not an upstream fix or a guarantee for
other GPUs/drivers. See `docs/performance/vulkan-validation-lifetime.md` for the
measured evidence and limitations.
