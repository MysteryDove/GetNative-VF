# Windows x86 CPU / CUDA / Vulkan Compute 后端开发 Handover

## 1. 任务目标和交付边界

本文件用于把 GetNative VF 的 Windows backend 开发交给另一名 agent。目标不是写独立 SIMD/CUDA/Vulkan demo，而是在现有 C++23 engine 中交付一个可回退的 x86 CPU 优化路径，以及两个可选、可诊断、可测试的 **极致优化 GPU backend**：

1. Windows x64 CPU：SSE2 / AVX2 / AVX-512 **production** ISA 分层（允许 FMA；正确性用 tolerance，不要求与 scalar bit-identical）。
2. Windows x64 + NVIDIA CUDA Driver API：**单一生产 artifact**，以数学输出精度 + 端到端性能为成功标准，并用 PTX/SASS 把 codegen 推到极限。
3. Windows x64 + Vulkan Compute：**单一生产 SPIR-V 路径**，目标与 CUDA 相同。

**CPU / GPU 数学路径政策（本 handover 权威定义，覆盖任何旧草稿或中间实现）：**

- CPU 与 GPU 均为 **production** 路径：允许并鼓励 FP32 FMA；目标是输出精度 + 性能，不是“与 scalar 逐位同路径”。
- CPU capability 使用 `math_modes: ["production"]` / `selected_math_mode: "production"`（单档，不是 multi-mode 切换面）。
- **不实现** CUDA/Vulkan 的 `strict` 数学通路（禁 FMA / 字面 SASS 无 HFMA / 为过扫描而 `-Xptxas=-O0` 等）。
- **不实现** 独立的 `fast-math` 产品/测试数学通路，也不得并列维护多套 math-mode fatbin/SPIR-V 供运行时切换。
- GPU 正确性由真实设备上的 metric tolerance、valley/order 与 generic/specialized 一致性判定。
- capability / provenance 对 GPU **不要** 暴露 `math_modes: [strict, relaxed-fma, fast-math]` 多档切换。

x86 CPU production 路径先完成，因为它既是 Windows fallback，也是 CUDA/Vulkan 性能结论的真实分母。随后 CUDA 优先；Vulkan 在共享 GPU packing、数值契约和测试矩阵稳定后实现。三个执行路径都必须消费 CPU planner 生成的同一份 Float32 `AxisPlan`。AArch64 NEON 与 x86 production 同样允许 FMA；planner 的 Float64/LDLT 契约不变。

本任务有两个不同完成层级，不能混称：

- **Backend 完成**：x86 CPU runtime dispatch 与 production SIMD、GPU C++ library、真实 GPU 执行、conformance、benchmark、capability 和 Windows packaging 完成。此时如果真实 `analyze` 命令仍不存在，`commands.analyze` 与 `analysis_command_available` 必须保持 `false`。
- **产品端到端完成**：持久 worker/CLI analyze protocol、Tauri job controller、前端 analysis planner、进度、取消和结果展示也已接通。当前仓库尚未达到这一层。

本次不包含媒体解码、色彩转换、导出格式或新滤镜语义。不要为了接 SIMD/GPU 而扩展这些范围。也不要把 planner 的 Float64 tap、normal-band 或 LDLT 构造纳入 x86 SIMD 第一阶段；当前 planner 已有独立的 batch/cache 性能证据和边界。

## 2. 接收基线和证据边界

### 2.1 当前 workspace 快照

本 handover 按 `2026-07-31` 的 workspace 重写，源码基线如下：

- 仓库：`<repo>`，即 Windows agent 收到的 GetNative-VF 根目录。
- 分支：`main`。
- `HEAD`：`0b035fa4ed0e2b96fbd5b988a653653f03d2cb61`，commit `perf: add 810p planning and verification evidence`。
- 相对 `origin/main`：一致。
- 本轮 CUDA/PTX handover 修改开始前 worktree 干净；本文件是当前任务产生的唯一未提交 path。因为它是正在生成的交付物，不在正文内写自引用 hash，接收方必须对收到的最终文件另行取 hash。
- 最新 `HEAD` 已提交而非 dirty 的关键文件：
  - `DESIGN.md`，SHA-256 `fb759d557a57a229f06221cf44d37e1d94320506d52a8a0938ad403137026234`；
  - `docs/gui-development-spec.md`，SHA-256 `07030242c7abae5338d75303530689a26c3d37fba4efe83d042a3b70d5684d04`；
  - `docs/performance/pre-gpu-planner-results.md`，SHA-256 `07b5ada963d9c45cc844ab3e307ac184f136bdda5d6c60335843177bcb399b6d`；
  - `docs/performance/fixed-recipe-multiframe-results.md`，SHA-256 `71553a039955cd83f1ce119e6a4bafd2ccd601fe5fb0d0f2ba13366aae97abd3`；
  - `engine/CMakeLists.txt`，SHA-256 `a410a4a1fafdff4439a06d43c9edb5ba51195ac075b1d9eff613de304da12842`；
  - `engine/bench/metal_benchmark.cpp`，SHA-256 `257ec8f941b1f71b20d9f5ae38e463e6fb4fbf1b6419d3706b2c1fc3a295a9f0`；
  - `engine/bench/fixed_recipe_benchmark.cpp`，SHA-256 `a61b616f089441567ece91ee1ba2eb6c85f0e726295f382e8df4027241ffe7cb`。
- `DESIGN.md` 和 GUI spec 扩展了产品 planner/RunGroup/Recipe 目标；planner/fixed-recipe results 与 Metal benchmark 扩大了 planner、kernel/native-height、session-cache 和 prepared-once 多帧测量面。它们是当前 committed baseline，不是 Windows agent 可以删除的临时文件。

Windows agent 开工时必须重新记录：

```powershell
git rev-parse HEAD
git status --short --branch
git diff --stat
Get-FileHash -Path @(
  "DESIGN.md"
  "docs/performance/pre-gpu-planner-results.md"
  "docs/performance/fixed-recipe-multiframe-results.md"
  "docs/windows-cuda-vulkan-handover.md"
  "docs/gui-development-spec.md"
  "engine/CMakeLists.txt"
  "engine/bench/metal_benchmark.cpp"
  "engine/bench/fixed_recipe_benchmark.cpp"
) -Algorithm SHA256
```

如果收到的状态与以上快照不同，以收到的 workspace 为准，先保存差异，再继续。禁止用 `git reset --hard`、`git checkout --` 或覆盖复制来“恢复基线”。

### 2.2 固定上游参考

- descale：`https://github.com/Irrational-Encoding-Wizardry/descale.git`
- descale revision：`8c53f5d1297dee286e5a854ae5731103614a0583`
- zimg revision：`1ad1895d5ff0bbe69c61243f9996aede713d1b5f`

本地 `upstream/descale` 与 `upstream/zimg` 在重写时均为干净 checkout。产品 runtime 不链接它们；它们只用于 test-only conformance。

### 2.3 尚未具备的证明

当前验证机是 Apple Silicon。仓库内没有 x86 SIMD、CUDA 或 Vulkan backend 源码，也没有 Windows x86、NVIDIA、AMD、CUDA driver、Vulkan loader 或真实 Windows package 的执行证据。因此：

- 当前 NEON 结果不能证明 SSE2、AVX2 或 AVX-512 正确或更快。
- “CMake 配置成功”不等于 backend 已运行。
- NVIDIA 上的 Vulkan 结果不证明 AMD 性能或驱动正确性。
- shader/fatbin 编译成功不证明数值等价。
- 无真实 Windows GPU 运行时，不得声称 CUDA/Vulkan 已完成、可默认启用或达到性能门槛。

## 3. 当前实现状态，按源码而不是设计稿判断

| 层 | 当前事实 | 主要证据 |
| --- | --- | --- |
| Filter/planner | Bilinear、任意有限 B/C Bicubic、Lanczos1-15、Spline16/36/64；Float64 构造和 LDLT，Float32 `AxisPlan` | `engine/include/getnative/filter.hpp`；`engine/src/planner/filter.cpp`；`engine/src/planner/axis_plan.cpp` |
| Batch planner | 按完整 bit-key 去重独立 plan，以有界 worker 并行构建；B/C sweep 可复用几何但每个 plan 仍独立权重和 LDLT | `engine/src/planner/axis_planner.hpp`；`engine/src/planner/axis_planner.cpp` |
| Session cache | 公开、caller-owned、固定 admission、整批成功后发布、非 single-flight | `engine/include/getnative/axis_plan.hpp:63-104`；`engine/src/planner/axis_plan.cpp:852-1006` |
| CPU backend | H、V、both、任意 `p>=1`、确定性 batch；AArch64 有 adjacent-column `neon-f32x8`，x86 当前退回 scalar；CPU 是严格参考与 fallback | `engine/include/getnative/cpu_analysis.hpp`；`engine/src/backend/cpu/cpu_analysis.cpp`；`engine/src/backend/cpu/inverse_columns.cpp`；`engine/src/backend/cpu/inverse_columns_neon.cpp` |
| Metal backend | H、V、both；p=1；B3/B7/B11/B15/generic；混合形状 tile；双轴两种 forward order；buffer reuse、遥测和取消后 drain | `engine/include/getnative/metal_analysis.hpp`；`engine/src/backend/metal/metal_backend.mm`；`engine/src/backend/metal/getnative.metal` |
| Fixed-Recipe benchmark | 新增但尚未提交；prepared-once 单一 vertical Recipe，predecoded deterministic frame ring；对比 CPU serial、bounded frame-parallel CPU 和 Metal serial calls，覆盖 1/2/10/100/1000 帧、cold total、throughput、MAD、correctness 和 decision gates；不是产品 planner | `engine/bench/fixed_recipe_benchmark.cpp`；`engine/CMakeLists.txt:325-333` |
| CLI | 仅 `capabilities`、`geometry`；`analyze=false` | `engine/src/cli/main.cpp:85-145` 和 `engine/src/cli/main.cpp:194-208` |
| Tauri | 每次命令启动 engine 子进程并等待输出；只注册 `engine_capabilities`、`engine_geometry` | `app/src-tauri/src/lib.rs:121-138` 和 `app/src-tauri/src/lib.rs:353-359` |
| React UI | geometry workbench；Analysis tab 由 `commands.analyze` 禁用；backend 行可通用展示 capability | `app/src/App.tsx:107`、`app/src/App.tsx:172`、`app/src/App.tsx:326-355` |
| Product planner design | `DESIGN.md` 已定义 Height/Kernel/Verification、Recipe、RunGroup 和 math-mode 语义；GUI spec 给出交付切片，但这些尚未进入 React/Rust/CLI | `DESIGN.md:25-46`、`DESIGN.md:289-413`、`DESIGN.md:502-565`；`docs/gui-development-spec.md:120-161` |
| CUDA | capability 占位，始终 `compiled=false` | `engine/src/cli/main.cpp:130` |
| Vulkan | 源码、CMake option 和 capability 均不存在 | 当前 tree |

实现事实和未来产品契约使用两条不同的优先级，不能混在一起：

```text
当前实现事实：源码和测试 > benchmark/evidence 文档 > architecture/旧 handover
未来产品语义：DESIGN.md > docs/gui-development-spec.md > docs/architecture.md
```

`docs/architecture.md` 仍包含未来 JSONL worker 和完整 GUI 的旧设计，也低估了最新 Metal B11/B15 specialization。`DESIGN.md` 的 Active contract 不代表对应代码已经存在。

## 4. “前端 planner”必须拆成四个不同概念

### 4.1 已确定但尚未实现的产品 planner

最新 `DESIGN.md` 与 `docs/gui-development-spec.md` 已确定以下产品语义：

- Height search：一个 engine Run 是一个 Sample、一个固定 kernel、多个 height candidate。
- Kernel search：一个 engine Run 是一个 Sample、一个固定 geometry、多个 kernel candidate。
- Verification：一个 engine Run 是一个 Source、一个固定 locked Recipe、多个 frame。
- 多 Sample、多固定 kernel 或多 Source 是 UI-level `RunGroup`，成员失败、取消和 provenance 独立；不能拼成一个越界 engine request。
- H+W、H-only、W-only 都是一级轴模式。
- Recipe 锁定 geometry、kernel、MetricSpec、compatibility profile；CPU 侧 math mode 固定 strict。GPU 无多档 math mode。backend/device 属于 Run provenance。
- backend fallback 必须可见并写 provenance，不能静默发生。

这些是 Windows backend 最终要服务的调用形状，但当前 tree 中没有 `HeightRun`、`KernelRun`、`VerificationRun`、`RunGroup` 或 Recipe persistence 的实现。不要在 backend 内发明临时 JSON schema 来替代产品 integration lane。

### 4.2 当前 React frontend

React 当前不是 analysis planner。它只：

1. 调用 `engine_capabilities`。
2. 调用 `engine_geometry`。
3. 展示 backend 的 compiled/device/command/axes/p/shape 状态。

它没有 candidate grid、`AxisPlanRequest`、backend selection、job queue、progress、cancel 或 result curve。不要把 C++ backend 完成误写成“前端 planner 已接通”。

### 4.3 当前 Tauri controller

Rust 当前通过 `std::process::Command::output()` 为每个请求启动一次 engine，无法承载长任务的流式进度、job id 或 cooperative cancel。真正的 analysis controller 需要持久 child process 和版本化 JSONL protocol；这是一项独立集成工作，不应藏进 CUDA/Vulkan host adapter。

当前 Rust capability validator 还存在三个硬限制：

- `schema_version` 必须为 2。
- backend 数组必须严格等于 `cpu, metal, cuda`，并按 index 读取。
- `commands.analyze=true` 或 `cuda.compiled=true` 会被测试拒绝。

见 `app/src-tauri/src/lib.rs:140-257` 和 `app/src-tauri/src/lib.rs:461-478`。

### 4.4 Engine-side primitives 与缺失的 orchestration planner

当前已有的公开 primitives 是：

```cpp
std::vector<Candidate> generate_candidates(
    const CandidateGridSpec &spec, GridSemantics semantics);

Geometry descale_geometry(...);
Geometry descale_geometry_pro(...);

AxisPlan build_axis_plan(const AxisPlanRequest &request);
AxisPlanCacheBatchResult AxisPlanCache::get_or_build_batch(...);

std::vector<CandidateResult> analyze_batch_f32(...);
std::vector<CandidateResult> MetalAnalysisEngine::analyze_axis_batch_f32(...);
```

对应 source of truth 为 `engine/include/getnative/candidate_grid.hpp`、`crop_geometry.hpp`、`axis_plan.hpp`、`cpu_analysis.hpp` 和 `metal_analysis.hpp`。`generate_candidates()` 已区分 repeated-addition、index-multiplication 与 decimal fixed-point semantics；Windows integration 不得在 TypeScript/Rust 中用另一套浮点循环重建 candidate sequence。

产品最终需要的 analysis planner 应负责：

1. 从 profile/geometry/grid 生成稳定顺序的 candidate request。
2. 生成完整 `AxisPlanRequest` 集合。
3. 在 scan/video session 生命周期内调用公开 `AxisPlanCache::get_or_build_batch()`。
4. 按请求顺序把 returned `shared_ptr<const AxisPlan>` 附到 `CandidateAnalysis`。
5. 把 source、candidate span、metric 和 stop token 交给一个明确 backend。
6. 保存 backend/device、cache 命中、tile、strict mode、timing 和失败 provenance。

当前 repository 没有把这些 primitives 组合成 Height/Kernel/Verification Run 的端到端对象。Windows backend 不得绕过它自己生成 filter taps，也不得在 CPU/GPU context 内建立另一套 planner cache。

最新未提交的 `engine/bench/fixed_recipe_benchmark.cpp` 演示了“先构造一个 immutable `AxisPlan`，再跨 1/2/10/100/1000 个预解码 frame 复用”的 bounded benchmark shape，并比较 CPU serial、CPU frame-parallel 和 Metal serial calls。它只有一个 locked vertical candidate，不具备 candidate generation、RunGroup、Recipe persistence、job/cancel 或 UI state，因此只能作为 prepared-once execution/telemetry 参考，不能被提升为 frontend planner 实现。

## 5. Planner 和 cache 的真实接口契约

### 5.1 公开接口

候选与几何入口必须先读：

- `engine/include/getnative/types.hpp:16-49`：`GridSemantics`、`CandidateGridSpec`、`Candidate`、`Geometry`。
- `engine/include/getnative/candidate_grid.hpp:9-11`：`generate_candidates()`。
- `engine/include/getnative/profile.hpp:11-14`：profile 和默认 grid semantics。
- `engine/include/getnative/crop_geometry.hpp:10-24`：standard/pro geometry。

