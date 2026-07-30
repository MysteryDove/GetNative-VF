# Windows CUDA / Vulkan Compute 后端开发 Handover

## 1. 文档目的

本文件用于把 GetNative VF 的 Windows GPU 后端工作交给另一名 agent。目标不是单独写一个可运行的 CUDA demo，而是在不改变现有 CPU 数学契约的前提下，交付两个可选、可诊断、可验证的生产后端：

1. Windows x64 + NVIDIA CUDA Driver API 后端。
2. Windows x64 + Vulkan Compute 后端。

CUDA 是第一优先级，Vulkan Compute 在共享数据契约稳定后实现。两个后端都必须复用同一份 CPU 生成的 `AxisPlan`，都以现有 Metal 后端为 GPU 算法和调度参考，并分别在真实 Windows GPU 上完成正确性、内存、取消、缺失运行时和性能验证。

原设计只把 CUDA 列为第二平台 GPU 后端，见 `.omx/plans/standalone-getnative.md:19-21` 和 `:220-236`。本 handover 根据新的任务要求把 Vulkan Compute 明确加入 Windows GPU 范围，但不扩大媒体解码、JSONL worker、导出或 GUI 分析功能的范围。

## 2. 当前基线和证据边界

编写本 handover 时的工作区状态：

- 主目录：仓库根目录（下文记为 `<repo>`）。
- 日期：`2026-07-30`。
- 主仓库已经 `git init`，但还没有首个 commit；全部现有文件均为未跟踪文件。
- 因为没有 `HEAD`，不能把 `git diff` 或 `git reset` 当作基线或恢复手段。Windows agent 开工前必须记录收到的完整文件清单和哈希，并保留传输原件。
- `upstream/descale` 是独立、干净的 Git checkout：
  - 远端：`https://github.com/Irrational-Encoding-Wizardry/descale.git`
  - 固定 revision：`8c53f5d1297dee286e5a854ae5731103614a0583`
  - commit：`Add "repeat" mode for border handling`
- `upstream/zimg` 固定 revision：`1ad1895d5ff0bbe69c61243f9996aede713d1b5f`。
- 当前机器是 Apple M4 Max，没有 `nvcc` 和真实 NVIDIA/Vulkan Windows 运行证据。任何 CUDA/Vulkan 的“可用”“正确”“更快”结论都必须由 Windows 目标机重新证明。

当前实现边界：

- CPU planner、CPU Float32 执行、全部 filter family、单轴/双轴分析、p-norm 和批处理已实现。
- Metal 已实现 H、V、H+V 三种轴模式、p=1、B3/B7/generic kernel、混合形状 tile、双轴 forward-order、融合 metric 和固定顺序 CPU merge。
- CUDA 只有 capability 占位，`compiled=false`，见 `engine/src/cli/main.cpp:130-133`。
- Vulkan 尚未出现在源码、capability 或既有架构文档中。
- `analyze` CLI/JSONL 命令仍未实现，见 `engine/src/cli/main.cpp:85-87` 和 `README.md:31-35`。因此 GPU library 和 conformance benchmark 可以先完成，但 GUI 端到端分析不是本次 GPU 后端自身的完成证据。

## 3. 已确定的实现决策

以下决策不留给实现者重新解释：

1. CPU 永远是系数生成者、严格参考和显式 fallback。GPU 不生成 filter taps，不构造 `A`，不构造 `A^T A`，不做 Float64 LDLT。
2. CUDA 使用 Driver API，并在运行时动态加载 `nvcuda.dll`。产品可执行文件不能静态依赖 `nvcuda.dll`、CUDA Runtime 或 `cudart64_*.dll`。
3. Vulkan 使用 `VK_NO_PROTOTYPES` 和运行时动态加载 `vulkan-1.dll`。产品可执行文件不能在 PE import table 中硬依赖 `vulkan-1.dll`。
4. CUDA kernel 以离线 fatbin 打包，并包含由 Windows CI/toolkit 确定的 SASS 架构集合和一个 PTX fallback。不要在没有目标 GPU/CI 矩阵时硬编码 SM 列表。
5. Vulkan shader 离线编译为 SPIR-V 并嵌入可执行文件。运行时不调用 shader compiler，不依赖 Vulkan SDK。
6. 第一版 GPU capability 与现有 Metal 对齐：H、V、H+V，p=1，`max_half_bandwidth=15`，`max_forward_width=16`。
7. 第一版只做 strict 模式。fast 模式使用独立编译产物和独立 provenance，不能通过运行时开关把 strict artifact 变成 fast artifact。
8. 后端失败不能静默切到 CPU，strict 不能静默切到 fast。错误或显式 fallback 必须进入结果 provenance 和 GUI 状态。
9. `compiled`、`device_available`、`analysis_command_available` 是三个不同事实，不能互相代替。
10. 默认后端仍是 CPU，直到某个 GPU 后端在同一 Windows 主机、同一输入和同一候选集上通过正确性、内存和至少 3x 性能门槛。
11. 不在 Windows-only 工作中重构 Metal Objective-C++ 文件。可以新增可移植 GPU packing helper，但 Metal 迁移必须留到有 Apple 主机验证时进行。

## 4. 必读代码顺序

实现前按下列顺序阅读，不要只看 Metal shader：