Planner/cache/backend 桥必须读 `engine/include/getnative/axis_plan.hpp:20-104` 与 `engine/include/getnative/cpu_analysis.hpp:72-111`：

```cpp
AxisPlan build_axis_plan(const AxisPlanRequest &request);
std::size_t axis_plan_storage_bytes(const AxisPlan &plan) noexcept;

class AxisPlanCache {
public:
    std::shared_ptr<const AxisPlan> get_or_build(const AxisPlanRequest &request);
    AxisPlanCacheBatchResult get_or_build_batch(
        std::span<const AxisPlanRequest> requests,
        std::size_t worker_count = 0U);
    AxisPlanCacheLimits limits() const noexcept;
    std::size_t size() const;
    std::size_t resident_bytes() const;
    void clear();
};
```

默认 cache 限制：

```text
maximum_entries        = 1024
maximum_resident_bytes = 256 MiB
```

`AxisPlanCacheBatchResult` 的字段语义：

| 字段 | 精确定义 |
| --- | --- |
| `plans` | 与输入 request 数量和顺序完全一致，可共享 pointer |
| `unique_key_count` | 本次输入中的唯一完整 key 数 |
| `ready_hit_count` | 调用开始时已 resident 的输入 request 数，不是唯一 key 数 |
| `physical_build_count` | 本次实际构建的唯一 miss 数 |
| `published_plan_count` | 本次新进入 cache 的 plan 数 |
| `peak_active_builds` | 同时进行的 plan build 峰值 |
| `effective_worker_count` | 本次 miss build 的实际 worker 上限 |
| `resident_entry_count` | 返回时 cache 内 entry 数 |
| `resident_bytes` | `axis_plan_storage_bytes()` 口径的逻辑 resident bytes |

### 5.2 完整 cache key

`engine/src/planner/axis_plan_key.hpp:11-56` 定义完整 exact-bit key：

```text
source_size, destination_size,
bit(active_length), bit(shift),
kernel type, bit(b), bit(c), taps,
border mode
```

不得用 candidate id、height label、filter name 或格式化 decimal string 代替该 key。`+0.0` 与 `-0.0` 也按 bit 区分。

### 5.3 并发和失败语义

`detail::build_axis_plans()` 是内部实现，不是新 backend 的产品 API。其语义必须理解并保留：

- 先按完整 `PlanKey` 做 stable first-occurrence dedup。
- 每一个完整 `AxisPlan` 内部仍串行执行 tap、normal bands 和 LDLT。
- 并行只发生在互相独立的 unique plan 之间。
- 自动 worker 为 `min(unique_count, hardware_concurrency, 8)`，最少 1；unique 数不超过 2 时强制单 worker。
- 失败后停止领取新 index，等待已开始 worker join，再抛出最低稳定 index 的失败。
- 一个 batch 失败时，`AxisPlanCache` 不发布本批的部分成功结果。

`AxisPlanCache` 是线程安全发布，不是 single-flight：多个并发 cold call 可以重复构建同一 miss，最终已有 entry 胜出。不要把它改成全局 single-flight service，除非新的 workload 和 benchmark 明确授权。

### 5.4 固定 admission，不是 LRU

Cache 不淘汰已有 entry。达到 entry/byte 上限后：

- overflow plan 仍返回给当前调用者；
- overflow plan 不进入 cache；
- 下次仍可能重建；
- 已 resident pointer 不因新 miss 改变；
- `clear()` 释放 cache ownership，但外部 `shared_ptr` 仍有效。

建议的产品生命周期是“一次 scan 或一个 video session 一个 cache”，不是 process-global cache。

### 5.5 Bicubic 几何复用不是 GPU topology cache

当前 batch planner 对非退化 B/C sweep 可复用 B/C-independent sampling geometry，见 `engine/src/planner/axis_planner.cpp:78-224`。每个 plan 仍然：

- 计算自己的 B/C weights 和 zero mask；
- 生成自己的 transpose values；
- 构造自己的 normal bands；
- 按原顺序执行自己的 LDLT；
- 产生自己的完整 `AxisPlan`。

`B=0,C=0` 因 exact-zero topology 单独处理。不要把此优化误读为 GPU plan interning 或跨 backend packed buffer cache。

### 5.6 当前 planner 没有 stop token

`get_or_build_batch()` 不接收 `std::stop_token`。Metal 的 stop token 只覆盖 backend execution。Windows agent 不得声称 cancel 可以中断当前 planner build；如果要扩展 planner cancellation，需单独设计失败/发布语义和测试，不能在 backend 工作中顺手加入。

## 6. 不可改变的数学和数据契约

### 6.1 数学定义

每个轴的 forward resize 矩阵为 `A`。给定观测向量 `b`：

```text
x = (A^T A)^-1 A^T b
```

CPU planner 负责：

1. 以 Float64 构造 descale 稀疏 `A`。
2. 以 Float64 直接构造 banded `A^T A`。
3. 以 Float64、固定循环顺序做 LDLT。
4. 构造 zimg-compatible forward rows，包括边界 residual-carry。
5. 按固定顺序投影为 Float32 `AxisPlan`。

GPU 只负责：

1. `A^T b`。
2. `LD y = A^T b` forward solve。
3. `L^T x = y` backward solve。
4. 使用已存储 forward weights 重建。
5. strict crop、`difference > threshold`、p=1 partial reduction。

GPU 不生成 filter weight，不做 border mapping，不构造 `A^T A`，不做 Float64 LDLT。

### 6.2 `AxisPlan` 布局

`engine/include/getnative/axis_plan.hpp:29-58` 是唯一 source of truth：

- `forward_offsets/indices/weights`：zimg-compatible forward rows。
- `transpose_offsets/indices/weights`：按 native output index 组织的稀疏 `A^T`。
- `lower_ld[(distance-1)*destination_size+i]`：`L(i,i-distance)*D(i-distance)`。
- `upper_l[(distance-1)*destination_size+i]`：`L(i+distance,i)`。
- `inverse_diagonal[i]`：`1/D(i)`。

GPU packing 可以把每行连续的 `forward_indices` 无损压为一个 `forward_left`，但必须先验证：

- `forward_offsets[0]` 和 `transpose_offsets[0]` 都是 0；
- 两张 offset table 均非递减，每项不超过对应 entry count，尾项恰好等于 entry count；
- 每行 element 数等于 `forward_width`；
- index 从 `left` 开始严格连续；
- `0 <= left <= destination_size-forward_width`；
- transpose index 在 source axis 范围内；
- `forward_weights`、`transpose_weights`、`lower_ld`、`upper_l`、`inverse_diagonal` 全部 finite；
- 所有 offset/product/addition 收窄到 32-bit 前先检查 overflow。

`AxisPlan::valid()` 当前只覆盖维度和 vector size 的基本一致性，不是 GPU 上传安全证明，见 `engine/src/planner/axis_plan.cpp:669-683`。共享 `gpu_batch` 必须在任何 allocation、upload 或 dispatch 之前执行上述深度校验；malformed plan 必须稳定失败，不能依赖 host vector 越界、shader 越界或设备 validation layer 才发现。

不得重算或重新归一化任何 coefficient。

### 6.3 GPU POD ABI

当前 Metal host/shader 使用相同字段顺序：

```cpp
struct alignas(16) AxisPlanDescriptor; // 64 bytes, 16 x uint32
struct AnalysisJob;                    // 40 bytes
```

见 `engine/src/backend/metal/metal_backend.mm:59-100` 与 `engine/src/backend/metal/getnative.metal:10-40`。共享 Windows packing helper 可以命名为 `GpuAxisPlanDescriptor`/`GpuAnalysisJob`，但必须保持：

- size、alignment、每个 field offset 有 `static_assert`；
- CUDA device struct layout test；
- Vulkan std430 stride 或 scalar layout 与 64-byte host stride 一致；
- 40-byte job 若走 push constants，其 offset 与 shader 一致。

`reserved_0`/`reserved_1` 在双轴阶段实际承载 intermediate/native base 和 native-height 信息。重命名时不能只改 host，不改全部 kernel 和 layout test。

### 6.4 Filter、shape 和 specialization

下表按 destination axis 足够大时的通常形状列出。小 destination 可能裁剪实际 `half_bandwidth`，仍应走 validated generic path。

| Filter | support | descale full bandwidth | `half_bandwidth` | `forward_width` | 当前 Metal path |
| --- | ---: | ---: | ---: | ---: | --- |
| Bilinear / Lanczos1 | 1 | 3 | 1 | 2 | B3/F2 specialized |
| Bicubic / Spline16 / Lanczos2 | 2 | 7 | 3 | 4 | B7/F4 specialized |
| Spline36 / Lanczos3 | 3 | 11 | 5 | 6 | B11/F6 specialized |
| Spline64 / Lanczos4 | 4 | 15 | 7 | 8 | B15/F8 specialized |
| Lanczos5 | 5 | 19 | 9 | 10 | generic |
| Lanczos6 | 6 | 23 | 11 | 12 | generic |
| Lanczos7 | 7 | 27 | 13 | 14 | generic |
| Lanczos8 | 8 | 31 | 15 | 16 | generic |

CPU core 支持 Lanczos1-15，因此 capability 为 `max_half_bandwidth=29`、`max_forward_width=30`。Metal 当前仅接受 `1..15` 和 `1..16`，GPU v1 与 Metal 对齐。不要提前广告 29/30。

### 6.5 strict 累加顺序

以下循环顺序不能做“数学等价”重排：

- `A^T b`：transpose entry index 递增。
- forward solve：distance 从最远可用项递减到 1。
- backward solve：
  - B7，即 `half_bandwidth==3`，distance 从 1 递增，near-to-far；
  - B3、B11、B15 和其他 generic shape，distance 从最远项递减到 1。
- forward reconstruction：tap 从 0 递增到 `forward_width-1`。
- threshold：只有 `difference > threshold` 才累加，等于 threshold 不累加。
- workgroup/block：固定 256-thread 二叉树，Float32 partial。
- host：candidate-major，再按 group index 递增，把 Float32 partial 转为 double 后求和，最后除 cropped pixel count。

CPU 精确参考见 `engine/src/planner/axis_plan.cpp:1010-1097` 和 `engine/src/backend/cpu/cpu_analysis.cpp:327-408`。Metal 对应见 `engine/src/backend/metal/getnative.metal:47-121` 与 `engine/src/backend/metal/getnative.metal:331-402`。

GPU **不是** bitwise CPU/GPU 相等承诺，也 **不是** “与 CPU 相同中间舍入路径”的承诺。CUDA/Vulkan 的正确性以真实设备上逐 candidate metric tolerance、finite、order 与 valley/top-k 门槛为准（见第 15.4 节）。允许 FP32 FMA 与正常后端优化；不得为了“SASS 里看不到 FMA 字样”而关闭 ptxas 优化或维护第二条 slow/reference 数学通路。

Float16 coefficient storage 已因 Lanczos8 valley 漂移被移除，见 `docs/backend-hotpath-evidence.md:32`。不得在 Windows MVP 中重新引入。

## 7. 当前 Metal backend 是主要 GPU 参考

### 7.1 公开 API

`engine/include/getnative/metal_analysis.hpp:15-103` 定义：

- `MetalDeviceInfo`。
- `MetalKernelDispatchPolicy::{automatic,generic_only,required_specialized}`。
- `MetalAnalysisOptions`。
- `MetalRuntimeTelemetry`。
- `MetalAnalysisEngine::analyze_axis_batch_f32(...)`。
- `runtime_telemetry()`、`reset_analysis_telemetry()`、`trim_working_buffers()`。

实际执行边界是：

```cpp
std::vector<CandidateResult> analyze_axis_batch_f32(
    ConstImageView source,
    std::span<const CandidateAnalysis> candidates,
    const MetricSpec &metric,
    std::stop_token stop = {});
```

`CandidateAnalysis`、`CandidateResult`、`ConstImageView`、`MetricSpec` 和 `AnalysisAxes` 来自 `engine/include/getnative/cpu_analysis.hpp:14-82`。其中 `CandidateAnalysis` 是 planner 与 backend 之间的现有桥：planner 负责附上 immutable `shared_ptr<const AxisPlan>`，backend 只消费，不取得 cache ownership。CUDA/Vulkan public API 应复用这些输入/输出类型和调用语义，只让 device info、selector、options、telemetry 保持 backend-specific。

尽管函数名含 `axis`，它支持 H、V 和 both。CUDA/Vulkan 公开 API 应保持同形，并增加后端特有 device selector，但不要复制 Objective-C 类型。

默认 Metal options：

```text
tile_size                          = 32
reduction_groups_per_candidate     = 8
inverse_threads_per_threadgroup    = 32
workspace_limit_elements           = 0 (device-bound)
profile_split_kernels              = false
kernel_dispatch                    = automatic
reuse_working_buffers              = true
retained_working_buffer_limit      = 2 GiB
```

### 7.2 五个 stage，五类 shape

Metal shader 包含五个逻辑 stage：

1. `inverse_axis_*`：source image 到单轴/双轴第一个 intermediate。
2. `inverse_axis_matrix_*`：intermediate 到二维 native。
3. `forward_axis_matrix_*`：双轴 first forward。
4. `metric_axis_p1_*`：单轴或 vertical-first 双轴 final forward + metric。
5. `metric_axis_p1_horizontal_first_*`：horizontal-first 双轴 final forward + metric。

每个 stage 都有：

```text
b3, b7, b11, b15, generic
```

Host 构造 engine 时 eager 创建 generic、B3、B7 的全部 stage；B11/B15 首次使用时 lazy 创建。`required_specialized` 只有 inverse 和 forward shape 都有 fixed path 时才允许 dispatch。

### 7.3 单轴和双轴执行图

单轴 H/V：

```text
image inverse -> fused final forward + p1 metric -> Float32 partials
```

双轴：

```text
inverse H -> inverse V -> selected first forward -> fused second forward + p1 metric
```

Inverse 顺序固定 H 后 V。`select_forward_order()` 只选择 reconstruction 是 horizontal-first 还是 vertical-first，见 `engine/src/backend/cpu/cpu_analysis.cpp:245-257`。

`profile_split_kernels=false` 时同一 tile 的 stage 编码进一个 command buffer；true 时拆开用于 profiler，不得改变结果。

### 7.4 Packing 和 tile signature

参考 `engine/src/backend/metal/metal_backend.mm:152-431`：

- `PackedTile` 分别保存 descriptor、transpose offsets/indices/weights、lower、upper、diagonal、forward-left/weights。
- `TileSignature` 包含 axes、H/V inverse shape、H/V forward shape 和 forward order。
- 只把相邻、signature 相同的 candidate 放入一个 tile。
- tile 同时受 `tile_size` 和 workspace byte limit 限制。
- candidate id 和最终 result 顺序永远与输入相同。

不要为了提高 batching 先排序 candidate。若以后想重排，必须保留 inverse permutation、稳定失败语义和完整测试；MVP 不做。

### 7.5 Workspace、buffer reuse 和 queued tiles

当前 Metal 使用 shared `MTLBuffer`，适合 Apple UMA，但不是 Windows discrete GPU 的默认内存策略。

每次 public call：

- source buffer、最大 tile workspace、全 candidate partial buffer 由 grow-to-fit retained capacity 提供；
- source 内容仍每次 call 上传；
- workspace 在 tile 间复用；
- partial buffer 保留所有 candidate 的 group partial；
- plan buffers 当前每 tile 单独分配和上传；
- 最多先提交 32 个 tile，再 drain；
- peak working set 包含 retained working buffers 与 queued plan buffers。

持久 working buffers 已通过测试并保留。以下 Metal 实验已被实测移除或推迟：

| 实验 | 当前结论 | Windows 含义 |
| --- | --- | --- |
| B11/F6、B15/F8 specialization | 保留，约 13%-18% case 改善 | CUDA/Vulkan generic 正确后应实现并独立 profile |
| B19/F10 到 B31/F16 specialization | Metal 上未达 gate，已移除 | 先保持 generic；NVIDIA/AMD 若 profile 显示收益可重新评估 |
| plan upload arena/ring | 未达 wall-time gate，已移除 | 不直接照搬；先采 profiler |
| packed topology interning | working-set/wall gate 未过，已移除 | 不引入 topology ABI 或 equality cache |
| packed-content cache | 缺少 planner canonical identity 传递，推迟 | 不在 backend 中每次 hash 大量 plan bytes |
| Float16 coefficients | 数值/valley gate 失败，移除 | 禁止作为 MVP 优化 |