| 顺序 | 文件和行 | 需要理解的内容 |
| --- | --- | --- |
| 1 | `docs/architecture.md:211-262` | 独立数值流水线、GPU batch、CUDA Driver API 目标 |
| 2 | `docs/architecture.md:304-339` | 数值等价、性能、内存和取消门槛 |
| 3 | `engine/include/getnative/axis_plan.hpp:20-84` | 唯一允许上传到 GPU 的 `AxisPlan` 语义 |
| 4 | `engine/src/planner/axis_plan.cpp:134-452` | `A`、forward weights、`A^T A`、LDLT 和 Float32 packing |
| 5 | `engine/src/planner/axis_plan.cpp:541-629` | CPU inverse/forward 的精确累加顺序和 specialization |
| 6 | `engine/include/getnative/cpu_analysis.hpp:14-111` | image、metric、candidate、result 和 batch API |
| 7 | `engine/src/backend/cpu/cpu_analysis.cpp:244-380` | 双轴和单轴融合分析的参考语义 |
| 8 | `engine/src/backend/metal/getnative.metal:1-441` | GPU kernel 的完整算法表面 |
| 9 | `engine/src/backend/metal/metal_backend.mm:20-190` | GPU POD、packing、shape、workspace 约束 |
| 10 | `engine/src/backend/metal/metal_backend.mm:192-377` | shape dispatch、plan flatten、单轴/双轴 workspace |
| 11 | `engine/src/backend/metal/metal_backend.mm:512-953` | tile、上传、dispatch、取消、partial merge |
| 12 | `engine/tests/metal_conformance_test.cpp:57-99` | GPU/CPU tolerance 和 valley 规则 |
| 13 | `engine/tests/metal_conformance_test.cpp:137-441` | filter、shape、轴、取消、内存和稳定顺序测试矩阵 |
| 14 | `engine/bench/metal_benchmark.cpp:103-203` | 基准输入、报告字段和默认启用门槛 |
| 15 | `engine/tests/upstream_conformance_test.cpp:43-194` | descale/zimg 逐 bit 系数与执行参考 |
| 16 | `engine/CMakeLists.txt:1-133` | 可选后端、embedded binary、strict FP、CLI link 模式 |
| 17 | `engine/src/cli/main.cpp:85-145` | capability schema v2 和当前 CUDA 占位 |
| 18 | `app/src-tauri/src/lib.rs:140-256` | 当前 Rust capability validator 的硬编码限制 |

## 5. 不可改变的数学和数据契约

### 5.1 数学定义

每个轴的 forward resize 矩阵为 `A`。给定观测向量 `b`，descale 求解：

```text
x = (A^T A)^-1 A^T b
```

CPU planner 的职责：

1. 用 Float64 构造 descale 方向的稀疏 `A`。
2. 构造 zimg-compatible forward `A`，并保留边界 residual-carry 后的 Float32 taps。
3. 直接在 banded storage 中构造 `A^T A`。
4. 用 Float64 进行 LDLT。
5. 按参考顺序投影为 Float32 `transpose_weights`、`lower_ld`、`upper_l`、`inverse_diagonal` 和 `forward_weights`。

GPU 的职责：

1. 计算 `A^T b`。
2. 按序执行 `LD y = A^T b`。
3. 按序执行 `L^T x = y`。
4. 用已经存储的 forward `A` 重建。
5. 计算 strict threshold、crop、p=1 partial reduction。

GPU 不得重新计算 `Filter::weight`，不得独立实现边界 mapping，也不得把 `AxisPlan` 换成另一套系数格式后再宣称与 CPU 同契约。

### 5.2 `AxisPlan` 布局

`engine/include/getnative/axis_plan.hpp:29-54` 定义：

- `forward_offsets/indices/weights`：重建矩阵 `A` 的连续 taps。
- `transpose_offsets/indices/weights`：按 native 输出 index 组织的稀疏 `A^T`。
- `lower_ld[(distance-1) * destination_size + i]`：`L(i, i-distance) * D(i-distance)`。
- `upper_l[(distance-1) * destination_size + i]`：`L(i+distance, i)`。
- `inverse_diagonal[i]`：`1 / D(i)`。

所有 GPU 索引先在 host 侧检查后收窄为 `uint32_t`。保持 Metal 的两个 POD 大小：

```cpp
struct alignas(16) GpuAxisPlanDescriptor; // exactly 64 bytes
struct GpuAnalysisJob;                    // exactly 40 bytes
```

字段顺序直接参考 `engine/src/backend/metal/metal_backend.mm:33-74`，不要为 CUDA 和 Vulkan 各定义一套顺序。host、CUDA 和 Vulkan shader 都必须对每个字段的 offset 写静态断言或 CPU-only layout test。

### 5.3 filter shape 映射

`descale` 的 `bandwidth` 是完整奇数带宽；GetNative 的 `half_bandwidth` 是单侧带宽。两者不能混用。

| Filter | support | descale `bandwidth=4*support-1` | GetNative `half_bandwidth=2*support-1` | `forward_width=2*support` | 第一版 GPU 路径 |
| --- | ---: | ---: | ---: | ---: | --- |
| Bilinear | 1 | 3 | 1 | 2 | B3 specialized |
| Bicubic | 2 | 7 | 3 | 4 | B7 specialized |
| Spline16 | 2 | 7 | 3 | 4 | B7 specialized |
| Spline36 / Lanczos3 | 3 | 11 | 5 | 6 | generic |
| Spline64 / Lanczos4 | 4 | 15 | 7 | 8 | generic |
| Lanczos5 | 5 | 19 | 9 | 10 | generic |
| Lanczos6 | 6 | 23 | 11 | 12 | generic |
| Lanczos7 | 7 | 27 | 13 | 14 | generic |
| Lanczos8 | 8 | 31 | 15 | 16 | generic |

CPU core 支持到 Lanczos15，即 `half_bandwidth=29`、`forward_width=30`，见 `engine/src/cli/main.cpp:98-102`。CUDA/Vulkan 第一版只需与 Metal 的 15/16 上限一致；扩大到 29/30 是通过 profile 和工作集测试后的后续功能，不能提前广告。

### 5.4 严格累加顺序

以下顺序会影响 threshold 附近的像素，不能“数学等价”地重排：