证据见 `docs/backend-hotpath-evidence.md:14-32`。

### 7.6 取消和对象复用

一个 `MetalAnalysisEngine` 的 public calls 由 mutex 串行化。stop 检查发生在：

- call 开始；
- tile 规划；
- 每个 tile；
- 每个 encoder 前；
- drain 后。

已提交 command buffer 不会被强行终止。发生 cancel 或其他异常时，backend 必须 drain 所有已提交 work，再抛出原始失败；之后同一 engine 可以立即安全复用。CUDA stream/Vulkan queue 也必须提供等价资源安全，不得返回时仍让 GPU 写入已释放/复用 buffer。

### 7.7 遥测

Windows backend 至少提供与 Metal 同类的：

- allocation count/bytes/time；
- working-buffer active/retained/peak；
- source upload、plan upload、buffer wiring；
- submission/completion count；
- plan upload bytes；
- analyzed/generic/specialized tile count；
- module/pipeline creation time 和已创建 kernel 名；
- GPU execution time；
- peak workspace 和 peak total explicit working set。

这些字段用于性能归因，不能只报告一个总 wall time。

最新未提交的 `getnative_fixed_recipe_benchmark` 已测 `buffer_allocation`、working/plan allocation、source/plan upload、buffer wiring、pipeline creation、GPU execution 和 host residual，并明确把 decode/color conversion 标为不适用。但当前 Metal telemetry 仍不能独立拆出 `plan_pack_ms`、`readback_ms` 和 `cpu_merge_ms`，这些被包含在 host residual。Windows CUDA/Vulkan benchmark 应保留“不可分离”的诚实状态，或增加明确计时边界；不得从总 wall 反推后伪装成实测 phase。

## 8. descale 和 zimg 的参考边界

### 8.1 descale 中适合参考的代码

优先读标量 C，因为它明确表达 strict 顺序：

| 文件位置 | 参考内容 |
| --- | --- |
| `upstream/descale/src/descale.c:288-319` | B3 horizontal solve |
| `upstream/descale/src/descale.c:322-369` | B7 horizontal，backward near-to-far |
| `upstream/descale/src/descale.c:372-409` | generic horizontal |
| `upstream/descale/src/descale.c:412-442` | B3 vertical |
| `upstream/descale/src/descale.c:445-491` | B7 vertical |
| `upstream/descale/src/descale.c:494-532` | generic vertical |
| `upstream/descale/src/descale.c:535-559` | B3/B7/generic dispatch |

AVX2 只作为数据复用和寄存器压力参考：

- `upstream/descale/src/x86/cpuinfo_x86.c:26-104`：MSVC/GNU CPUID、XGETBV 和 AVX2/AVX-512 bit 查询骨架。
- `upstream/descale/src/x86/descale_avx2.c:100-211`：B3 horizontal。
- `upstream/descale/src/x86/descale_avx2.c:215-358`：B7 horizontal。
- `upstream/descale/src/x86/descale_avx2.c:361-467`：generic horizontal。
- `upstream/descale/src/x86/descale_avx2.c:561-744`：vertical fixed/generic。

Pinned descale 没有 SSE2 或 AVX-512 solver。它的 AVX2 solver 明确使用 aligned `_mm256_load_ps` 和 `_mm256_fmadd_ps`/`_mm256_fnmadd_ps`，会改变 GetNative strict 的舍入语义，因此只能帮助理解 register reuse，不能直接成为 GetNative strict 或 unaligned/tail 实现。`cpuinfo_x86.c` 也只是 probe 参考；GetNative 必须使用第 10.2 节完整 `XCR0 & 0xE6`、compiled mask 和实际 emitted-extension 检查。

适合 CUDA 的思想：

1. 一个 thread 负责一个 candidate-vector 的完整有序 solve。
2. 并行来自 candidate 与独立 row/column，不来自拆分同一 triangular dependency chain。
3. B3/B7 用 compile-time fixed bandwidth 减少 branch/load。
4. Generic 从 SoA factor bands 读取，先保证顺序正确。
5. H/V 共用 direction/stride/index helper，不复制数学定义。

descale 没有 GetNative 最新 B11/B15 GPU specialization。B11/B15 应参考当前 Metal shader 和 specialization tests，而不是声称来自 descale 上游。

### 8.2 不应搬到 GPU 的代码

以下内容只作为 CPU planner 或 test oracle：

- `upstream/descale/src/descale.c:37-142`：dense/banded helper、LDLT、factor compression。
- `upstream/descale/src/descale.c:166-285`：filter、round-half-up、border、dense weights。
- `upstream/descale/src/descale.c:562-647`：`create_core`、dense `A^T A` 和 reference packing。
- `upstream/descale/src/descale.c:650-685`：reference lifecycle 和 CPU dispatch。
- VapourSynth/AviSynth plugin glue。
- `DESCALE_MODE_CUSTOM` callback。

GetNative 已用 banded planner 独立实现并由 `engine/tests/upstream_conformance_test.cpp:43-194` 做 bit-level conformance。不要退回 dense allocation，也不要把 `DescaleCore` 变成 GPU ABI。

zimg 只用于验证 forward rows。GPU 必须消费 `AxisPlan.forward_*`，不能在 shader 中重新运行 zimg filter logic。

### 8.3 许可证

descale 是 MIT，notice 已在 `THIRD_PARTY_NOTICES.md:8-32`。复制 substantial code 时保留 copyright/permission notice。产品 runtime 不链接 descale/zimg。

不要修改 `upstream/descale` 来适配 MSVC。若 test-only C target 的 `restrict` 不兼容，可在该 target 单独定义 `restrict=__restrict`，或用 clang-cl 构建 conformance；产品 core 继续使用 GetNative 自身实现。

## 9. 建议文件边界

以下均为**待新增**，当前 tree 不存在：

```text
engine/src/backend/cpu/cpu_features_x86.hpp
engine/src/backend/cpu/cpu_features_x86.cpp
engine/src/backend/cpu/inverse_columns_sse2.cpp
engine/src/backend/cpu/inverse_columns_avx2.cpp
engine/src/backend/cpu/inverse_columns_avx512.cpp
engine/bench/cpu_backend_benchmark.cpp

engine/src/backend/gpu/gpu_batch.hpp
engine/src/backend/gpu/gpu_batch.cpp
engine/tests/gpu_batch_test.cpp

engine/include/getnative/cuda_analysis.hpp
engine/src/backend/cuda/cuda_backend.cpp
engine/src/backend/cuda/getnative_cuda.cu
engine/src/backend/cuda/getnative_cuda_kernels.cuh
engine/src/backend/cuda/cuda_ptx_intrinsics.cuh   # 仅在第 12.7 节 gate 通过后增加
engine/tests/cuda_conformance_test.cpp
engine/tests/cuda_artifact_test.cpp
engine/bench/cuda_benchmark.cpp

engine/include/getnative/vulkan_analysis.hpp
engine/src/backend/vulkan/vulkan_backend.cpp
engine/src/backend/vulkan/vulkan_loader.cpp
engine/src/backend/vulkan/shaders/inverse_axis.comp
engine/src/backend/vulkan/shaders/inverse_axis_matrix.comp
engine/src/backend/vulkan/shaders/forward_axis_matrix.comp
engine/src/backend/vulkan/shaders/metric_axis_p1.comp
engine/tests/vulkan_conformance_test.cpp
engine/bench/vulkan_benchmark.cpp
```

若 profiler 批准 P1 row kernels，可再按 ISA 增加 `analysis_rows_sse2.cpp`、`analysis_rows_avx2.cpp` 和 `analysis_rows_avx512.cpp`，或放入同一组 ISA object libraries。文件名可调整，但不能把高阶 intrinsics 留在 baseline `cpu_analysis.cpp` 并对整个 target 开高阶 ISA flag。

共享 `gpu_batch` 保持 private C++ implementation，不作为稳定 public ABI。它负责：

- POD layout 和 static assertions；
- candidate/metric/plan validation，包括完整 offset table、index 和 finite-value 校验；
- B3/B7/B11/B15/generic shape 分类；
- lossless plan flatten；
- H/V/both workspace layout；
- adjacent signature tiling；
- overflow 检查；
- deterministic partial merge；
- backend-neutral tile metadata。

先让 CUDA/Vulkan 共用该 helper。不要在无 Apple build/test 的 Windows lane 中删除或重写 Metal 自带 packing。后续 Apple lane 可在 byte-for-byte CPU test 和 Metal conformance 下迁移 Metal。

不要在 shared helper 中加入已被移除的 topology interning、plan digest cache 或 GPU allocation object。

## 10. Windows x86 CPU backend 优化要求

### 10.1 当前边界和目标

当前 `engine/src/backend/cpu/inverse_columns.cpp` 只有编译期 AArch64 NEON 分支；x86 上 `column_simd_available()` 返回 false，`column_simd_name()` 返回 `scalar`。`engine/src/backend/cpu/cpu_analysis.cpp:134-202` 的 absolute-difference row 和 vertical reconstruction row 也只有 NEON/scalar 两套实现。因此 Windows x64 目前可正确运行，但实际执行路径是 scalar，不得把 C++ 编译器可能发生的自动向量化称为 x86 backend。

本任务必须保留 scalar **forced 回退**，并交付三个可诊断的 **production** ISA 执行层级（均可使用 FMA；正确性用 tolerance，不要求与 scalar 逐位一致）：

| ISA tier | 向量宽度 | 必须能力 | 语义 |
| --- | ---: | --- | --- |
| `scalar` | 1 | baseline C++ | 强制回退与诊断；实现使用 `std::fma` |
| `sse2` | 4 x Float32 | SSE2 | Windows x64 最低 SIMD 回退；无硬件 FMA 时用 mul+add 仿真 fused 接口 |
| `avx2` | 8 x Float32 | AVX、OSXSAVE、XMM/YMM state、AVX2、FMA | 使用 `_mm256_fmadd_ps` / `_mm256_fnmadd_ps` |
| `avx512` | 16 x Float32 | AVX-512 OS state、AVX512F（及代码实际用到的扩展）、FMA | 使用 `_mm512_fmadd_ps` / `_mm512_fnmadd_ps` |

AArch64 NEON production 路径同样使用 `vfmaq`/`vfmsq`。CPU 与 GPU 一致：**允许并鼓励 FMA**。

默认 production dispatch 的候选顺序是：

```text
AVX-512 -> AVX2 -> SSE2 -> scalar
```

“候选顺序”不等于盲目选择最宽 ISA。AVX-512 可能因频率下降或实现寄存器压力慢于 AVX2；没有第 16.2 节同机数据时，自动策略应保守选择 AVX2。只有 AVX-512 在规定 case matrix 上满足选择门槛，才可在对应 CPU signature 上自动选中。SSE2 是 Windows x64 的最低 SIMD tier，scalar 仍必须可被强制运行，并在 feature probe 异常或显式测试时回退。

### 10.2 CPUID、XCR0 和安全 dispatch

实现一个可单测的 x86 feature snapshot；Windows 可用 `__cpuidex` 和 `_xgetbv`，但业务 dispatch 不得散落直接 intrinsic probe。至少记录 CPU vendor、family/model/stepping、逻辑处理器数、CPUID bits、XCR0、各 tier 的 `compiled`、`available` 和最终 `selected`。

判定规则：

- 先用 leaf 0 取得最大 basic leaf；不支持 leaf 7 时对应 tier 为 unavailable。只有 OSXSAVE bit 已置位才执行 `_xgetbv(0)`。
- SSE2：CPUID leaf 1 EDX bit 26。Windows x64 ABI 保证 SSE2，但仍记录 probe 结果并让测试可注入异常 snapshot。
- AVX2：leaf 1 ECX 的 OSXSAVE bit 27、AVX bit 28 都为 1；`XCR0 & 0x6 == 0x6`，表示 OS 保存 XMM/YMM；leaf 7 subleaf 0 EBX AVX2 bit 5 为 1。
- AVX-512：先满足 AVX2 的 OS state 前提，再要求 `XCR0 & 0xE6 == 0xE6`，即 XMM、YMM、opmask、ZMM_hi256、hi16_ZMM state 全部启用；至少要求 leaf 7 EBX AVX512F bit 16。若 object code 使用 AVX512DQ/BW/VL 或其他扩展，probe 必须逐项要求对应 bit，不能只检查 AVX512F。
- `compiled=false` 的 tier 即使硬件支持也不能 available；`available=false` 的 tier 不能进入 function pointer table。

CPUID 能力不代表 OS 已保存扩展寄存器。禁止只看 AVX/AVX2/AVX-512 bit 后直接调用；forced unavailable tier 必须在调用 ISA-specific function 前返回稳定错误，不能靠 `STATUS_ILLEGAL_INSTRUCTION` 证明检测有效。

为测试和 benchmark 提供明确 override，等价 surface 可采用：

```text
--cpu-isa auto|scalar|sse2|avx2|avx512
```

本 handover 的 CPU math mode 固定为 **`production`**（允许 FMA）。测试 override 不应变成不受校验的全局环境开关；若使用环境变量，只允许 test/benchmark process 读取，并输出 forced provenance。

### 10.3 Translation unit 和编译隔离

ISA-specific code 必须放在独立 translation unit 或 object library；baseline dispatcher、planner、CLI 和公共 CPU API 不得继承高阶 ISA flag：

- baseline/feature probe：x64 baseline，不含 AVX 指令；
- SSE2 production：MSVC x64 baseline 或 source-local SSE2，clang-cl `-msse2`；
- AVX2 production：仅该 object 使用 `/arch:AVX2`，或 clang-cl `-mavx2 -mfma`；
- AVX-512 production：仅该 object 使用 `/arch:AVX512`，或 clang-cl `-mavx512f -mfma`（及代码实际需要的子集）。

若 P1 被保留，`cpu_analysis.cpp` 只负责调用 baseline-safe private dispatcher；SSE2/AVX2/AVX-512 row intrinsics 仍分别编译在对应 ISA object 中。

不得对 `getnative_core`、engine executable、tests 或 package 全局设置 `/arch:AVX2`、`/arch:AVX512`、`-mavx2` 或 `-mavx512*`。否则 dispatcher 本身可能在 feature check 前执行高阶指令，使 SSE2 fallback 失效。构建后必须反汇编 baseline/SSE2/AVX2/AVX-512 objects：baseline/SSE2 不得出现高阶 VEX/EVEX 泄漏；**AVX2/AVX-512 production objects 应出现 FMA**（`vfmadd*`/`vfnmadd*` 或等价），作为优化证据而非失败条件。

现有 public `analyze_batch_f32()` 不需要为了 SIMD 改签名。可以扩展 private dispatch policy 和 benchmark/test override；只有产品 planner 真正需要显式 CPU math mode 时，才把受版本控制的 mode 放入 job/Recipe schema。不要让 public caller 传裸 function pointer。

### 10.4 production 数值契约

CPU production 路径以 **输出精度与性能** 为目标，不要求与 scalar 逐 Float32 bit-identical：

- 每个 lane 内保持第 6.5 节的 transpose、forward solve、backward solve 和 reconstruction **循环次序**；
- **允许并鼓励** FP32 FMA（`std::fma`、`vfma*`、`_mm*_fmadd_ps` 等）；SSE2 无硬件 FMA 时用 mul+add 仿真 fused 接口；
- 正确性：相对 forced scalar（或同次 reference）的 metric 使用与 GPU 同量级的 relative tolerance（默认约 `max(1e-7, 5e-4 * |ref|)`），并保持 finite、candidate id/order、valley 门槛；
- 不能用 horizontal reduction 改变像素提交顺序。向量 difference 先按 x 递增顺序写回/提取，再依次调用现有 `MetricAccumulator::add()`；
- 不改变 MXCSR 的 FTZ/DAZ 或 rounding mode；若调用方已有非默认 MXCSR，记录测试边界，不在 library 内修改全局线程状态；
- fixed `half_bandwidth==1`、`==3` 和 generic path 只可减少 branch，不能重排 B7 的特殊 backward order。

### 10.5 算子优化优先级

| 优先级 | CPU operator | 要求 | 当前参考 |
| --- | --- | --- | --- |
| P0 | adjacent-column inverse | SSE2/AVX2/AVX-512 跨相邻列并行；覆盖 `A^T b`、forward solve、B7/generic backward solve；fixed B3/B7 + generic；scalar tail | `inverse_columns_neon.cpp`；descale AVX2 只作寄存器/复用参考 |
| P1 | vertical reconstruction row | 按 x 向量化 tap FMA/mul-add，再生成 abs difference；保持 tap 顺序和 metric lane 提交顺序 | `cpu_analysis.cpp` |
| P1 | absolute-difference row | SSE2/AVX2/AVX-512 load/sub/abs，按 x 顺序送入现有 accumulator | `cpu_analysis.cpp:134-160` |
| P2 | 其他 CPU execution loops | 只在 profiler 证明占比后处理；可评估跨 row/candidate 并行，但不得改变 batch result 顺序 | `cpu_analysis.cpp:274-549` |