- `A^T b`：按 `transpose_offsets[i]` 到 `transpose_offsets[i+1]` 递增。
- forward solve：distance 从可用最远项递减到 1，见 `axis_plan.cpp:555-560`。
- backward solve：
  - B7，即 `half_bandwidth==3`，distance 从 1 递增到 3，见 `axis_plan.cpp:567-572`。
  - B3 和 generic，distance 从最远项递减到 1，见 `axis_plan.cpp:573-577`。
- forward reconstruction：tap 从 0 递增到 `forward_width-1`，见 `axis_plan.cpp:589-597`。
- strict threshold：只有 `difference > threshold` 才贡献，等于 threshold 不贡献，见 `cpu_analysis.cpp:70-88` 和 `core_test.cpp:291-325`。
- GPU workgroup 内先按固定二叉树规约为 Float32 partial。
- host 按 candidate，再按 group index 递增，把 Float32 partial 转成 double 后合并，见 `metal_backend.mm:945-951`。

strict CUDA 编译禁止 `--use_fast_math`，首版使用 `--fmad=false --prec-div=true --prec-sqrt=true`。strict Vulkan shader 对关键运算使用 `precise`/SPIR-V `NoContraction`，首版不依赖 subgroup reduction。最终 strict 定义仍由真实设备 conformance 结果决定，而不是仅由编译 flag 决定。

## 6. `descale` 参考仓库的使用边界

### 6.1 可以作为 CUDA/Vulkan 算法参考的部分

优先参考标量 C 路径，因为它最清楚地表达累加顺序：

| 上游位置 | 用途 |
| --- | --- |
| `upstream/descale/src/descale.c:288-319` | B3 horizontal 的 `A^T b + LD + L^T` |
| `upstream/descale/src/descale.c:322-369` | B7 horizontal，尤其 backward 的 near-to-far 顺序 |
| `upstream/descale/src/descale.c:372-409` | generic horizontal |
| `upstream/descale/src/descale.c:412-442` | B3 vertical |
| `upstream/descale/src/descale.c:445-491` | B7 vertical |
| `upstream/descale/src/descale.c:494-532` | generic vertical |
| `upstream/descale/src/descale.c:535-559` | B3/B7/generic dispatch 条件 |

AVX2 路径只用于优化思路，不是 strict 数值 oracle：

- `src/x86/descale_avx2.c:99-211` 展示 B3 怎样保留最近结果、减少 load/store。
- `src/x86/descale_avx2.c:214-358` 展示 B7 怎样保留三项依赖并展开求解。
- `src/x86/descale_avx2.c:361-467` 说明 generic 路径因任意带宽需要更多 load/store。
- `src/x86/descale_avx2.c:553-744` 说明 vertical 方向在内存访问和寄存器保留之间的取舍。

适合迁移到 CUDA 的思想：

1. 一个线程处理一个 candidate 的一条独立 row/column vector。
2. B3 只保留一个前向依赖，B7 保留三个前向依赖。
3. B3/B7 用 compile-time specialization 展开 factor load。
4. generic 直接从 SoA factor bands 读取，先保证正确再做 shared-memory/register tuning。
5. 横向和纵向不要复制两套数学代码，用 direction、stride 和 index helper 统一。

### 6.2 不应搬到 GPU 的部分

以下代码继续只作为 CPU planner 或 test oracle：

- `descale.c:37-142`：dense/banded helper、LDLT 和 factor compression。
- `descale.c:166-285`：filter weight、round-half-up、border mapping、dense scaling weights。
- `descale.c:562-647`：`create_core`、dense `A^T A` 和参考 packing。
- `descale.c:650-685`：reference core lifecycle 和 CPU/AVX2 API dispatch。
- `src/vsplugin.c`、`src/avsplugin.c`、`src/plugin.h`：VapourSynth/AviSynth glue，产品运行时禁止依赖。
- `DESCALE_MODE_CUSTOM`：当前 GetNative filter contract 不支持 custom callback，GPU 端不要新增。

GetNative 已在 `engine/src/planner/axis_plan.cpp:134-452` 独立实现更适合共享后端的 planner，并由 `engine/tests/upstream_conformance_test.cpp:43-128` 对 descale 逐 bit 验证。不要为了“更接近上游”退回 dense allocation 或直接把 `DescaleCore` 当 GPU ABI。

### 6.3 许可证和 Windows 测试注意事项

`descale` 是 MIT，copyright 和许可文本已进入 `THIRD_PARTY_NOTICES.md:8-32`。允许改写其数学核心，但复制 substantial portions 时必须保留 notice。产品 runtime 不链接 `descale`，只有 `GETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON` 的测试 target 链接 `src/descale.c`。

不要改动 `upstream/descale` checkout 来适配 MSVC。如果 MSVC C 模式拒绝上游的 `restrict`，仅在 test-only target 上定义 `restrict=__restrict`，或用 clang-cl/MinGW 运行上游 conformance；产品 core 必须继续用标准 GetNative 实现。

## 7. 建议的文件和模块边界

新增文件建议：

```text
engine/include/getnative/gpu_batch.hpp
engine/src/backend/gpu/gpu_batch.cpp
engine/tests/gpu_batch_test.cpp

engine/include/getnative/cuda_analysis.hpp
engine/src/backend/cuda/cuda_backend.cpp
engine/src/backend/cuda/getnative_cuda.cu
engine/tests/cuda_conformance_test.cpp
engine/bench/cuda_benchmark.cpp

engine/include/getnative/vulkan_analysis.hpp
engine/src/backend/vulkan/vulkan_backend.cpp
engine/src/backend/vulkan/shaders/inverse_axis.comp
engine/src/backend/vulkan/shaders/inverse_axis_matrix.comp
engine/src/backend/vulkan/shaders/forward_axis_matrix.comp
engine/src/backend/vulkan/shaders/metric_axis_p1.comp
engine/tests/vulkan_conformance_test.cpp
engine/bench/vulkan_benchmark.cpp
```

`gpu_batch` 只放纯 C++、无 GPU SDK 依赖的内容：