P0 完成并通过 conformance/benchmark 后再做 P1；P2 必须有 before/after profiler。不要 SIMD 化 Float64 tap 生成、normal-band 构造、LDLT、`AxisPlanCache` key/admission 或 product planner。它们不是这个 x86 backend 的 kernel，且重排会破坏现有 bit-level upstream 证据。

descale 的 x86 AVX2 代码可以帮助理解宽 load、临时寄存器、FMA 和 fixed/generic loop，但 GetNative 的 `AxisPlan` 布局、B7 backward order 和 border semantics 优先。不得复制 descale dense core 或它的 runtime dispatch 作为新架构。

### 10.6 Alignment、stride、tail 和边界

- 输入、输出、crop 起点和 row stride 不保证 16/32/64-byte alignment；正常路径使用 unaligned load/store，除非运行时明确验证 alignment 后进入等价 fast path。
- 列数/宽度小于 lane width 或不是 4/8/16 的倍数时必须正确。SSE2/AVX2 用 scalar tail；AVX-512 可用经过 bounds 测试的 mask 或 scalar tail。
- 不得越过 crop、row、allocation 末端读取，即使宽 load 的越界字节通常可映射；不得覆盖 row padding。
- 覆盖 `column_count=0`、奇数 width、misaligned base、non-contiguous stride、最小/裁剪后的 band、正负零、subnormal 和有限极值。source/plan 现有 validation 语义不能因 SIMD 变宽而放松。
- feature detection 和 dispatch 每次 process/session 缓存一次即可；不得在 inner row/column loop 重跑 CPUID。

### 10.7 自动选择和性能门槛

CPU benchmark 必须分别强制 scalar、SSE2、AVX2 strict 和 AVX-512 strict；记录 planner、execution 和 end-to-end，不把 planner/cache 时间算进 SIMD kernel speedup。每个 tier 先过正确性，才进入自动选择评审。

同一 CPU signature 上，较高 tier 相对下一 tier 的 primary matrix median 至少快 5%，且任何 primary case 不得回退超过 3%，才可自动选择；原始样本和 MAD 必须支持该结论。特别是 AVX-512 未通过相对 AVX2 的门槛时，保持 `available=true` 但 `selected=avx2`。若没有对应硬件数据，AVX-512 只能 forced experimental。

CUDA/Vulkan 的 `>=3x` 判定必须以同机 `auto + strict` 选出的最快正确 CPU end-to-end 路径为分母，不得继续用 scalar。

## 11. 必须完成和优化的 GPU operator/kernel

### 11.1 正确性优先级

| 优先级 | Operator/kernel | 输入到输出 | Dispatch | 当前参考 |
| --- | --- | --- | --- | --- |
| P0 | host validate/pack/tile | `CandidateAnalysis` -> flat tile | CPU | `engine/src/backend/metal/metal_backend.mm:206-431` |
| P0 | host partial merge | Float32 partials -> double metric | CPU | `engine/src/backend/metal/metal_backend.mm:1406-1413` |
| P1 | `inverse_axis_generic` | source -> single-axis native/intermediate | 1 thread / candidate-vector | `engine/src/backend/metal/getnative.metal:47-193` |
| P1 | `metric_axis_p1_generic` | native -> fixed partials | 1 block/workgroup / partial, 256 threads | `engine/src/backend/metal/getnative.metal:331-466` |
| P1 | single-axis scheduler | upload, inverse, metric, readback | tile | `engine/src/backend/metal/metal_backend.mm:1275-1304` |
| P2 | `inverse_axis_matrix_generic` | H intermediate -> 2D native | 1 thread / candidate-vector | `engine/src/backend/metal/getnative.metal:195-278` |
| P2 | `forward_axis_matrix_generic` | 2D native -> first forward intermediate | 1 thread / candidate-vector | `engine/src/backend/metal/getnative.metal:280-329` |
| P2 | horizontal-first metric generic | intermediate -> partials | 256 threads | `engine/src/backend/metal/getnative.metal:468-488` |
| P2 | both scheduler | inverse H/V, first forward, final metric | tile | `engine/src/backend/metal/metal_backend.mm:1305-1378` |
| P3 | B7/F4 specialization | primary Bicubic/Spline16/L2 | fixed loop | current Metal + descale B7 |
| P3 | B3/F2 specialization | Bilinear/L1 | fixed loop | current Metal + descale B3 |
| P3 | B11/F6 specialization | Spline36/L3 | fixed loop | current Metal tests/evidence |
| P3 | B15/F8 specialization | Spline64/L4 | fixed loop | current Metal tests/evidence |
| P4 | allocation/capacity reuse | repeated calls avoid working alloc | host backend | Metal retained buffers |
| P4 | source/plan async transfer | overlap only when profiler proves | host backend | backend-specific |

Generic 必须先覆盖全部 `half_bandwidth<=15`、`forward_width<=16` 的合法 plan。Specialization 只优化，不扩大 capability，也不能改变 result bits 相对 generic path。

### 11.2 Launch 规则

- Inverse：一个 thread 沿 destination axis 串行完成 `A^T b + forward solve + backward solve`。
- Image H：vector 为 source row；image V：vector 为 source column。
- Matrix inverse/forward：vector 为 intermediate/native 的独立 row/column。
- Metric：固定 256 threads，`groups_per_candidate` 默认 8。
- 同一 triangular vector 不做 parallel scan/PCR MVP。
- CPU merge 保持 candidate 和 group 顺序。

只有 profiler 证明 dependency chain 是主瓶颈、且现有模型无法达门槛时，才评估 PCR/scan；任何算法替换都重新跑完整 conformance 和 valley matrix。

### 11.3 优化顺序

在每层 correctness green 后按以下顺序：

1. B7/F4 fixed path。
2. B3/F2 fixed path。
3. B11/F6、B15/F8 fixed path。
4. CUDA 按 axis/stage/shape 编译期专用化，以及 candidate x vector 二维 grid；不要按滤镜名称重复生成相同 shape。
5. grow-to-fit source/workspace/partial capacity reuse。
6. source 每 call 一次 upload、partial-only readback，plan packing/upload 分段遥测。
7. CUDA source/plan/workspace 的合并访问、对齐和只读缓存；horizontal source transpose/repack 只在摊销后获益时保留。
8. CUDA 可证明不需要跨 block barrier 的 kernel fusion，优先评估同一 vector owner 的 matrix inverse + first forward。
9. CUDA pinned staging、真正异步的 HtoD/DtoH 和 copy/compute overlap。
10. 重复 fixed-recipe 的 CUDA Graph；只有 timeline 显示 launch/host overhead 时保留。
11. CUDA 多 stream，只在 timeline 证明 overlap 后保留。
12. `sm_80+` async copy、`sm_90a`/后续 architecture-specific TMA，只为有 shared-memory reuse 的 tile 增加独立变体。
13. metric reduction 的显式 warp-shuffle binary tree；必须保持第 6.5 节相同 pair/order。
14. Generic register pressure、occupancy、spill、memory coalescing、cache 和 barrier profile；再调 tile size、groups、inverse threads、`__launch_bounds__`。
15. inline PTX，仅处理第 12.7 节已证明的编译器 codegen 缺口，并通过第 16.6 节逐 SM A/B gate。
16. Vulkan device-local arena + staging reuse。
17. Vulkan pipeline cache，key 包含 shader hash、device、driver。
18. p>1，仅在产品需求和新数值契约明确后。

### 11.4 GPU 数学路径：仅一条极致优化生产 artifact

本 handover **明确废除** 下列 GPU 设计（若代码树中已出现，必须收敛为单路径，不得继续并列交付）：

- `math_mode=strict` / `--fmad=false` 参考通路，以及任何“为字面 SASS 无 `FFMA|DFMA|HFMA` 而 `-Xptxas=-O0`”的实现；
- `math_mode=fast-math` / 独立 `--use_fast_math` 产品或测试通路；
- 运行时在多套 fatbin/SPIR-V 之间切换数学模式的 API（`CudaMathMode`、`VulkanMathMode`、`--cuda-math-mode` 等）。

**唯一允许的 GPU 数学配置** 是极致优化生产路径：

| 目标 | 要求 |
| --- | --- |
| 输出精度 | 第 15.4 节 tolerance / valley / order；不以 CPU 中间 Float32 bits 为金标准 |
| 性能 | 正常 ptxas/驱动优化；FMA、指令调度、寄存器分配全部允许，只要通过正确性门槛 |
| PTX/SASS | 用来找坏 codegen、spill、访存与指令选择，并把优化推到极限；**不是** 用来证明“没有 FMA” |
| 交付物 | 每个 backend **一个** 主 fatbin / 主 SPIR-V 集合；profile 构建可加 `-lineinfo`，但数学语义与 release 相同 |

仍禁止：`half2` / Float16 coefficient storage、hardware sampler interpolation 替代 `AxisPlan` 权重、为刷分而改 threshold/crop/candidate order。

## 12. CUDA 后端要求

### 12.1 Build artifact

新增 `GETNATIVE_ENABLE_CUDA`，默认 `OFF`。启用时需要 build-time CUDA Toolkit/nvcc，关闭时不查找 CUDA Toolkit。

推荐用 custom command 生成 **单一** 生产 fatbin，而不是让 host target 自动链接 CUDA runtime：

```text
nvcc --fatbin getnative_cuda.cu
  -O3
  -gencode arch=compute_X,code=sm_X
  -gencode arch=compute_Y,code=[sm_Y,compute_Y]
```

架构列表由 Windows CI/目标 GPU 决定，不在 handover 中猜测。fatbin 至少包含每个已验证目标的 native cubin 和一个不依赖 architecture-specific feature 的 PTX fallback。`sm_XXa`/`compute_XXa` 之类 architecture-specific feature 不得成为通用 fallback；对应优化只放入匹配的 native cubin，其他 GPU 回到 generic 优化 kernel。使用现有 `engine/cmake/embed_binary.cmake` 嵌入 binary。

要求：

- 不链接 cudart。
- 不链接 CUDA Driver import library。
- 主 executable 的 import table 无 `nvcuda.dll`、`cudart64_*.dll`。
- **只构建并嵌入一套** 生产 fatbin（可含 generic + specialized + 经批准的 architecture/PTX variants 的 **kernel 变体**，但不是多套 math mode）。
- **允许并鼓励** FP32 FMA 与正常 ptxas 优化。默认 **不要** 使用 `--fmad=false`，**不要** 使用 `-Xptxas=-O0` 作为生产配置。
- 不需要、也不允许并行维护 `strict` / `relaxed-fma` / `fast-math` 三套 fatbin 或运行时 math-mode 选择。
- 源码可用普通 `float` 乘加（或等价可收缩写法），让编译器生成高效 SASS；不得为“语义可解析”强制全程 `__fmul_rn`+`__fadd_rn` 再另开一条 FMA 档。
- profile build 增加 `-lineinfo -Xptxas=-v`，保存每个 kernel 的 registers、shared/local memory 和 spill 输出；release 与 profile 的优化级别与数学配置相同（仅调试信息不同）。
- 保存 nvcc 生成的 PTX、每个 native cubin 和最终 fatbin hash。PTX 是虚拟 ISA，不能作为最终指令证明；完成判断看每个目标 cubin 的 SASS **是否达到预期优化效果**（吞吐、spill、访存），而不是是否“零 FMA”。
- Driver API 的 PTX fallback 使用 module JIT；需要 JIT 诊断时通过 `cuModuleLoadDataEx` 保存 info/error log。产品不链接 `nvptxcompiler_static.lib`，也不在运行时引入 CUDA Toolkit/PTX Compiler API。

### 12.2 Driver API 动态加载

Host 仅用 `LoadLibraryW(L"nvcuda.dll")` 和 `GetProcAddress`。最小 function table 覆盖：

- init/device enumeration/name/UUID/attributes/driver version；
- context create/destroy/current guard；
- module load/load-data-with-JIT-log/unload/function lookup；
- stream create/destroy/synchronize，event create/record/elapsed/destroy；
- device alloc/free、pinned host alloc/free；
- HtoD/DtoH async copy；
- kernel launch；
- error name/string。

M5A 启用 CUDA Graph 时再增加可选 Driver API function table：stream capture begin/end、graph instantiate/update/launch、graph-exec/node parameter update 和 graph/exec destroy。缺少这些可选 symbols 只让 Graph variant unavailable；不能让 generic CUDA backend 初始化失败，也不能改为链接 cudart。

注意 `_v2` symbol 和 Driver API version。每个 resolve 失败要报告确切 symbol，不得解引用 null function pointer。

无 DLL、`cuInit` 失败、无 device、不支持目标 module、context/module/function 创建失败必须分别形成稳定 reason。engine 和 GUI 仍应启动。

### 12.3 生命周期和提交

- 一个 `CudaAnalysisEngine` public call 可先串行化，与 Metal 一致。
- 每次 call 显式保证正确 context current。
- source、workspace、partials 使用 grow-to-fit capacity；总 retained bytes 有配置上限。
- plan tile upload 可先朴素实现，之后按 profiler 决定 arena/ring。
- stop 请求后停止提交新 tile。
- 已提交 stream work 必须 synchronize/drain 后才返回失败或复用 buffer。
- CUDA error 保留 API 名和 error string。
- 仅复制 partials 回 host；不回传完整 reconstructed frame。

### 12.4 CUDA 验证工具

- Nsight Systems：CPU planning、upload、launch、sync、tile timeline。
- Nsight Compute：先用 SpeedOfLight/LaunchStats/Occupancy/MemoryWorkloadAnalysis/SourceCounters 判断瓶颈，再看 kernel duration、register、spill、memory transaction、cache、branch、barrier 和 warp stall。高 occupancy 不是单独目标。
- Compute Sanitizer：`memcheck`、`racecheck`、`initcheck`、`synccheck` 中适用项。
- `cuobjdump`：确认 fatbin 中的 PTX/cubin 架构、kernel symbols、resource usage 和 SASS。
- `nvdisasm`：对逐目标 cubin 检查 control flow、source/PTX line mapping、register live range 和最终指令。SASS 只作检查输出，不是本项目的源码层。

### 12.5 对底层优化路线的项目化结论

给定分析的主方向正确，但必须按 GetNative 当前实现收紧：

| 路线 | 本 handover 决策 | GetNative 约束 |
| --- | --- | --- |
| CUDA C++ template/specialization | 必做 | 按 `AxisShape`、axis 和 stage 专用化；Bilinear/Lanczos1 同属 B3/F2，不为滤镜名重复 kernel |
| CUDA intrinsics / libcu++ async primitives | 优先于 PTX | warp shuffle、vector load/store、`cuda::memcpy_async`/pipeline、cooperative groups 必须有实际数据流收益且保持第 6.5 节循环/归约顺序 |
| SASS inspection | 必做 | 每个 native SM 都看最终 cubin；用 SASS 证明优化到位（调度、spill、访存、是否生成预期 FMA/向量访存），不能从 PTX 行数推断性能 |
| 局部 inline PTX | 条件必做 | 只有编译器持续不能生成目标指令且第 16.6 节 A/B 通过时保留；必须有 CUDA C++ fallback |
| 独立手写 `.ptx` kernel | 例外路径 | 仅当局部 wrapper 仍不足且热点已稳定；需要独立 ABI、ptxas、SASS 和每个 SM conformance |
| 直接写/patch SASS | 不属于支持路径 | 不使用 maxas、TuringAs、CuAssembler 或其他非官方 assembler/patcher 生成 shipping artifact |
| `half2`、Float16 coefficient、texture interpolation | 禁止 | Float16 已有 valley 漂移；硬件插值不消费 CPU `AxisPlan` 的准确权重/顺序 |
| FP32 FMA | **允许且优先** | 单一生产路径应利用 FMA；禁止再拆 strict/fast-math 数学档 |
| 多 math-mode fatbin | **禁止** | 不维护 strict / relaxed-fma / fast-math 并行 artifact |
| `-Xptxas=-O0` 生产构建 | **禁止** | 不得为 SASS 扫描或“语义证明”关闭优化 |
| PTX Compiler API | shipping 不采用 | 当前 runtime 只动态依赖 Driver API；offline nvcc/ptxas + embedded fatbin 已覆盖目标，避免新增 Toolkit static-library 依赖和自管 JIT cache |