- `GpuAxisPlanDescriptor`、`GpuAnalysisJob`、`GpuKernelShape`。
- candidate validation 和 B3/B7/generic shape 分类。
- `AxisPlan` flatten/packing。
- single-axis/two-axis workspace 计算。
- 按 axes、horizontal shape、vertical shape、forward order 分 tile。
- 32-bit offset、乘法、加法和 workspace overflow 检查。
- Float32 partial 按固定顺序合并为 double metric。

Windows agent 可以从 `metal_backend.mm:20-377` 提取等价逻辑，但不要在没有 Mac 测试的情况下删改 Metal 内部版本。先让 CUDA/Vulkan 共用新 helper，并用 CPU-only `gpu_batch_test` 锁定描述符内容、workspace bases、candidate order 和异常行为。后续由 Apple lane 再把 Metal 切到公共 helper。

公开 API 与 Metal 保持同形：

```cpp
bool cuda_backend_available() noexcept;
class CudaAnalysisEngine {
public:
    const CudaDeviceInfo &device_info() const noexcept;
    const CudaAnalysisOptions &options() const noexcept;
    std::size_t peak_workspace_elements() const noexcept;
    std::size_t peak_working_set_bytes() const noexcept;
    std::vector<CandidateResult> analyze_axis_batch_f32(
        ConstImageView source,
        std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric,
        std::stop_token stop = {});
};
```

Vulkan 提供相同方法和同样的失败语义。options 至少包含 device ordinal/UUID、tile size、reduction groups、workspace limit 和 inverse workgroup size。一次 engine call 可以串行化；首版不需要并发复用同一个 context/device object。

## 8. 必须完成的 operator / kernel 清单

下表同时适用于 CUDA 和 Vulkan。CUDA 使用 `__global__` + template specialization；Vulkan 使用四个 compute shader 和 specialization constants 创建 B3/B7/generic pipeline。

| 优先级 | Kernel / operator | 输入到输出 | Dispatch | 参考 |
| --- | --- | --- | --- | --- |
| P0 | host plan validation/packing | `AxisPlan` -> flat tile buffers | CPU | `metal_backend.mm:192-377` |
| P0 | host deterministic partial merge | Float32 partials -> double metric | CPU | `metal_backend.mm:945-951` |
| P1 | `inverse_axis_generic` | source image -> single-axis native workspace | 1 thread / candidate-vector | `getnative.metal:47-121,157-172` |
| P1 | `metric_axis_p1_generic` | native workspace -> Float32 partials | 1 block/workgroup / partial, 256 threads | `getnative.metal:305-376,408-421` |
| P1 | single-axis H/V scheduler | upload once, inverse, metric, read partials | tile | `metal_backend.mm:835-855` |
| P2 | `inverse_axis_b7` | B7 specialized inverse | 1 thread / candidate-vector | `getnative.metal:140-155` |
| P2 | `metric_axis_p1_b7` | four-tap forward + p1 reduction | 256 threads | `getnative.metal:393-406` |
| P2 | `inverse_axis_b3` | B3 specialized inverse | 1 thread / candidate-vector | `getnative.metal:123-138` |
| P2 | `metric_axis_p1_b3` | two-tap forward + p1 reduction | 256 threads | `getnative.metal:378-391` |
| P3 | `inverse_axis_matrix_*` | inverse-H intermediate -> 2D native | 1 thread / candidate-vector | `getnative.metal:174-254` |
| P3 | `forward_axis_matrix_*` | 2D native -> first forward intermediate | 1 thread / candidate-vector | `getnative.metal:256-303` |
| P3 | `metric_axis_p1_horizontal_first_*` | horizontal-first intermediate -> partials | 256 threads | `getnative.metal:423-441` |
| P3 | two-axis scheduler | inverse H, inverse V, first forward, fused final metric | tile | `metal_backend.mm:856-923` |
| P4 | p2/p3/p4 metric variants | Float32 moment + host root | 256 threads | `cpu_analysis.cpp:70-95` |
| P5 | general p metric | `pow(diff,p)` + host root | 256 threads | CPU only until profiled |

P1 的 generic kernel 必须能处理 B3/B7 plan，因此先建立全 filter correctness。P2 specialization 只负责性能，不能改变结果顺序。P3 完成后才允许 capability 广告 `axes=[horizontal,vertical,both]`；在此之前必须准确广告实际支持的 axes。

### 8.1 Kernel launch 规则

Inverse kernel：

```text
linear_gid = candidate * maximum_vector_count + vector
candidate = linear_gid / maximum_vector_count
vector    = linear_gid % maximum_vector_count
```

每个线程沿 destination axis 串行求解。不要尝试把同一条 triangular solve 拆给多个线程作为 MVP；依赖链会引入同步和不同累加顺序。并行度来自 candidate 数和 row/column vector 数。

Metric kernel：

- 固定 local/block size 256，设备不支持时 backend 不可用并返回 reason。
- 一个 block/workgroup 生成一个 partial。
- pixel 遍历和二叉规约顺序与 `getnative.metal:321-375` 一致。
- 不写回完整 reconstructed frame。
- partial buffer 按 `[candidate][group]` 连续排列。

## 9. CUDA 后端实施要求

### 9.1 Build 和打包

在 `engine/CMakeLists.txt` 新增：

```cmake
option(GETNATIVE_ENABLE_CUDA "Build the optional CUDA analysis backend" OFF)
set(GETNATIVE_CUDA_ARCHITECTURES "" CACHE STRING
    "Semicolon-separated SASS architectures; CI must set this when CUDA is enabled")
set(GETNATIVE_CUDA_PTX_ARCHITECTURE "" CACHE STRING
    "PTX fallback compute architecture; CI must set this when CUDA is enabled")
```

当 `GETNATIVE_ENABLE_CUDA=ON`：

1. 构建时要求 CUDA Toolkit 和 `nvcc`，缺失时 configure 失败，不要悄悄生成假 CUDA build。
2. 只用 `nvcc` 生成 strict fatbin，不链接 `CUDA::cudart` 或 `CUDA::cuda_driver`。
3. 复用 `engine/cmake/embed_binary.cmake` 把 fatbin 转为 C++ header。
4. 生成 `getnative_cuda` static library，链接 `getnative_core`。
5. 给 `getnative-engine` 定义 `GETNATIVE_HAS_CUDA=1`。
6. strict fatbin 不启用 fast math；fast fatbin 是后续独立目标。
7. 不把 CUDA Toolkit DLL 或 compiler 组件复制进产品包；fatbin 的生成和分发条件进入 release license inventory。

fatbin 至少包含：

- CI 明确指定的 SASS `sm_*` 列表。
- 一个兼容目标 GPU/driver 的 `compute_*` PTX fallback。
- build log 中打印完整 `nvcc` version、SASS 列表和 PTX target。

不得凭当前日期猜测架构列表。实现者必须根据实际 CUDA Toolkit 支持范围、Windows CI GPU 和最低驱动策略填写 cache 变量，并把选择写回结果报告。

### 9.2 Driver API 动态加载

用 `LoadLibraryW(L"nvcuda.dll")` 和 `GetProcAddress` 建立最小函数表。至少需要覆盖：

- 初始化和设备：`cuInit`、`cuDriverGetVersion`、`cuDeviceGetCount`、`cuDeviceGet`、`cuDeviceGetName`、`cuDeviceGetAttribute`。
- context：优先 primary context retain/release；或明确管理 `cuCtxCreate_v2`/`cuCtxDestroy_v2`。
- module/kernel：`cuModuleLoadDataEx`、`cuModuleUnload`、`cuModuleGetFunction`。
- stream/event：`cuStreamCreate`、`cuStreamDestroy_v2`、`cuStreamSynchronize`、`cuStreamQuery`、必要的 event 计时 API。
- memory/copy：`cuMemAlloc_v2`、`cuMemFree_v2`、`cuMemcpyHtoDAsync_v2`、`cuMemcpyDtoHAsync_v2`、`cuMemGetInfo_v2`。
- launch/error：`cuLaunchKernel`、`cuGetErrorName`、`cuGetErrorString`。

所有符号解析失败都转成 `device_available=false` + 可诊断 reason。`cuda_backend_available() noexcept` 必须捕获所有异常。没有 `nvcuda.dll`、没有设备、driver 太旧、PTX 不兼容、fatbin 无匹配 image 是不同错误，不要都压成 `CUDA unavailable`。

### 9.3 内存和提交

- source GRAY F32 每个 job 上传一次。
- workspace、partials 和 tile plan buffer 按最大 tile 容量复用。
- 每个 tile 只上传该 tile 的 flattened plans。
- 首版同一个 stream 串行提交 kernel，利用 stream ordering 保证阶段依赖。
- 只把 partials 和必要的错误/计时数据传回 host。
- 每次分配都计入 `peak_working_set_bytes`；同时报告 device bytes 和 host staging bytes。
- 首版可用 pageable host memory；优化阶段再引入 pinned staging 和双缓冲，必须用 benchmark 证明收益。
- 遇到 stop request 后立即停止提交新 tile；已提交 kernel 不强制 unsafe preemption。返回 cancelled，不返回部分成功结果。

## 10. Vulkan Compute 后端实施要求

### 10.1 Build 和 shader 打包

新增：

```cmake
option(GETNATIVE_ENABLE_VULKAN "Build the optional Vulkan Compute analysis backend" OFF)
```

当选项为 ON：

1. 构建时要求 Vulkan headers 和 `glslc` 或等价离线 compiler。
2. shader target 固定为 Windows agent 验证过的 Vulkan/SPIR-V 版本；建议 MVP 以 Vulkan 1.2 为最低 strict capability。
3. strict shader 初始以优化关闭/保守设置编译，并对关键 Float32 表达式使用 `precise`。
4. 四个 SPIR-V 模块通过 `embed_binary.cmake` 嵌入 host library。
5. 不链接 `Vulkan::Vulkan`，只使用其 include path。运行时由自有 loader 解析函数。
6. Vulkan headers、shader compiler 和 SPIR-V 生成物的许可证/分发条件进入 release license inventory；产品包不携带 Vulkan SDK。

四个 shader 使用 specialization constants：

- inverse：`fixed_half_bandwidth` 和 `bandwidth7_order`。
- forward：`fixed_forward_width`。
- metric：`fixed_forward_width` 和 `horizontal_first_two_axis`。

由此创建 B3、B7 和 generic pipeline，不复制十五份 shader 源码。

### 10.2 Loader 和设备门槛

编译时定义 `VK_NO_PROTOTYPES`。运行时：

1. `LoadLibraryW(L"vulkan-1.dll")`。
2. 先解析 `vkGetInstanceProcAddr`。
3. 再按 instance/device 层级解析所需函数。
4. 枚举 physical devices 和 compute queue family。
5. 用 PCI vendor/device、UUID 和 device name 建立稳定 device info；不要只保存数组 ordinal。

MVP device 至少满足：

- Vulkan 1.2 或 handover 后续明确降低的版本。
- 可用 compute queue。
- `maxComputeWorkGroupInvocations >= 256` 且 X 维 local size 支持 256。
- 每个 compute stage 至少支持 9 个 storage buffer binding，或实现者先完成经过测试的合并 buffer 方案。
- 最大 storage buffer range、allocation count 和 heap budget 能容纳一个 candidate tile。

不要求 subgroup、descriptor indexing、buffer device address、timeline semaphore 或 shader Int64。首版依赖这些特性会无谓缩小支持面。

### 10.3 Descriptor、同步和内存