“模板实例化 Lanczos1-8”需要改写为 shape specialization。滤镜名称只影响 CPU planner 生成的 coefficients；GPU 看到的是 `half_bandwidth`、`forward_width` 和已打包的 Float32 arrays。首批只保留当前 Metal 已有证据的 B3/F2、B7/F4、B11/F6、B15/F8 加 generic。B19/F10 到 B31/F16 若 profiler 证明 generic loop/control 是实际瓶颈，再逐 shape 增加；每个实例都要计入 cubin size 和 instruction-cache 影响。

`prmt`/byte permutation 示例也不直接适用于这些 Float32 solver。不要为了展示 inline PTX 引入与 profile 无关的指令。

### 12.6 CUDA kernel/dataflow 优化方向

#### 12.6.1 Dispatch 和专用化

- 将 flat `gid / maximum_vector_count` 改为可比较的二维 launch 变体：一个 grid dimension 表示 candidate，另一个表示独立 vector。这样同一 block 的 descriptor/weights 可保持 uniform，减少整数除法、candidate boundary divergence 和重复地址计算。
- 用共同的 device template 定义数学顺序，再实例化 `Axis::horizontal/vertical`、image/matrix/metric stage 和 B3/B7/B11/B15/generic；不得复制并逐渐分叉数学实现。
- `if constexpr`、固定 loop bound 和审计过的 unroll 优先于 inline PTX。Generic 必须始终保留为 correctness oracle 和未知合法 shape fallback。
- 使用 `const`/`__restrict__`、预计算 base pointer/stride、32-bit validated offsets，减少 inner-loop 64-bit address arithmetic。任何 host-side layout 改动都先通过 `gpu_batch_test` 的 byte/offset contract。

#### 12.6.2 Memory layout 和 reuse

- Vertical image/matrix path 先验证 warp 相邻 lane 的 source/workspace load 已合并；若已 coalesced，不要为使用 shared memory 而额外 copy。
- Horizontal image inverse 的相邻 vector 访问通常跨 row stride。评估 source 一次性 tiled transpose/repack 或同时保留 row-major/column-major source；成本必须在同一 call 的 candidate 数和 fixed-recipe 多帧中摊销，且计入 working set/upload/total wall。
- plan coefficients 对同一 candidate 的 vectors 是只读共享数据。先观察 L1/L2 hit、uniform load 和 long-scoreboard；只有 cache 不能满足时，才把当前 block 实际复用的小窗口 staged 到 shared memory。
- `float2/float4` load/store 只在 host packer 保证 8/16-byte alignment、row/arena offset 和 tail 后使用。不得对未验证的 crop/stride/arena offset 直接 reinterpret cast；vector load 不能改变 Float32 运算顺序。
- Hardware texture interpolation 禁止。只读/texture cache 若仅用于 point load，也必须证明比普通 global load 更快，并保持完全相同的 values 和地址边界。

#### 12.6.3 Fusion、reduction 和 launch overhead

- 当前 final forward reconstruction 已与 abs/threshold/metric reduction 融合，不要重复声称这是待完成优化。
- 两轴路径优先评估“同一 thread 已拥有完整 vector”的 matrix inverse + 同轴 first forward 融合；它可减少 native intermediate 的一次 global write/read。若需要跨 block/grid barrier、cooperative launch 或改变 B7/generic 次序，则保持分离 kernel。
- 256-thread metric reduction 可在 shared-memory 的 128/64/32 层后，用显式 `__shfl_down_sync` 完成 16/8/4/2/1；pairing 和 add order 必须与第 6.5 节 binary tree 相同。不要替换成顺序未被契约固定的 aggregate reduce intrinsic。
- fixed-recipe/多帧在 buffer address、tile signature 和 launch topology 稳定后评估 CUDA Graph capture/update。只有 Nsight Systems 证明 launch/host overhead 可见且 graph 的 update/recapture 成本可摊销时保留。
- persistent kernel、cooperative grid synchronization 和跨 candidate work queue 是最后一层实验；它们不得削弱 stop/drain、workspace ceiling 或稳定结果顺序。

#### 12.6.4 Architecture-specific data movement

- `sm_80+` 可评估 CUDA pipeline/`cuda::memcpy_async` 对 global -> shared 的硬件 async copy；前提是该 shared tile 被重复消费并能与计算重叠。单次读取数据不应为了 `cp.async` 多走 shared memory。
- Hopper `sm_90a`/后续支持 TMA 的目标可评估 1D/2D tile transfer 和 double buffering。TMA variant 单独编译、单独 dispatch、单独 benchmark；generic cubin/PTX fallback 不含 `a` feature。
- `__launch_bounds__` 和 `--maxrregcount` 都是 trade-off，不是越低越好。只有 ptxas/occupancy/Nsight 同时证明 register limiter 或 spill 问题时按 kernel、按 SM 调整；禁止全 artifact 统一硬压 register count。
- CUDA Tile 不是 M4/M5 依赖。当前 workload 是有序 Float32 banded solve 和 reduction，不是 dense tensor-core tile；只有目标 Toolkit 上的独立原型证明它改善同一生产 kernel 的数据移动或代码生成时再立项。

### 12.7 inline PTX 准入和实现契约

inline PTX 是最后的 codegen 修正层，不是完整 kernel 的首选语言。每个候选必须遵守：

1. 先有同一 binary 内可 forced 的纯 CUDA C++ 生产 baseline（单一数学路径）；baseline 已通过第 15.3/15.4 节并有 Nsight/SASS 证据。
2. 写一份候选记录：目标 kernel/SM、当前 CUDA C++/PTX/SASS、具体坏 codegen、预期替换指令、受影响数据和第 16.6 节成功门槛。
3. inline asm 限制在一个 `__device__ __forceinline__` wrapper 的一个语义操作；放入 `cuda_ptx_intrinsics.cuh`。调用点仍保留 C++ fallback，使用 `__CUDA_ARCH__` 和 PTX ISA/SM capability guard。
4. operand constraint 与 C++ scalar size/type 严格匹配；pointer 先确认 generic/global/shared address space。局部 PTX register 放在 `{}` scope，避免 wrapper 多次 inline 的 namespace collision。
5. 有副作用或不可移动的操作使用 `asm volatile`；影响未列出内存的 load/store/barrier 使用正确的 `"memory"` clobber。不要机械地给纯寄存器表达式加 clobber而阻断编译器调度。
6. nvcc host front-end 不解析 PTX template string；语法/type错误可能到 ptxas 才暴露。每个 native `sm_XX` 都必须实际经过 ptxas；每个保留的 `compute_XX` PTX 必须至少在兼容真实 device 上 forced Driver JIT，不能只让一个架构通过。
7. PTX 指令仍由 ptxas 做 instruction selection、register allocation、scheduling 和可能的多指令 lowering。最终验收以逐 SM SASS、resource usage、Nsight 和 wall time 为准，不以“PTX 看起来更短”为准。

优先候选仅包括：

- 编译器无法稳定表达的 cache-policy/eviction-hint load，且 Nsight 已证明对应 cache miss/scoreboard 是瓶颈；
- 没有等价 CUDA intrinsic 的目标 SM 新指令；
- 可减少已在 SASS 中确认的多余 address conversion、packed integer/bit 操作；
- 需要精确定义且 CUDA primitive 无法表达的 memory ordering；
- 在已证明 hotspot 上，用更紧的 load/store 或数据搬运指令把吞吐推到极限（仍须通过第 15.4/16.6 节）。

以下不准作为首批 inline PTX：

- `A^T b`、forward/backward solve 或 reconstruction 的完整手写 PTX（先把 C++ specialization/layout 做满）；
- approximate math 改变第 15.4 节输出门槛、Float16/`half2`；
- 已能由 `__shfl_*_sync`、cooperative groups、`cuda::memcpy_async` 等官方接口产生同等 SASS 的操作；
- 为追求特定 SASS 排列而塞入无语义依赖的 barrier、volatile load 或 dummy instruction；
- undocumented opcode、raw SASS encoding 或 binary patch；
- 仅为“去掉 FMA”或“制造可解析舍入语义”而写的 PTX。

独立手写 `.ptx` kernel 需要比 inline wrapper 更高的 gate：固定 `.version/.target/.address_size`、导出参数 ABI test、逐目标 ptxas、Driver module-load failure test、完整 CUDA C++ fallback 和逐 SM benchmark。没有这些证据时不创建 `.ptx` source。

### 12.8 cubin/PTX 分发、选择和可维护性

- 每个 Windows release 记录 Toolkit、nvcc、ptxas 和 driver version；不同 compiler/driver 可能从相同 PTX 生成不同 SASS，因此优化批准表必须带 toolchain 与 SM。
- fatbin 放入已验证 GPU 的 native cubin，另放一个在最低 driver/device policy 上可用的最高通用 virtual architecture 的 generic 生产 PTX，以保留尽可能新的 PTX codegen 和未来 GPU JIT。用 `CUDA_FORCE_PTX_JIT=1` 在真实设备验证 fallback；测试后清除环境变量。
- architecture-specific `sm_XXa` kernel 只能在匹配 device 上选中。runtime provenance 至少记录 device UUID、compute capability、kernel variant、native cubin/PTX-JIT 路径、artifact hash 和批准 benchmark id。
- 如果同一 fatbin 含 baseline、specialized、async/TMA 和 PTX-optimized kernels，host 必须按稳定 function table/variant id 选择；缺 symbol 或 variant 未获该 SM benchmark 批准时回到 generic 生产 kernel。**不得** 用 math-mode 切换替代 variant 选择，也不得存在第二套数学 fatbin。
- 不以删除 PTX、去符号或混淆 kernel 名作为本任务的“性能/保密优化”。cubin 仍可反汇编；删除 PTX 会失去 forward-compatible fallback。若产品以后优先保护实现，应单独做分发 ADR，而不是由 Windows backend agent 擅自决定。
- 直接 SASS patch 没有进入仓库、CI 或 package 的批准路径。若所有受支持层完成后仍需要最后几个百分点，先提交独立 R&D 结果、固定 GPU/driver/toolkit 商业收益和维护预算，再由项目 owner 决定是否另立非 portable artifact。

### 12.9 NVIDIA 官方参考边界

以下链接在 2026-07-31 核对为 CUDA 13.3 文档；Windows agent 必须按实际安装 Toolkit 版本使用对应 archive，并在 handback 记录差异：