为减少与 Metal/CUDA 的数据分歧，MVP 可直接保持独立 buffers：source、descriptors、transpose offsets/indices/weights、lower、upper、diagonal、forward left/weights、workspace、partials。`GpuAnalysisJob` 使用 40-byte push constants；`GpuAxisPlanDescriptor[]` 使用 std430 storage buffer，并验证 64-byte stride。

每个 tile 的 command buffer 阶段：

```text
host/staging -> device plan buffers
inverse image
compute barrier: workspace write -> read/write
[inverse matrix]
compute barrier
[forward matrix]
compute barrier
metric/reduction
compute -> transfer barrier
partials device -> host staging
fence completion
CPU double merge
```

优先使用 device-local buffers + host-visible staging。只有在 UMA 设备上并经测量后，才允许直接使用 host-visible compute buffers。记录 heap type、是否 UMA、每个 allocation 的字节数和 peak。

Vulkan 取消同样发生在 tile 边界。`VK_ERROR_DEVICE_LOST`、pipeline creation failure、loader missing 和 no compatible queue/device 分别报告。销毁顺序必须在 device/context 生命周期内完成，异常路径也不能泄露对象。

## 11. Capability、CLI 和 Tauri 集成

### 11.1 Engine capability

保持 schema v2 的字段形状，但把 backend validator 改为按 `id` 查找，不能继续依赖数组 index。Windows 完整 build 的 backend ids 为：

```text
cpu, metal, cuda, vulkan
```

语义：

- `compiled=true`：对应 host adapter 和 embedded kernel/shader 已进入 binary。
- `device_available=true`：动态运行时加载成功、至少一个兼容设备可创建 context/device，并且 module/pipeline 可初始化。
- `analysis_command_available=true`：CLI/worker 的真实 analyze 路径已经接到该 backend。只完成 library 时仍为 false。
- backend 编译但设备不可用时，仍报告其静态 `axes`、`p_norms` 和 shape 上限，并附 reason，行为与 Metal 一致。
- backend 未编译时，`axes=[]`、`p_norms=null`、shape 为 null、reason 为 `not compiled`。

CUDA/Vulkan 每个 device 记录 name、稳定 id/UUID、driver/API version、可用 memory 和 strict/fast 支持状态。不要把“机器装了 Vulkan loader”当成存在可用 compute device。

### 11.2 需要修改的现有代码

- `engine/src/cli/main.cpp:3-5`：条件 include CUDA/Vulkan headers。
- `engine/src/cli/main.cpp:85-145`：真实枚举和 reason；增加 Vulkan entry。
- `engine/CMakeLists.txt:121-127`：根据 target 组合链接多个可选 backend，而不是 Metal/CMake 二选一。
- `engine/CMakeLists.txt:149-152`：更新 capability test，不再强制 CUDA false。
- `app/src-tauri/src/lib.rs:184-240`：按 backend id 验证，接受真实 CUDA 和 Vulkan shape。
- `app/src-tauri/src/lib.rs:372-478`：新增 compiled/no-driver/invalid-shape/Vulkan tests。
- `app/package.json:8-9`：当前打包脚本没有传 GPU CMake options；增加 Windows GPU preset 或明确的环境参数转发，同时保留 CPU-only preset。
- `app/src/App.tsx:326-354`：现有通用 backend row 可复用；确认 Vulkan 长 reason 不溢出。

不要在 GPU library 完成前把 `commands.analyze` 或 `analysis_command_available` 改为 true。当前 Rust 测试故意拒绝虚假的 analyze/CUDA 声明，见 `lib.rs:467-474`；应把它改成拒绝不一致的声明，而不是删除验证。

### 11.3 Fallback 和选择

- 用户显式选 CUDA/Vulkan 时，初始化或执行失败默认返回错误，不自动改跑 CPU。
- 如果未来增加 `auto` backend，它必须在 job 创建时解析成一个具体 backend 并写入 provenance。
- GPU 失败后的“Retry on CPU”是显式新 job，保留原失败记录。
- 在 CUDA 和 Vulkan 都可用的 NVIDIA 机器上，不能因为 CUDA 通常更快就提前隐藏 Vulkan；两个 backend 分别可选、分别测量。

## 12. 测试和验证矩阵

### 12.1 CPU-only、任何机器都必须通过

新增 `gpu_batch_test`，覆盖：

- `GpuAxisPlanDescriptor` size、alignment、field offsets。
- `GpuAnalysisJob` size 和 field offsets。
- B3/B7/generic shape 分类。
- H、V、both 的 workspace bases 和最大 vector count。
- mixed shape 保持输入顺序并按相邻兼容 signature 分 tile。
- 负数/越界 index、非连续 forward row、null plan、overflow、过小 workspace 全部拒绝。
- partial merge 的 group order、candidate id 和结果顺序。
- CUDA/Vulkan disabled 时 CPU-only configure/build/test 不需要任何 GPU SDK。

### 12.2 每个真实 GPU 后端的 conformance

复制 `metal_conformance_test.cpp` 的测试形状，但不要复制 Objective-C：

1. Vertical 和 horizontal bicubic batch。
2. Bilinear、任意 `(b,c)` Bicubic、Spline16/36/64、Lanczos1-8。
3. 所有 `half_bandwidth=1..15` 对应的实际支持 shape。
4. H、V、both。
5. both 的 horizontal-first 和 vertical-first。
6. mixed B3/B7/generic 和 interleaved forward order，结果 id/order 不变。
7. 非连续 source stride。
8. 空 candidates。
9. stop 已请求、tile 间 stop、完成后的稳定清理。
10. 非 finite/负 threshold、无效 crop、p!=1、null plan、超过 shape 上限。
11. 单 candidate 超 workspace limit 和自适应 tile 缩小。
12. peak device/host/total working set 统计。
13. 连续重复运行，检查 module/pipeline/context 复用和无增长泄漏。
14. 多设备枚举和显式 device selection。
15. CUDA：无 `nvcuda.dll`、无设备、PTX/fatbin 不兼容的诊断测试。
16. Vulkan：无 `vulkan-1.dll`、无 compute queue、不足 descriptor/workgroup limit、device lost 的诊断测试。

### 12.3 数值门槛

对每个 candidate：

```text
abs(gpu_metric - cpu_metric)
    <= max(1e-7, 5e-4 * abs(cpu_metric))
```

另外必须：

- 所有 metric finite。
- argmin 与 CPU 的 candidate index 距离不超过一个 search step。
- top-k local valley 集保持一致，按 `docs/architecture.md:311-317` 执行。
- candidate 数、id 和输入顺序完全一致。

不得修改 CPU oracle、放宽 threshold 规则或只比较最终 valley 来绕过逐 candidate 误差。

### 12.4 upstream conformance

`GETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON` 继续证明：

- `AxisPlan` factor、transpose weights 和 inverse output 与 descale `DESCALE_OPT_NONE` 逐 bit 相同。
- forward offsets/weights 与 pinned zimg 逐 bit 相同。

这证明 planner，不证明 CUDA/Vulkan runtime。反过来，shader 编译成功也不证明真实 GPU 正确。

### 12.5 基准和默认启用门槛

固定主基准：

- GRAY F32 `1920x1080`。
- Bicubic `(b=0,c=0.5)`。
- vertical height analysis。
- `native_height=720`。
- 1000 个 fractional candidates。
- crop 5、threshold `0.015`、p=1。
- 同一进程 warmup 后至少 5 次 measured runs，报告 median 和每次值。

每个 backend 报告：

- CPU ms、GPU total ms、speedup。
- upload、plan pack/upload、inverse、forward、metric、readback、CPU merge 分段时间。
- maximum metric error、valley distance。
- tile size、reduction groups、workgroup/block size。
- peak workspace、device allocations、host staging 和 total explicit working set。
- GPU name、UUID/PCI id、driver、CUDA/Vulkan version、compiler 和 build flags。

门槛：

- total explicit GPU working set `< 2 GiB`。
- strict tolerance pass。
- valley distance `<= 1`。
- cancellation 在两次 tile duration 内结束。
- 同机同输入相对优化 CPU `>= 3x` 才能考虑默认启用。

必须另外跑 generic-heavy cases，例如 Lanczos8、双轴 H+V 和 mixed shapes。主 B7 benchmark 通过不代表 generic 路径已经优化。

## 13. 优化顺序

只有前一层 conformance 通过后再进行下一层：

1. B7 compile-time unroll，目标主 bicubic workload。
2. B3 compile-time unroll。
3. plan/workspace capacity reuse，避免每 tile allocation。
4. source upload once 和 partial-only readback。
5. CUDA pinned staging + async copy。
6. CUDA 双 buffer/多 stream；只有 timeline 证明 overlap 后保留。
7. Vulkan device-local plan arena + staging reuse。
8. Vulkan pipeline cache；cache key 必须包含 shader hash、driver/device identity。
9. generic factor band load、register pressure 和 occupancy tuning。
10. reduction group count、tile size、inverse threads 的设备 profile。
11. p2/p3/p4 specialized metric。
12. 独立 fast artifact：CUDA fast math / Vulkan relaxed precision，带明确 provenance。

不要把同一 triangular vector 拆成并行 scan 作为首轮优化。只有 B3/B7/generic 的串行-per-vector 模型在真实 GPU 上无法达到门槛，并且已有 profile 证明 solve dependency 是主瓶颈时，才评估 parallel cyclic reduction 或其他 solver；任何替换都必须重新过完整数值和 valley 矩阵。

## 14. 里程碑和提交边界

### M0: Windows CPU 基线和共享 GPU packing

交付：

- MSVC 或 clang-cl C++23 CPU build/test 通过。
- `gpu_batch` 和 CPU-only tests。
- CUDA/Vulkan option 默认 OFF 时无 SDK 依赖。
- 记录工作区文件哈希，避免无 `HEAD` 基线丢失。

完成标准：任何机器都能在无 GPU SDK/driver 情况下 build 和启动 engine。

### M1: CUDA vertical generic strict MVP

交付：

- Driver loader、device enumeration、context/module/memory/stream RAII。
- generic inverse + generic p1 metric。
- vertical single-axis batch、fixed partial merge、取消。
- `cuda_conformance_test` 的 vertical filter matrix。
- CUDA benchmark 初版。

完成标准：真实 NVIDIA GPU 上逐 candidate tolerance 通过；无 NVIDIA driver 的 Windows VM 中 engine 仍能启动并报告 reason。

### M2: CUDA 完整 strict surface

交付：

- B3/B7 specialization。
- horizontal、both、forward-order、matrix kernels。
- mixed shape、memory、cancel、multi-device tests。
- fatbin SASS/PTX 和 packaging dependency scan。

完成标准：与 Metal 当前功能范围等价，除非 capability 明确收窄；完整 CUDA conformance 通过。

### M3: Vulkan vertical generic strict MVP

交付：

- Vulkan dynamic loader、device/queue selection、RAII。
- SPIR-V embedding、descriptor/push constants、barriers、staging。
- generic inverse + p1 metric。
- vertical conformance 和 no-loader/no-device smoke。

完成标准：至少一台真实 Windows Vulkan GPU 通过逐 candidate tolerance；engine 无 Vulkan runtime 仍可启动。

### M4: Vulkan 完整 strict surface

交付：

- B3/B7 pipelines。
- H、V、both 和全部 matrix/fused metric 路径。
- mixed shape、memory、cancel、device-lost、multi-device tests。

完成标准：完整 Vulkan conformance 通过，capability 与实际功能一致。

### M5: Capability、CLI/Tauri 和 Windows packaging

交付：