- [NVCC compiler driver](https://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/index.html)：CUDA C++ -> PTX/cubin/fatbin、`-gencode`、`--fmad`、`--maxrregcount`。
- [Using Inline PTX Assembly](https://docs.nvidia.com/cuda/inline-ptx-assembly/index.html)：operand constraints、namespace/memory-space、`volatile`/`memory` clobber 和 ptxas error boundary。
- [CUDA Binary Utilities](https://docs.nvidia.com/cuda/cuda-binary-utilities/index.html)：`cuobjdump`、`nvdisasm`、resource usage、control flow 和 register live range。
- [Nsight Compute Profiling Guide](https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html)：SpeedOfLight、MemoryWorkloadAnalysis、Occupancy、SourceCounters 和 stall 解释。
- [Ampere Tuning Guide](https://docs.nvidia.com/cuda/ampere-tuning-guide/index.html)：global -> shared async copy、pipeline 和 occupancy/resource limits。
- [Hopper Tuning Guide](https://docs.nvidia.com/cuda/hopper-tuning-guide/index.html)：TMA、thread-block cluster 和 architecture-specific resource trade-off。
- [Blackwell Compatibility Guide](https://docs.nvidia.com/cuda/blackwell-compatibility-guide/index.html)：native cubin compatibility、forward-compatible PTX 和 `CUDA_FORCE_PTX_JIT` 验证。
- [PTX Compiler APIs](https://docs.nvidia.com/cuda/ptx-compiler-api/index.html)：只作为为何本项目不引入 runtime compiler/static library 的决策参考。

## 13. Vulkan Compute 后端要求

### 13.1 Build artifact

新增 `GETNATIVE_ENABLE_VULKAN`，默认 `OFF`。启用时允许依赖 build-time Vulkan-Headers 与 `glslc`/`spirv-val`；关闭时不查找 Vulkan SDK。

每个 shader 离线编译为 SPIR-V、运行 `spirv-val`、再用 `embed_binary.cmake` 嵌入。runtime 不调用 shader compiler，不依赖 Vulkan SDK 环境变量。

建议四份 shader，通过 specialization constants 产生 stage shape：

- inverse image；
- inverse matrix；
- forward matrix；
- metric，含 normal/horizontal-first 变体。

Shape constants 至少表达 fixed half-bandwidth、fixed forward-width 和 B7 backward order。必须产生 B3/B7/B11/B15/generic pipeline。

### 13.2 Loader 和 device selection

Host 编译时定义 `VK_NO_PROTOTYPES`，runtime 用：

1. `LoadLibraryW(L"vulkan-1.dll")`。
2. resolve `vkGetInstanceProcAddr`。
3. 通过它加载 global/instance/device functions。
4. 枚举 physical device 和 compute queue family。
5. 用 device UUID、PCI 信息和 name 建立稳定 selector；ordinal 只能作为用户输入便利，不是持久 identity。

MVP 目标 Vulkan 1.2。至少要求：

- compute queue；
- local size 256；
- 40-byte push constants；
- inverse stage 所需的 9 个 storage buffer binding；
- storage buffer range、allocation 和 heap 能容纳一个 tile；
- Float32 compute 能力与实际 conformance 通过。

不要求 subgroup、descriptor indexing、buffer device address、timeline semaphore 或 shader Int64。

### 13.3 单一优化 shader 路径

- **只交付一套** 生产 SPIR-V / pipeline 数学配置；不实现 `strict`（`NoContraction` 强制）与 `fast-math`/`RelaxedPrecision` 并行通路。
- 允许编译器与驱动在 Float32 上做 contraction/FMA，只要第 15.4 节输出门槛通过。
- 不为“语义可解析”强制关键乘加带 `NoContraction`；也不为刷分默认打开未经验证的 `RelaxedPrecision` 产品档。
- 不使用 subgroup reduction 改变第 6.5 节归约顺序。
- 查询并记录 Float32 float-controls properties，作为 provenance，不作为多 mode 切换面。
- capability 不能仅因 loader 存在而报告可执行 backend。
- NVIDIA 上通过不代表 AMD；至少一台目标 AMD GPU 才能形成 AMD 支持证据。

### 13.4 Descriptor、barrier 和 memory

MVP 可保持独立 buffers：

```text
source
descriptors
transpose offsets / indices / weights
lower / upper / inverse diagonal
forward left / weights
workspace
partials
```

阶段：

```text
staging upload
inverse image
compute barrier
[inverse matrix]
compute barrier
[forward matrix]
compute barrier
metric
compute-to-transfer barrier
partial copy to host staging
fence wait
CPU double merge
```

Discrete GPU 默认 device-local + host-visible staging。UMA 直接 host-visible compute buffer 只能经 benchmark 保留。记录 memory type、heap、allocation bytes 和 peak。

不得假设 `HOST_VISIBLE` 同时具备 `HOST_COHERENT`：

- CPU 写 staging 后，在 transfer/compute 使用前调用 `vkFlushMappedMemoryRanges()`；
- GPU 写 readback staging 后，等待对应 fence，再调用 `vkInvalidateMappedMemoryRanges()`，之后才读 partials；
- non-coherent range 按 `nonCoherentAtomSize` 向外对齐并限制在 allocation 范围内；
- mapping lifetime、flush/invalidate range 和失败清理必须可测试；
- 单元测试 coherent/non-coherent range 计算，并在有 non-coherent memory type 的真实设备上覆盖该路径；若实现选择不支持，初始化时必须明确拒绝并给稳定 reason，不能静默按 coherent 处理。

`VK_ERROR_DEVICE_LOST`、loader missing、no compute queue、pipeline/layout failure 分别报告。异常和 cancel 都要等待/处理已提交 fence，再按正确 parent-child 顺序销毁对象。

### 13.5 Vulkan 验证工具

- Vulkan validation layers：Debug/CI correctness，不进入 release runtime 依赖。
- RenderDoc / Nsight Graphics：descriptor、barrier、dispatch capture。
- Nsight Systems：Vulkan API 和 CPU/GPU timeline。
- Radeon GPU Profiler：只在真实 AMD 机器使用。
- `spirv-val`、`spirv-dis`：离线 artifact 校验与反汇编证据（用于优化与正确性排查，不是 no-contraction 门禁）。

## 14. Capability、CLI、Tauri 和 React 集成

### 14.1 三个状态不能合并

- `compiled=true`：host adapter 与 embedded kernel/shader 已进入 binary。
- `device_available=true`：runtime 动态加载成功，兼容 device/context/module 或 device/pipeline 可初始化。
- `analysis_command_available=true`：真实 CLI/worker analyze path 已把 job 接到该 backend。

Library backend 完成但 analyze command 未实现时，前两个可以 true，第三个仍 false。

### 14.2 Backend list

最终 capability list 在所有平台稳定包含：

```text
cpu, metal, cuda, vulkan
```

输出顺序可保持上述顺序，但 validator 必须按 id map：

- 每个 id 唯一；
- CPU 必须存在；
- 可选 backend disabled 时仍给 `compiled=false` stub；
- 未编译时 axes 空、p/shape null、reason `not compiled`；
- 已编译但无 device 时仍报告静态 axes/p/shape，并给具体 reason；
- 不再按 `backends[0]`、`[1]`、`[2]` 解释。

当前 schema v2 consumer 明确要求 backend 数组恰好为 `cpu, metal,cuda`；旧 consumer 收到额外 Vulkan entry 会拒绝。因此本 handover 规定：扩展为四个 backend 时把 capability schema 升到 v3，并让 engine、Rust validator、TypeScript types/tests 和 package 锁步升级。v3 validator 按 id map 校验，不依赖数组 index；不得向现有 v2 consumer 静默发送四项数组。只有项目另行建立并记录“engine/app 永远锁步、v2 不承诺向后兼容”的正式 schema policy，才可重新评估是否保留版本号。

### 14.3 CPU ISA、math mode 和 provenance

Capability schema v3 的 CPU entry 除现有 backend 三态外，必须能表达：

- `compiled_isa`：本 binary 实际包含的 `scalar/sse2/avx2/avx512`；
- `available_isa`：当前 CPUID + XCR0 + compiled mask 共同允许的 tiers；
- `selected_isa`：本 process 在 `auto` 下最终使用的 tier；
- `math_modes` / `selected_math_mode`：CPU 侧固定为 **`production`**（允许 FMA 的单档）；**不要** 为 GPU 复制多档 math mode。
- `selection_reason`：例如 `widest available production tier`、`avx512 not benchmark-approved`、`forced by test` 或 stable fallback reason。

字段名可在 v3 schema 设计时调整，但上述信息不能丢失。普通 capability 不必暴露所有 raw CPUID bits；benchmark/debug handback 必须包含 raw feature snapshot 和 XCR0。CPU `compiled=true` 不代表 `analysis_command_available=true`，仍遵守第 14.1 节三态。

Recipe 对 CPU 锁定的是 CPU `math_mode=production` 与滤镜/几何，不是某台机器的 ISA。`auto` 在 job 创建时解析到具体 `selected_isa`，结果 provenance 至少保存 `backend=cpu`、ISA、`math_mode=production`、是否 forced、CPU signature、compiler flags 和 benchmark source id。相同 Recipe 在另一台 CPU 上可以选择不同 ISA，但 CPU math mode 保持 production。

CUDA/Vulkan capability **不要** 暴露 `strict|relaxed-fma|fast-math` 选择面。CUDA debug/provenance 至少保存：device UUID、compute capability、driver/toolkit、kernel variant、native cubin 或 PTX-JIT 路径、**单一** fatbin hash、nvcc/ptxas 版本与生产优化 flags、benchmark approval id。普通用户不需要选择 PTX/SASS；forced **kernel variant**（generic/specialized/…）只开放给 test/benchmark，不是 math mode。

### 14.4 需要修改的现有文件

- `engine/CMakeLists.txt`：x86 ISA object-library/逐文件 flags、独立 CUDA/Vulkan options、artifact build/embed、可组合 link targets、backend tests/benchmarks。
- `engine/src/backend/cpu/inverse_columns.hpp`、`inverse_columns.cpp`：从 coarse `required_simd` 扩展为可测试的 ISA/math dispatch，同时保持现有 automatic/scalar 调用兼容。
- `engine/src/backend/cpu/cpu_analysis.cpp`：把 P1 row kernels 接到相同 dispatch snapshot，metric lane 提交顺序不变，允许 FMA。
- `engine/tests/cpu_column_simd_test.cpp`：扩为 SSE2/AVX2/AVX-512 forced conformance、feature matrix、tail/alignment 和 **tolerance**（非 bit-identical）tests。
- `engine/src/cli/main.cpp:85-145`：CUDA/Vulkan capability probe；始终保留 disabled stub。
- `engine/src/cli/main.cpp:194-208`：只有真实 analyze command 完成后才扩命令表。
- `app/src-tauri/src/lib.rs:140-257`：backend id-map validation，接受 Vulkan 和真实 CUDA/Vulkan compiled/device 状态。
- `app/src-tauri/src/lib.rs:461-478`：测试 disabled/compiled/no-device/duplicate/missing/id-order cases。
- `app/package.json:8-9`：当前 engine build script 不转发 GPU options；增加 Windows preset 或明确 env/preset，不破坏 CPU-only/macOS build。
- `app/src/App.tsx:326-355`：现有 `BackendRow` 可直接展示 Vulkan；检查长 reason 和 device name。
- `engine/CMakeLists.txt:222-230`：capability regex 当前强制 CUDA false，改为按 build option 的期望。

### 14.5 Backend selection 和 fallback

- CPU `auto` 按第 10.7 节选择 strict ISA；用户显式 forced ISA 不可用时返回错误，不静默换低 tier。只有真正的 `auto` 才能逐级回退。
- 用户显式选 CUDA/Vulkan时，初始化或执行失败返回错误，不自动跑 CPU。
- `auto` 若以后加入，job 创建时必须解析为具体 backend 并写 provenance。
- “Retry on CPU”创建新 job，保留原 GPU failure。
- NVIDIA 机器上 CUDA/Vulkan 都可见、可独立选择、独立测量。
- GPU 不得静默切换到未批准的 kernel variant，也不得存在第二套数学 artifact 可被静默选中。

### 14.6 真实 analyze/front-end planner 的额外工作

只有以下全部存在后才能把 `commands.analyze` 改为 true：

1. versioned request/response schema；
2. 持久 engine worker；
3. job id、progress、cancel、error/result events；
4. source frame 以非 JSON bulk path 进入 engine；
5. engine-side session `AxisPlanCache` ownership；
6. backend selection/provenance；
7. Tauri child lifecycle 和 crash handling；
8. React candidate/job/result state；
9. end-to-end tests。

不要用一次性的 `Command::output()` 加一个长时间 `analyze` argv 命令冒充完整协议。

## 15. 测试矩阵

### 15.1 任何平台都必须通过

新增 `gpu_batch_test`：

- POD size/alignment/field offsets。
- H/V/both descriptor 内容和 workspace bases。
- B3/B7/B11/B15/generic 分类。
- exact input order、adjacent signature tiling。
- forward/transpose offset table 的 zero-origin、monotonic、bounds 和 exact-tail validation。
- forward row width/contiguous index validation，以及全部 weight/factor/diagonal finite validation。
- null/invalid plan、index、crop、threshold、overflow、workspace limit；malformed plan 在 allocation/upload/dispatch 前失败。
- deterministic partial merge。
- plan flatten 不修改 Float32 bits。

保留并通过：

- core/geometry/CPU SIMD tests；
- `getnative_axis_planner_tests`；
- CLI capability/geometry tests；
- optional descale/zimg upstream conformance；
- CUDA/Vulkan options OFF 时无 SDK/driver/loader 依赖。

### 15.2 x86 CPU SIMD conformance

扩展现有 `getnative_cpu_column_simd_tests`，让每个已编译且 available 的 production tier 都可被单独 forced；不能只比较 `automatic` 和 scalar。至少覆盖：

1. `scalar`、`sse2`、`avx2`、`avx512` forced dispatch；unsupported/uncompiled tier 在进入 kernel 前稳定拒绝。
2. H、V、both 和两种 forward order；Bilinear、任意有限 B/C Bicubic、Spline16/36/64、Lanczos1-15，所有实际 CPU `half_bandwidth=1..29`、`forward_width=1..30`。
3. `half_bandwidth==1`、`==3` 和 generic 的 exact loop order；B7 near-to-far backward 特例。
4. width/column count 至少含 `0,1,2,3,4,5,7,8,9,15,16,17,31,32,33,63`，覆盖每个 lane width 的短向量、整块和 tail。
5. base address 偏移 `0..lane_bytes-1` 的代表性 misalignment、non-contiguous input/output stride、crop 起点、row padding canary；无越界读写。
6. positive/negative zero、subnormal、最小 normal、有限大值和 threshold 的 `<,==,>` 邻域；library 不改变调用线程 MXCSR。
7. `p=1,2,3,4` 和至少一个 `p>4`，empty batch、multi-worker batch、candidate id/order、workspace reuse。
8. 每个 production tier 相对 forced scalar 的 inverse/workspace/metric 在 relative tolerance 内（默认约 `5e-4`），finite 且 order 正确；不要求 bit-identical。
9. feature detector 使用可注入 CPUID/XCR0 provider 覆盖 SSE2-only、AVX2 bit 但 OSXSAVE/XCR0 缺失、完整 AVX2、AVX512F 但 ZMM state 缺失、完整 AVX-512，以及 compiled mask 缺 tier。
10. baseline/SSE2/AVX2/AVX-512 object 的 disassembly 或保存证据：ISA 隔离；AVX2/AVX-512 应含 FMA；baseline/SSE2 无高阶 ISA 泄漏。

除 mock feature tests 外，还必须在至少一台无 AVX2 的 Windows x64 host/VM 和一台有 AVX2 的真实 host 运行 package；AVX-512 完成声明需要真实 AVX-512 host。CPUID masking/mock 不能替代真实指令执行证据。

### 15.3 每个真实 GPU backend 的共享 conformance

镜像当前 Metal test shape，不复制 Objective-C：

1. H、V、both。
2. horizontal-first 和 vertical-first both。
3. Bilinear、任意有限 B/C Bicubic、Spline16/36/64、Lanczos1-8。
4. actual `half_bandwidth=1..15`、`forward_width=1..16` 合法 path。
5. forced generic 与 automatic/specialized 逐 candidate bit-identical。
6. B3/B7/B11/B15 pipeline creation/dispatch telemetry。
7. mixed shapes 和交错 forward order 保持 id/order。
8. non-contiguous source stride。
9. empty candidates。
10. invalid source、crop、threshold、p!=1、null plan、shape over limit。
11. 单 candidate 超 workspace 和 tile 自适应缩小。
12. repeated calls 的 working-buffer reuse、grow、trim 和 ceiling。
13. cancel-before-submit、cancel-after-submit、drain 后立即复用。
14. submission count 等于 completion count。
15. peak host/device/total explicit working set。
16. 多 device enumeration 和稳定 selector。

CUDA 额外：

- no `nvcuda.dll`、no device、unsupported fatbin/PTX、missing symbol、context/module failure；
- test/benchmark-only forced `cpp-generic`、`cpp-specialized`、architecture-specific async/TMA 和 `inline-ptx` variant；未编译、SM 不匹配或未批准 variant 在 launch 前稳定拒绝；
- 每个优化 variant 与同一 artifact 的 `cpp-generic` 逐 candidate bit-identical，并通过相同 valley/order matrix；
- warp-shuffle reduction 与 shared-memory 256-thread binary tree 的 partial/result bits 一致；
- alignment/vector-load tail、horizontal transpose/repack、kernel fusion、CUDA Graph update/replay 各有启用/禁用 A/B correctness；
- 每个包含的 native cubin 在对应真实 SM 上执行 conformance；mock compute capability 不能替代实际指令执行；
- `CUDA_FORCE_PTX_JIT=1` 下完整 conformance，证明 generic 生产 PTX fallback 存在且可 JIT；architecture-specific `a` variant 不得污染该 fallback；
- inline PTX wrapper 的 capability guard、C++ fallback、每个目标 ptxas assembly 和 Compute Sanitizer；
- SASS/resource 证据证明生产路径使用正常优化（无 `-Xptxas=-O0`）、热点合理，ptxas 无未解释 spill；若 local memory 是显式设计，必须逐项说明而不能归因于 compiler spill。**不要求** SASS 零 `FFMA`/`HFMA`；出现 FMA 是预期优化结果。

Vulkan 额外：no `vulkan-1.dll`、no compute queue、insufficient limits、validation clean、device lost。

Vulkan memory 额外：coherent/non-coherent range-alignment 单元测试；真实 non-coherent memory path 若设备提供则验证 flush/invalidate + fence 顺序，否则验证明确 capability rejection。

### 15.4 GPU 数值门槛

每个 candidate：

```text
abs(gpu_metric - cpu_metric)
    <= max(1e-7, 5e-4 * abs(cpu_metric))
```

同时要求：

- metric finite；
- candidate 数、id、顺序完全一致；
- argmin index 与 CPU 距离不超过 1 个 search step；
- 产品完成前 top-k local valleys 与 CPU 保持一致；
- specialized 与 generic result bits 一致。

不得修改 CPU oracle、threshold/crop 或放宽 tolerance 来让 GPU 通过。

### 15.5 上游 conformance 的证明边界

`GETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON` 证明：

- transpose weights、factor 和 inverse output 与 descale `DESCALE_OPT_NONE` bit-conformant；
- forward row 与 pinned zimg bit-conformant。

它只证明 planner/CPU reference，不证明 CUDA/Vulkan runtime。

## 16. Benchmark、profiler 和默认启用

### 16.1 共享 case matrix

复用 `engine/bench/fixtures/metal_kernel_matrix.json` 的参数语义：

- 1920x1080；
- 1000 candidates；
- tile 32；
- 8 reduction groups；
- inverse threads 32；
- primary `bicubic-catrom@810`；
- named heights 362/540/720/810/846/864/900；
- Bilinear、两组 Bicubic、Spline16/36/64、Lanczos1-8；
- fractional scan `800..899.9`；
- crop 5、threshold 0.015、p=1。

`engine/bench/fixtures/6.2-1.png` 是 ignored/local-only fixture，只有 SHA-256 被跟踪。没有明确再分发权时不得复制到 Windows handoff。Windows agent 使用获授权的本地 fixture 或可再分发 synthetic fixture，并记录原始和 decoded Float32 hash。

另保留当前 fixed-recipe 的 prepared-once 多帧场景：一个 locked vertical Recipe，predecoded ring，逻辑 frame counts `1,2,10,100,1000`，CPU serial/frame-parallel 与 backend serial calls 交错测量。它用于 amortization、frame-level parallelism 和 plan-residency判断，不能替代上面的 1000-candidate batch matrix；Windows benchmark 可复用其 JSON 字段和 decision gate，不需要让 Metal-only source 在 Windows 编译。

### 16.2 x86 CPU benchmark

新增 `getnative_cpu_backend_benchmark`，使用与 GPU 相同的 source/candidate/metric 输入，并提供第 10.2 节 forced ISA surface。每个 case 预热后至少收集 21 个 raw samples，报告 median 和 MAD。必须分开记录：

- cold planner、warm session-cache planner；
- adjacent-column inverse P0；
- vertical reconstruction row 与 absolute-difference row P1；
- complete CPU execution；
- end-to-end；
- scalar/SSE2/AVX2/AVX-512 strict；
- selected ISA、selection reason、CPU vendor/family/model/stepping、CPUID/XCR0、core count、Windows power plan、compiler/version/flags。

除第 16.1 节 primary matrix，还加入宽度/列数 `3,5,7,8,9,15,16,17,31,32,33,63,1919,1920,1921`、misaligned base、padded stride、B3/B7/B11/B15 和 Lanczos8/15 generic。避免只用整齐的 1920 宽度隐藏 tail 成本。

自动选择使用第 10.7 节 5% gain/3% per-case regression gate。报告 `available` 与 `selected` 的差异，并特别保存 AVX-512 相对 AVX2 的 wall time、有效频率/clock 或可获得的等价 downclock 证据。没有稳定优势时不是实现失败，但必须保持自动选择 AVX2。

GPU 报告必须同次运行 `auto + strict` CPU，且把该 end-to-end median 用作 speedup denominator；旧 scalar 数字只能作历史对照。

### 16.3 必须区分的 GPU 计时

每个 backend 报告：

- cold planner、warm session-cache planner；
- plan pack；
- source upload；
- plan upload；
- inverse H/V；
- first forward；
- metric；
- partial readback；
- CPU merge；
- backend total wall；
- end-to-end total wall。

视频场景报告 cold first frame 加 2/5/10/100/1000 frame amortization。不要把 cache prewarm 当作零成本，也不要把 GPU-only kernel time当产品 speedup。

### 16.4 资源和正确性报告

- maximum metric error；
- valley distance/top-k；
- tile/groups/thread/workgroup；
- working/retained/queued-plan/total peak bytes；
- allocation count 和 reuse count；
- GPU、UUID/PCI、driver、API/toolkit/shader compiler；
- strict compile flags 和 artifact hashes；
- 每次 raw sample、median、MAD。

CUDA 每个 kernel variant 另报：

- launch count、grid/block、dynamic shared memory、registers/thread、spill stores/loads、local-memory bytes、theoretical/achieved occupancy；
- kernel duration、SM/DRAM/L1/L2 throughput、global load/store transactions、branch efficiency、barrier、long/short scoreboard 和主要 warp-stall reason；
- `cuobjdump --dump-resource-usage`、目标 cubin SASS、`nvdisasm` register live range 和 Nsight Compute report 路径；
- CUDA Graph instantiate/update/replay、async copy/TMA、transpose/repack 和 fusion 各自的额外内存、setup time、amortization break-even；
- native cubin 与 forced PTX-JIT 的 first-load/cold-JIT/warm-cache 时间，不能把 JIT cache 预热隐藏在 steady-state 数字中。

### 16.5 门槛

- x86 strict 自动选择只包含通过第 15.2 和 16.2 节 gate 的 tier；可用但未获 benchmark 批准的 AVX-512 保持 forced experimental。
- total explicit GPU working set `< 2 GiB`；
- strict numeric gates 全过；
- cancel latency不超过两次 tile duration；
- 同机、同输入、同 candidate，相对 `auto + strict` 最快正确 **CPU** end-to-end `>=3x` 才可考虑默认启用 GPU（CPU 分母仍是 strict ISA 路径；GPU 自身是单一优化路径）。

CUDA 和 Vulkan 分别 go/no-go。一个通过不能替另一个背书。

### 16.6 CUDA 低层优化保留门槛

每个 CUDA C++ layout/fusion/graph/architecture/inline-PTX 变体必须与同一 binary 中的前一层 baseline 做单变量 A/B。至少 21 个预热后的交错 raw samples，保持输入、tile、clock/power 状态、driver、toolkit、**同一生产数学配置** 和其他 kernel variants 不变。

inline PTX 或独立手写 PTX 只有同时满足以下条件才可进入自动选择：

- 第 15.3/15.4 节全部 green，输出与 `cpp-generic`/对应 C++ specialized path bit-identical（在同一生产数学配置下）；
- 目标 hotspot kernel median 改善至少 3%，end-to-end median 改善至少 1%；
- observed gain 大于 baseline/candidate 两者 relative MAD 较大值的 2 倍；
- primary matrix 没有 case 回退超过 2%，cold/JIT/multiframe/memory/cancel 没有实质回归；
- 无新增 compiler spill；register/shared-memory 增长有 occupancy 和 wall-time 证据；
- SASS 确认目标 codegen 缺口确实消失，且没有越界 wide load 或无收益的额外 barrier。**允许并期望** FP32 FMA；不得以“出现 FMA”否决优化。

CUDA C++ specialization、layout、fusion、Graph、async/TMA 等较高层优化至少满足同样的噪声显著性和无 correctness 回归；是否保留可依据其 end-to-end gain、复杂度和通用性单独记录。批准必须按 GPU 型号/device class、compute capability、driver/toolkit 和 artifact hash 建表，并保存验证机 UUID；不得把一个 `sm_XX` 的结论外推到另一代或同 SM 的明显不同资源档位。profile 未发现合理的 PTX codegen 缺口时记录 `NO_PTX_CANDIDATE`；有候选但没有达到门槛时记录 `NO_PTX_WINNER`。两者都比提交无收益的 asm 更完整。

## 17. CMake 和 packaging 约束

目标 options：

```cmake
GETNATIVE_ENABLE_METAL
GETNATIVE_ENABLE_X86_SIMD
GETNATIVE_ENABLE_X86_AVX512
GETNATIVE_ENABLE_CUDA
GETNATIVE_ENABLE_VULKAN
GETNATIVE_BUILD_UPSTREAM_CONFORMANCE
```

要求：

- `GETNATIVE_ENABLE_X86_SIMD` 在 x86/x64 默认 ON、其他架构 OFF；在 x86 关闭后只编译 scalar，AArch64 NEON 和 GPU options 不受影响。`GETNATIVE_ENABLE_X86_AVX512` 在 Windows x64 且 compiler 支持时默认 ON，只控制 AVX-512 object，不能改变 AVX2/SSE2 fallback；显式 ON 但 compiler 不支持时 configure 必须明确失败。
- x86 baseline/SSE2、AVX2 和 AVX-512 使用独立 object libraries 或逐 source compile options；高阶 `/arch`/`-m` flag 不得传播到 `getnative_core` 的其他 source 或 consumer。
- AVX2/AVX-512 object 编译存在不等于运行时 available；所有入口仍经过 CPUID/XCR0 dispatch，math mode 固定为 **production**。
- CUDA/Vulkan 默认 OFF。
- CPU-only configure 不寻找 CUDA/Vulkan SDK。
- CUDA-only、Vulkan-only、两者同时 ON 都能 build。
- Metal option 与 Windows options 独立，不写互斥 `if/else` link 逻辑。
- host C++ 继续 C++23。AVX2/AVX-512 production objects 使用 FMA（MSVC `/arch:AVX2|AVX512`，clang `-mfma`）；不得再为 CPU production 强制 `-ffp-contract=off` 以禁止 FMA。
- generated fatbin/SPIR-V 加入 configure/build dependency 和 benchmark source identity。
- engine install/bundle 包含 embedded artifacts，不依赖开发机路径。
- package 包含 `THIRD_PARTY_NOTICES.md`。

## 18. 实施里程碑

### M0: 接收和 Windows CPU baseline

交付：

- 保存 HEAD、dirty diff、文件 hash。
- Windows x64 scalar-forced CPU-only build/test/capabilities。
- 无 CUDA/Vulkan SDK 的 configure/startup。
- 确认现有 benchmark dirty file 未丢失。
- 保存当前 x86 `selected=scalar` 的 baseline benchmark、CPU signature 和 compiler flags。

完成标准：scalar、planner 和可运行的 upstream conformance green，baseline raw samples 可复现。失败时不开始 SIMD 或 GPU correctness。

### M1: x86 feature dispatch 和 SSE2 production fallback

交付：

- 可注入测试的 CPUID/XCR0 snapshot 与 compiled/available/selected 状态。
- baseline/SSE2 独立 object，`auto|scalar|sse2` forced surface。
- P0 adjacent-column SSE2 production，fixed B3/B7 + generic + scalar tail。
- 第 15.2 节 SSE2 tolerance、alignment/stride/tail、unsupported tier tests。
- baseline/SSE2 disassembly 和无 AVX Windows x64 package run。

完成标准：SSE2 相对 scalar 在 tolerance 内；无 AVX host 不触发 illegal instruction；scalar forced 仍可运行。

### M2: AVX2、AVX-512 和 x86 自动选择

交付：

- AVX2/AVX-512 production objects（含 FMA）和 runtime dispatch。
- P0 完整 conformance；P1 vertical reconstruction/absolute difference 只在 P0 green 后加入。
- 全 forced ISA matrix、tolerance correctness、disassembly（含 FMA 证据）和真实 AVX2/AVX-512 host evidence。
- `getnative_cpu_backend_benchmark`、raw samples、median/MAD、5%/3% selection gate。
- AVX-512 downclock 结论；未过门槛时 available 但自动选择 AVX2。

完成标准：所有 production tiers 相对 scalar 在 tolerance 内，`auto + production` 在每台验证 CPU 上选择有证据的最快 tier。CPU 任一 gate 失败时不开始 GPU correctness。

### M3: 共享 GPU contract

交付：

- private `gpu_batch` helper。
- POD/layout/packing/tiling/merge CPU tests。
- CUDA/Vulkan public header/API shells。
- CMake options OFF/ON dependency boundaries。
- backend capability probe/stub 的内部数据路径；公开 capability 输出保持当前 v2，直到 M8 与 app 一起迁移。

完成标准：无 GPU SDK 机器仍能 build/start；shared packing test 覆盖 H/V/both 和全部 shapes。

### M4: CUDA vertical generic 生产 MVP

交付：

- Driver loader、device/context/module/stream/memory RAII。
- embedded **单一** 生产 fatbin/PTX（允许 FMA、正常 ptxas 优化）。
- generic image inverse + generic metric。
- vertical p=1 batch、partial merge、cancel/drain。
- vertical full filter conformance 和 benchmark。

完成标准：真实 NVIDIA GPU 逐 candidate pass；无 driver VM 正常启动并报告 reason。

### M5: CUDA 完整生产 backend

交付：

- H、V、both 与两种 forward order。
- matrix inverse/forward、horizontal-first metric。
- B7、B3、B11、B15 optimization。
- mixed shape、buffer reuse/trim、multi-device、failure tests。
- Nsight/Compute Sanitizer evidence。

完成标准：完整 CUDA matrix、memory、cancel、artifact/dependency gates green；仍为单一数学路径。

### M5A: CUDA C++ / intrinsic 极限优化

交付：

- Nsight Systems/Compute baseline 和逐 kernel roofline/bottleneck 分类。
- axis/stage/shape compile-time variants、二维 candidate/vector grid 和 address simplification。
- coalescing/alignment/vector-load 证据；horizontal transpose/repack 的含 setup/amortization A/B。
- 可行的 matrix inverse + same-axis first-forward fusion，以及第 6.5 节顺序的 warp-shuffle reduction。
- fixed-recipe CUDA Graph；只有可复用 shared tile 才增加 `sm_80+` async-copy 或匹配硬件的 TMA variant。
- 每个优化的 forced fallback、correctness、resource、SASS、raw samples 和第 16.6 节保留决定。

完成标准：所有 selected variants 逐 SM 有 correctness/performance approval；未获益或回归的变体删除，generic 生产 kernel 始终可 forced。M5A 先于 inline PTX，但 Vulkan correctness 的 M6 不必等待没有依赖关系的 CUDA profile 实验。

### M5B: inline PTX 终局优化

交付：

- 对 M5A 后仍存在的 hotspot/codegen 缺口建立候选记录；不得为了使用 PTX 而虚构候选。
- 每个 wrapper 的 CUDA C++ fallback、SM/PTX guard、逐 target ptxas、SASS/resource diff 和 Compute Sanitizer。
- 同一 artifact forced A/B、21-sample raw data、逐 SM 第 16.6 节决定。
- native cubin + generic PTX-JIT fallback 验证，以及 direct-SASS/nonofficial-tooling exclusion 证明。

完成标准：达到门槛的 wrapper 才进入对应 device approval table；没有合理 codegen 缺口时提交 `NO_PTX_CANDIDATE`，有候选但未达门槛时提交 `NO_PTX_WINNER`，并以 M5A C++ 生产 variant 完成，不保留零收益 asm。

### M6: Vulkan vertical generic 生产 MVP

交付：

- dynamic loader、instance/device/queue RAII。
- embedded validated **单一** 生产 SPIR-V。
- descriptor/push constants/barriers/staging。
- generic vertical inverse + metric。
- no-loader/no-device tests 和真实 GPU conformance。

完成标准：至少一台真实 Windows Vulkan GPU pass；无 loader 环境正常启动。

### M7: Vulkan 完整生产 backend

交付：

- H、V、both 和两种 forward order。
- B7、B3、B11、B15 pipelines + generic。
- mixed shape、buffer reuse、device lost、multi-device tests。
- validation layer 和 capture evidence。

完成标准：完整 Vulkan matrix green；NVIDIA 结果只形成 NVIDIA Vulkan 证据，AMD 另测。

### M8: Capability 和 Windows package

交付：

- capability schema v3 的 engine/app 锁步迁移，包含 CPU ISA/math provenance；
- `cpu,metal,cuda,vulkan` id-map validation。
- compiled/device/command 三态 tests。
- CPU-only、CUDA-only、Vulkan-only、both packages。
- no-driver/no-loader packaged GUI startup。
- PE dependency scan。

完成标准：GUI 可展示真实 CUDA/Vulkan 状态；若 analyze 尚未接通，Analysis tab 仍禁用。

### M9: 可选的产品 analyze/front-end planner 集成

这是独立 scope。只有明确被分配时执行：

- persistent JSONL worker；
- session `AxisPlanCache`；
- backend job/provenance；
- Tauri progress/cancel；
- React analysis planner/results；
- end-to-end tests。

完成标准：真实 job 可运行/取消/重试，并且只有此时 command availability 变 true。

### M10: 性能和默认策略

交付 x86 ISA 自动选择表，以及每个 GPU backend 的 raw samples、profiler、memory、cancel、correctness 和 go/no-go。CUDA 还要交付逐 SM kernel-variant approval table、PTX-JIT/native 路径和所有 inline PTX 接受/拒绝记录。AVX-512 未过相对 AVX2 gate 时不自动选择；GPU 未达相对 `auto + strict` CPU 的 3x 时保持显式 experimental backend；不得降低 strict 门槛。

## 19. Windows 验证命令

### 19.1 当前代码的 CPU baseline

当前 `HEAD` 还没有 CUDA/Vulkan options，先运行：

```powershell
cmake -S engine -B build/engine-win-cpu -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DGETNATIVE_ENABLE_METAL=OFF `
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=OFF
cmake --build build/engine-win-cpu --parallel
ctest --test-dir build/engine-win-cpu --output-on-failure
build/engine-win-cpu/getnative-engine.exe capabilities
```

若 pinned upstream 已存在且 test-only C 编译兼容，再单独开 conformance build。不要修改 upstream source。

### 19.2 x86 SIMD 实现后的验证

实现 M1/M2 后提供等价的 deterministic test/benchmark CLI，并在每台 Windows 验证机运行：

```powershell
cmake -S engine -B build/engine-win-x86 -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DGETNATIVE_ENABLE_METAL=OFF `
  -DGETNATIVE_ENABLE_X86_SIMD=ON `
  -DGETNATIVE_ENABLE_X86_AVX512=ON `
  -DGETNATIVE_ENABLE_CUDA=OFF `
  -DGETNATIVE_ENABLE_VULKAN=OFF
cmake --build build/engine-win-x86 --parallel
ctest --test-dir build/engine-win-x86 --output-on-failure

build/engine-win-x86/getnative_cpu_column_simd_tests.exe --cpu-isa scalar
build/engine-win-x86/getnative_cpu_column_simd_tests.exe --cpu-isa sse2
build/engine-win-x86/getnative_cpu_column_simd_tests.exe --cpu-isa avx2
build/engine-win-x86/getnative_cpu_column_simd_tests.exe --cpu-isa avx512

build/engine-win-x86/getnative_cpu_backend_benchmark.exe `
  --matrix engine/bench/fixtures/metal_kernel_matrix.json `
  --cpu-isa all --samples 21 --assert `
  --artifact-root build/engine-win-x86/artifacts/cpu-backend

build/engine-win-x86/getnative-engine.exe capabilities
Get-CimInstance Win32_Processor | Format-List Name,Manufacturer,NumberOfCores,NumberOfLogicalProcessors
```

Forced command 请求当前 host 不 available 的 tier 时，预期结果是非零退出和明确 `requested CPU ISA is unavailable` 类错误；不要把该项当 test failure，也不能让进程以 illegal-instruction exception 退出。每个 host 只对 available tiers 运行 kernel correctness，完整 matrix 由多台 host 汇总。

保存逐 object 反汇编。实际 object 路径由 generator 决定，以下 `<...>` 必须替换为 build tree 中已解析的精确路径：

```powershell
llvm-objdump --disassemble <baseline-or-sse2.obj> | Set-Content build/engine-win-x86/artifacts/cpu-backend/sse2.asm
llvm-objdump --disassemble <avx2-production.obj> | Set-Content build/engine-win-x86/artifacts/cpu-backend/avx2.asm
llvm-objdump --disassemble <avx512-production.obj> | Set-Content build/engine-win-x86/artifacts/cpu-backend/avx512.asm

# Production AVX2/AVX-512 should contain FMA (count > 0).
Select-String build/engine-win-x86/artifacts/cpu-backend/avx2.asm -Pattern 'vfmadd|vfnmadd' |
  Measure-Object | Select-Object -ExpandProperty Count
Select-String build/engine-win-x86/artifacts/cpu-backend/avx512.asm -Pattern 'vfmadd|vfnmadd' |
  Measure-Object | Select-Object -ExpandProperty Count
```

AVX2/AVX-512 的 FMA 搜索计数应 **大于 0**。另检查 baseline/SSE2 object 不含高阶 VEX/EVEX 泄漏。可用 `dumpbin /DISASM` 替代 `llvm-objdump`，但 handback 必须保存命令、tool version 和原始输出。

至少在以下真实运行环境重复 CPU tests/package startup：无 AVX2 的 Windows x64、AVX2、AVX-512。若暂时没有 AVX-512 host，只能报告 compiled + mock dispatch green，不能完成第 22.1 节 AVX-512 runtime 条件。

### 19.3 M3 后的 GPU build matrix

```powershell
# CUDA only
cmake -S engine -B build/engine-win-cuda -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DGETNATIVE_ENABLE_METAL=OFF `
  -DGETNATIVE_ENABLE_CUDA=ON `
  -DGETNATIVE_ENABLE_VULKAN=OFF

# Vulkan only
cmake -S engine -B build/engine-win-vulkan -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DGETNATIVE_ENABLE_METAL=OFF `
  -DGETNATIVE_ENABLE_CUDA=OFF `
  -DGETNATIVE_ENABLE_VULKAN=ON

# CUDA + Vulkan
cmake -S engine -B build/engine-win-gpu -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DGETNATIVE_ENABLE_METAL=OFF `
  -DGETNATIVE_ENABLE_CUDA=ON `
  -DGETNATIVE_ENABLE_VULKAN=ON

cmake --build build/engine-win-cuda --parallel
cmake --build build/engine-win-vulkan --parallel
cmake --build build/engine-win-gpu --parallel
ctest --test-dir build/engine-win-cuda --output-on-failure
ctest --test-dir build/engine-win-vulkan --output-on-failure
ctest --test-dir build/engine-win-gpu --output-on-failure
```

实际 option 名与 architecture list 必须在 M3 固定并写入 README/CMake help；不要让 CI 靠隐含环境猜测。

### 19.4 GPU backend tools

```powershell
build/engine-win-cuda/getnative_cuda_benchmark.exe --full --assert
build/engine-win-vulkan/getnative_vulkan_benchmark.exe --full --assert

compute-sanitizer --tool memcheck `
  build/engine-win-cuda/getnative_cuda_conformance_tests.exe

spirv-val path/to/generated.spv
spirv-dis path/to/generated.spv -o path/to/generated.spvasm
```

CUDA artifact/SASS 检查使用 build tree 中解析出的真实 fatbin/cubin 路径；以下 placeholder 不得原样出现在 handback：

```powershell
nvcc --version
ptxas --version
ncu --version
nsys --version

cuobjdump --list-elf <getnative_cuda.fatbin>
cuobjdump --list-ptx <getnative_cuda.fatbin>
cuobjdump --dump-resource-usage <getnative_cuda.fatbin> |
  Set-Content build/engine-win-cuda/artifacts/cuda/resource-usage.txt
cuobjdump --dump-sass <getnative_cuda.fatbin> |
  Set-Content build/engine-win-cuda/artifacts/cuda/all-targets.sass

New-Item -ItemType Directory -Force build/engine-win-cuda/artifacts/cuda/cubins | Out-Null
Push-Location build/engine-win-cuda/artifacts/cuda/cubins
cuobjdump --extract-elf all <absolute-getnative_cuda.fatbin>
Pop-Location

nvdisasm --print-line-info-inline --print-life-ranges `
  build/engine-win-cuda/artifacts/cuda/cubins/<target-sm.cubin> |
  Set-Content build/engine-win-cuda/artifacts/cuda/<target-sm>.nvdisasm

# SASS 用于优化与资源审计，不是 no-FMA 门禁。
# 可按需统计 FMA / 访存 / 控制流，但不得因出现 FFMA/HFMA 判失败。
Select-String build/engine-win-cuda/artifacts/cuda/all-targets.sass `
  -Pattern 'FFMA|DFMA|HFMA' |
  Measure-Object |
  Select-Object -ExpandProperty Count
```

保存 `-Xptxas=-v` 原始输出；spill load/store 非零时先定位原因，不能仅用 `--maxrregcount` 压数字。生产 cubin **不得** 以 `-Xptxas=-O0` 构建。

benchmark/test 增加等价的内部 forced variant surface；名字可调整，但 handback 必须能复现每层 A/B：

```powershell
build/engine-win-cuda/getnative_cuda_conformance_tests.exe --cuda-variant cpp-generic
build/engine-win-cuda/getnative_cuda_conformance_tests.exe --cuda-variant cpp-specialized
build/engine-win-cuda/getnative_cuda_conformance_tests.exe --cuda-variant inline-ptx

build/engine-win-cuda/getnative_cuda_benchmark.exe --full --samples 21 `
  --cuda-variant all --artifact-root build/engine-win-cuda/artifacts/cuda

nsys profile --trace=cuda,osrt --sample=none `
  --output=build/engine-win-cuda/artifacts/cuda/timeline `
  build/engine-win-cuda/getnative_cuda_benchmark.exe --full --cuda-variant cpp-specialized

ncu --set full --target-processes all `
  --export build/engine-win-cuda/artifacts/cuda/ncu-specialized `
  build/engine-win-cuda/getnative_cuda_benchmark.exe --profile-cases `
  --cuda-variant cpp-specialized
```

验证 forward-compatible PTX fallback：

```powershell
$env:CUDA_FORCE_PTX_JIT = '1'
try {
  build/engine-win-cuda/getnative_cuda_conformance_tests.exe --cuda-variant cpp-generic
} finally {
  Remove-Item Env:CUDA_FORCE_PTX_JIT
}
```

若 forced `inline-ptx` 未编译、目标 SM 不匹配或未通过 approval，预期是明确拒绝；不能静默运行另一个 variant 后仍把结果标成 PTX。

### 19.5 GUI/package

```powershell
Set-Location app
npm ci
npm run build
cargo test --manifest-path src-tauri/Cargo.toml
cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings
npm run tauri build
```

当前 npm engine scripts 不转发 Windows GPU options。M8 前必须改为明确 preset/env，并证明 bundled engine 就是测试过的 GPU binary。

### 19.6 Dependency scan

```powershell
dumpbin /DEPENDENTS build/engine-win-gpu/getnative-engine.exe
dumpbin /DEPENDENTS path/to/GetNative-VF.exe
```

产品 PE import table 不得出现：

- `nvcuda.dll`；
- `cudart64_*.dll`；
- `vulkan-1.dll`；
- Python；
- VapourSynth/AviSynth。

最后在真正没有 NVIDIA driver/Vulkan loader 的干净 Windows VM 运行 engine capabilities 和 packaged GUI。仅修改 PATH 不等价。

## 20. 禁止事项和停止条件

遇到以下任一项，停止扩展范围并先修证据链：

- 丢失或覆盖接收时 dirty changes。
- CPU/planner/upstream conformance 失败。
- x86 production SSE2/AVX2/AVX-512 任一 available tier 相对 scalar 超出 tolerance，或 tail/unaligned path 越界、覆盖 padding。
- 只检查 CPUID、不检查 OSXSAVE/XCR0，或 forced unavailable ISA 触发 illegal instruction。
- 对整个 `getnative_core`/executable/package 使用 `/arch:AVX2`、`/arch:AVX512`、`-mavx2` 或 `-mavx512*`，使 baseline/SSE2 fallback 含高阶指令。
- AVX2/AVX-512 production object 反汇编 **缺少** FMA（在支持 FMA 的编译目标上），或 baseline/SSE2 泄漏高阶 ISA。
- 重新把 CPU 正确性改回 bit-identical / 禁 FMA 契约。
- AVX-512 只因 available 就自动优先，未比较 AVX2，或在 primary matrix 更慢仍选中。
- GPU 为通过测试而修改 CPU oracle、threshold、crop 或 candidate order。
- GPU 生成 filter taps、normal matrix 或 LDLT。
- 重新引入 GPU 多 math-mode（`strict` / `fast-math` / 并行 fatbin 切换），或为“语义扫描”使用 `-Xptxas=-O0` 生产构建。
- 重新引入 Float16/`half2`/hardware interpolation。
- 在 CUDA C++ baseline、profile、SASS 和逐 SM A/B 之前加入 inline PTX；inline wrapper 无 C++ fallback、无 `__CUDA_ARCH__` guard 或跨 address space 错用 pointer。
- 仅因 PTX 指令数更少就宣称优化；没有检查目标 cubin SASS、register/spill、Nsight 和 end-to-end wall time。
- 为指定 SASS 排列加入 dummy dependency/barrier，或使用非官方 SASS assembler/patcher 生成 shipping artifact。
- 对所有 kernel/SM 统一使用 `--maxrregcount`、`__launch_bounds__`、cache hint、`cp.async` 或 TMA，而没有逐 kernel resource/profile 证据。
- architecture-specific `sm_XXa` 指令进入 generic PTX fallback，或一个 SM 的 approval 被外推到其他 compute capability/driver/toolkit。
- CUDA Graph、fusion、persistent kernel 或多 stream 破坏 cancel/drain、stable order、workspace ceiling 或错误 provenance。
- options OFF 时仍要求 GPU SDK/runtime。
- PE import table 硬依赖 CUDA/Vulkan runtime。
- 只有 compile proof 却报告 device available。
- 真实 analyze path 不存在却报告 command available。
- GPU 失败静默跑 CPU，或静默切换到未批准 kernel variant / 第二套数学 artifact。
- specialized 与 generic 在同一生产数学配置下不一致。
- candidate 超 workspace 仍提交。
- cancel 返回时仍有 GPU 写入可复用/已释放 buffer。
- total explicit working set达到或超过 2 GiB。
- 在没有 profile 时重开被否决的 Float16、topology interning、plan ring/cache。
- 没有真实 Windows run 却声称 correctness/performance 完成。
- NVIDIA Vulkan 结果被宣传为 AMD 验证。
- 修改 upstream checkout、复制无许可证 muvsfunc 源码或引入未盘点依赖。
- 无 Apple 验证却重构 Metal Objective-C++/shader 路径。

## 21. Windows agent 最终 handback

Handback 必须包含：

1. 接收 HEAD、初始 dirty path/hash、最终 changed-file list。
2. 每个新增/修改文件的职责。
3. Windows version、CPU/RAM、CPU vendor/family/model/stepping、GPU、UUID/PCI、driver。
4. VS/MSVC 或 clang-cl、CMake、Ninja、CUDA Toolkit/nvcc、Vulkan SDK/glslc 版本。
5. x86 raw CPUID/XCR0、compiled/available/selected ISA、selection reason、CPU `math_mode=production`、是否 forced；每台 CPU 单独记录。
6. scalar/SSE2/AVX2/AVX-512 production forced conformance、unsupported-tier rejection 和无 illegal-instruction 结果。
7. baseline/SSE2、AVX2、AVX-512 object 的 compile flags、hash 和反汇编检查（含 FMA 证据）。
8. **单一** CUDA fatbin、PTX、Vulkan SPIR-V、engine、benchmark 和 package hashes（不得交付多套 GPU math-mode artifact）。
9. 完整 configure/build/test/benchmark/package 命令和 exit code。
10. scalar-only、x86 SIMD、CPU-only、CUDA-only、Vulkan-only、both build 结果。
11. no-AVX2、no-driver、no-loader 的 engine/package capability 和 startup 输出。
12. 每个 CPU/GPU conformance case 的最大误差、argmin distance、top-k、失败数；CPU/GPU 均报 relative tolerance 结果。
13. GPU generic vs specialized bit-identity 结果（同一生产数学配置）。
14. raw benchmark samples、median、MAD、speedup、cold/warm/amortized totals；GPU speedup 明确给出同次 `auto + production` CPU 分母。
15. AVX-512 相对 AVX2 的选择 gate 和可获得的 downclock/frequency 证据。
16. allocation/reuse、workspace、queued plan、total peak bytes。
17. cancel latency、tile duration、submission/completion counts。
18. Nsight/RenderDoc/RGP/validation/Compute Sanitizer 关键证据路径。
19. `dumpbin /DEPENDENTS` 关键输出。
20. Grok/其他 agent 的输出只能作为建议；最终以 Windows checkout diff 和 host tests 为证据。
21. 未验证 CPU/ISA、平台、GPU、driver、shape 和剩余风险。
22. fatbin 内 native cubin/PTX target list、逐 artifact hash、forced PTX-JIT conformance 和 cold/warm JIT 时间。
23. 每个 CUDA kernel variant 的 ptxas resource、register/spill、SASS/`nvdisasm` 和 Nsight Systems/Compute report 路径（用于证明优化，而非 no-FMA）。
24. CUDA C++ specialization、layout/transpose、fusion/reduction、Graph、async/TMA 的逐项 A/B 和保留/删除理由。
25. 每个 inline PTX candidate 的 codegen 缺口、wrapper/guard/fallback、逐 SM raw samples、approval，或 `NO_PTX_CANDIDATE`/`NO_PTX_WINNER` 结论。
26. CUDA runtime provenance sample：device UUID、compute capability、driver/toolkit、variant、native/PTX-JIT、生产优化 flags、单一 artifact/benchmark id。

## 22. 完成定义

### 22.1 x86 CPU backend 完成

以下全部满足，才可称 Windows x86 CPU backend 工作完成：

- SSE2/AVX2/AVX-512 production 与 scalar forced path 均有独立、安全的 runtime dispatch；CPUID、XCR0、compiled/available/selected 状态已验证；`math_mode=production`。
- 所有 available production tier 在第 15.2 节完整矩阵中相对 scalar 在 tolerance 内，无越界、padding overwrite 或 MXCSR 全局副作用。
- ISA-specific translation units/flags 隔离，baseline/SSE2 可在无 AVX2 host 运行；反汇编证明 AVX2/AVX-512 含 FMA，全局 target 无高阶 ISA 泄漏到 baseline/SSE2。
- P0 adjacent-column inverse 完成；P1 完成或用 profiler/benchmark 明确证明不保留的原因；planner Float64/LDLT/cache 契约未被改写。
- 自动选择由真实同机 benchmark 驱动；AVX-512 只有优于 AVX2 时才选中，available 与 selected 可不同。
- CPU capability/provenance 和第 21 节 handback 证据完整。缺少真实 AVX-512 host 时只能报告 AVX-512 compiled/mock-dispatch，不得宣称此完成定义全部满足。

### 22.2 CUDA/Vulkan backend handover 完成

以下全部满足，才可称 CUDA/Vulkan backend 工作完成：

- 两者有真实 host adapter、embedded artifact、device enumeration、执行和清理路径。
- 两者功能支持 H、V、both、p=1 和全部合法 15/16 shape。
- Generic 全覆盖；B3/B7/B11/B15 optimization 已实现或以 profiler 明确记录未保留原因。
- CPU tolerance、valley、order、generic/specialized、memory、cancel 和 failure-path tests 通过。
- CUDA 的 `cpp-generic`、selected C++/intrinsic/architecture/inline-PTX variants 可被 forced；每个 selected variant 与 generic bit-identical，并有对应真实 SM 的 sanitizer、SASS、resource、Nsight 和第 16.6 节 approval。
- CUDA/Vulkan 均为 **单一极致优化数学路径**；native cubin 与 forced generic PTX-JIT 都通过第 15.4 节。inline PTX 有逐 candidate 通过记录，或明确的 `NO_PTX_CANDIDATE`/`NO_PTX_WINNER`，direct SASS patch 未进入 artifact。不存在 strict/fast-math 并行通路，生产构建未使用 `-Xptxas=-O0`。
- caller-owned planner/cache 契约未被绕过。
- CPU-only build 与无 GPU runtime package startup 不回归。
- capability 只报告事实；command availability 可继续 false；GPU capability 不暴露多 math mode。
- runtime 可选且 PE import table 无硬依赖。
- handback 包含第 21 节证据。

### 22.3 产品端到端完成

除 22.1 和所需的 22.2 backend 条件外，还必须完成 M9：真实 analyze worker、Tauri controller、frontend analysis planner、progress/cancel/result/provenance 和 end-to-end tests。只有这时才允许 `commands.analyze=true` 和 per-backend `analysis_command_available=true`。

### 22.4 默认启用

默认启用不是 backend 完成的同义词。x86 `auto + strict` 只选择通过 5%/3% gate 的 ISA。每个 GPU backend 只有在相对同机 `auto + strict` CPU 的 end-to-end strict speedup `>=3x`、数值/内存/取消 gates 全过后，才能单独进入默认策略评审；否则保持显式 experimental selection。