- capability 按 id 验证 CUDA/Vulkan。
- 编译、设备、命令可用性三态测试。
- Windows Tauri package 含 engine 和 third-party notices。
- PE dependency scan 证明运行时可选。

完成标准：有/无 NVIDIA driver、有/无 Vulkan loader 的 Windows 环境都能打开 GUI 并看到真实状态；仍不得伪造 analyze 可用性。

### M6: 性能和默认启用评审

交付：

- 5-run median 报告。
- B7、generic、both、mixed shape profile。
- 内存、取消和稳定性报告。
- 每个 backend 独立 go/no-go。

完成标准：只有通过 3x 门槛的 backend 才提交默认策略变更；未通过的后端仍可作为显式 experimental backend，不降低正确性门槛。

## 15. Windows 验证命令

在 VS Developer PowerShell 或配置好 Ninja/Clang 的 shell 中执行。具体 generator 可以调整，但报告必须保存完整命令。

CPU-only：

```powershell
cmake -S engine -B build/engine-win-cpu -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DGETNATIVE_ENABLE_METAL=OFF `
  -DGETNATIVE_ENABLE_CUDA=OFF `
  -DGETNATIVE_ENABLE_VULKAN=OFF
cmake --build build/engine-win-cpu --parallel
ctest --test-dir build/engine-win-cpu --output-on-failure
build/engine-win-cpu/getnative-engine.exe capabilities
```

完整 GPU build，架构值由实际 CI/toolkit 填写：

```powershell
cmake -S engine -B build/engine-win-gpu -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DGETNATIVE_ENABLE_METAL=OFF `
  -DGETNATIVE_ENABLE_CUDA=ON `
  -DGETNATIVE_CUDA_ARCHITECTURES=<validated-sm-list> `
  -DGETNATIVE_CUDA_PTX_ARCHITECTURE=<validated-compute-target> `
  -DGETNATIVE_ENABLE_VULKAN=ON `
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
cmake --build build/engine-win-gpu --parallel
ctest --test-dir build/engine-win-gpu --output-on-failure
build/engine-win-gpu/getnative_cuda_benchmark.exe --full --assert
build/engine-win-gpu/getnative_vulkan_benchmark.exe --full --assert
```

Packaging：

```powershell
Set-Location app
npm ci
npm run build
cargo test --manifest-path src-tauri/Cargo.toml
cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings
npm run tauri build
```

依赖扫描至少检查：

```powershell
dumpbin /DEPENDENTS build/engine-win-gpu/getnative-engine.exe
```

产品 executable 的 import table 不得出现：

- `nvcuda.dll`
- `cudart64_*.dll`
- `vulkan-1.dll`
- Python runtime
- VapourSynth/AviSynth runtime

再在没有 NVIDIA driver/Vulkan loader 的干净 Windows VM 中运行 `getnative-engine.exe capabilities` 和打包 GUI。这个 smoke 不能用开发机 PATH 隔离来替代。

## 16. 禁止事项和停止条件

遇到以下情况停止扩展范围，先修复证据链：

- CPU core/upstream conformance 失败。
- GPU 为了通过测试而修改 CPU oracle 或 threshold/crop 语义。
- strict kernel 使用 fast math、FMA contraction 或未记录的 relaxed precision。
- CUDA/Vulkan 选项 OFF 时仍要求 SDK 或 driver。
- PE import table 硬依赖 `nvcuda.dll`、`cudart` 或 `vulkan-1.dll`。
- backend 只有 compile proof 却报告 `device_available=true`。
- `analysis_command_available=true` 但真实 analyze command 不存在。
- GPU 错误静默切 CPU，或 strict 静默切 fast。
- 结果 candidate id/order 改变。
- 一个 candidate 超过 workspace limit 却继续提交。
- 工作集超过 2 GiB。
- 没有真实 NVIDIA/Vulkan Windows run 却声称 correctness/performance 完成。
- Windows agent 想直接改 `upstream/descale`、复制 `muvsfunc` 或引入未盘点的新依赖。
- 没有 Apple 验证却大范围重写 Metal backend。

## 17. 交回时必须提供的证据

Windows agent 的最终 handback 不能只写“测试通过”。必须包含：

1. 修改文件清单和每个文件的职责。
2. 收到的源基线清单/哈希，以及最终 diff 的建立方式。
3. Windows version、CPU、RAM、GPU、PCI/UUID、driver version。
4. Visual Studio/clang-cl、CMake、Ninja、CUDA Toolkit/nvcc、Vulkan SDK/glslc 版本。
5. 完整 configure/build/test/benchmark/package 命令和 exit code。
6. CPU-only、CUDA-only、Vulkan-only、CUDA+Vulkan 四种 build 结果。
7. 无 NVIDIA driver、无 Vulkan loader 的 startup/capability 结果。
8. 每个 conformance case 的最大误差、valley distance 和失败数。
9. 5 次 benchmark 原始时间、median、speedup、peak memory。
10. cancellation latency 和 tile duration。
11. `dumpbin /DEPENDENTS` 关键输出。
12. 未验证的平台、设备、架构和剩余风险。

## 18. 完成定义

本 handover 的 GPU 后端任务只有在下列条件全部满足时完成：

- CUDA 和 Vulkan 都有真实 Windows host adapter、embedded kernel/shader、设备枚举、执行和清理路径。
- 两者都能运行 H、V、both，支持 p=1 和所有 `half_bandwidth<=15` / `forward_width<=16` 的现有 filter。
- 两者都通过 CPU tolerance、valley、order、memory、cancel 和 failure-path tests。
- CPU-only build 和无 GPU runtime 的 packaged startup 不回归。
- capability 只报告已经完成的事实。
- CUDA/Vulkan runtime 保持可选，产品无 VS/Python/plugin runtime 依赖。
- 默认启用决策按 backend 独立评审；没有 3x 证据就保持显式选择。
- handback 包含第 17 节的可复核证据和明确的未验证边界。
