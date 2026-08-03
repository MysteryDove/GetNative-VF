# 当前算法层流程图

本文按当前工作树绘制。实线表示已经存在的代码路径，虚线表示算法后端
已经实现但尚未由 GUI/CLI 的 `analyze` 命令接通的路径。

```mermaid
flowchart TB
    %% ---------------- GUI boundary ----------------
    subgraph GUI[桌面前端：React / TypeScript]
        U[用户输入\nprofile / mode / source geometry / base size]
        CAP[启动时 invoke engine_capabilities]
        GEO[点击 Preview Geometry\n校验数字输入]
        VIEW[渲染 geometry、profile、kernel、backend 能力]
        ANALYZE[分析控制\n当前 disabled：commands.analyze = false]
    end

    subgraph TAURI[Tauri Rust 控制层]
        TCAP[engine_capabilities]
        TGEO[engine_geometry]
        VALID[校验 profile、mode、整数/浮点范围\n组装 CLI 参数]
        SPAWN[find_engine + run_engine\n同步启动 getnative-engine\n读取 JSON stdout]
        ENV[EngineEnvelope\npath + payload]
    end

    subgraph ENGINE[getnative-engine CLI / core]
        CMD{command}
        CAPCMD[capabilities\n输出 schema_version=2]
        GEOCMD[geometry\n解析 profile / mode / 参数]
        GEOM[descale_geometry / descale_geometry_pro\n计算 native canvas + active crop/offset]
        JSONG[JSON geometry response]
        ANALYZECMD[analyze\n目标命令，当前不存在/未暴露]
    end

    U --> GEO
    CAP --> TCAP
    GEO --> TGEO
    TCAP --> SPAWN
    TGEO --> VALID --> SPAWN
    SPAWN --> CMD
    CMD -->|capabilities| CAPCMD --> ENV --> VIEW
    CMD -->|geometry| GEOCMD --> GEOM --> JSONG --> ENV
    ENV --> VIEW
    ANALYZE -. planned invoke .-> ANALYZECMD
    ANALYZECMD -. missing current CLI path .-> CMD

    %% ---------------- shared planning ----------------
    subgraph FRONT[算法前端：geometry / candidate / axis planner]
        SPEC[分析请求\nsource frame + profile + axes + candidates + metric]
        PROFILE[选择 compatibility profile\nmuf-d278cd3 / getfnative-44c8d0f / modern]
        GRID[生成 candidate grid\nprofile-specific decimal/grid semantics]
        CROP[确定 native width/height、active_length、shift\nborder / crop / threshold / p-norm]
        REQ[为每个 candidate 生成 AxisPlanRequest\nH、V 或指定单轴]
        CACHE{AxisPlanCache 命中?}
        BUILD[并行 build_axis_plans\n去重 request key + bounded workers]
        FILTER[生成 forward resize sparse A\nfilter taps + border handling]
        TRANSPOSE[构造 sparse A^T\n按 source observation index 排序]
        NORMAL[形成 banded A^T A]
        LDLT[banded LDLT factorization\nFloat64 build -> packed Float32]
        PACK[不可变 AxisPlan\nforward / transpose / lower_ld / upper_l / inverse_diagonal]
        READY[CandidateAnalysis 列表\n保留 candidate input order]
    end

    ANALYZECMD -. request .-> SPEC
    SPEC --> PROFILE --> GRID --> CROP --> REQ --> CACHE
    CACHE -->|hit| READY
    CACHE -->|miss| BUILD --> FILTER --> TRANSPOSE --> NORMAL --> LDLT --> PACK --> READY
    PACK -. publish bounded cache .-> CACHE

    %% ---------------- scheduler ----------------
    subgraph SCHED[共享调度与结果协议]
        SELECT[按 requested backend 选择\nCPU / Metal / CUDA]
        AXES{analysis axes}
        HORIZONTAL[horizontal only]
        VERTICAL[vertical only]
        BOTH[combined H+V\n固定 inverse H -> V]
        BATCH[按 backend 组织 candidate batch\nCPU workers / Metal tiles / CUDA candidate threads]
        METRIC[thresholded absolute difference\ncrop + p-norm + deterministic reduction]
        RESULTS[CandidateResult{id,error}\ninput order + telemetry/provenance]
        RETURN[JSON result/event\nprogress / warning / result / error]
    end
    READY --> SELECT --> AXES
    AXES --> HORIZONTAL --> BATCH
    AXES --> VERTICAL --> BATCH
    AXES --> BOTH --> BATCH

    %% ---------------- CPU ----------------
    subgraph CPU[Backend 1：CPU deterministic oracle / fallback]
        CPUSEL[CPU ISA dispatch\nscalar / SSE2 / AVX2 / AVX512\n当前选择由 cpu_features 决定]
        CPUDESC[descale_2d_f32\n逆向 H/V：A^T b -> forward solve -> D^-1 -> backward solve]
        CPUREC[reconstruct_2d_f32\n按 forward order 选择 V->H 或 H->V]
        CPUMEM[复用 CpuWorkspace\nintermediate / native / reconstruction_row\n不保存完整重建帧]
        CPUMETRIC[analyze_batch_f32\n固定 raster order reduction]
    end
    BATCH --> CPUSEL --> CPUDESC --> CPUMEM --> CPUREC --> CPUMETRIC --> METRIC
    HORIZONTAL -. axis policy .-> CPUMETRIC
    VERTICAL -. axis policy .-> CPUMETRIC

    %% ---------------- Metal ----------------
    subgraph METAL[Backend 2：Metal Apple GPU]
        MPROBE[Metal device probe\ncompiled + device available]
        MTILE[候选分 tile\ndefault tile_size=32]
        MUPLOAD[上传 source + immutable AxisPlan\nshared/reused MTLBuffer]
        MSHAPE[按 plan shape dispatch\nhalf-bandwidth 1..15 / forward width 2..16]
        MSPEC[bandwidth-1 / bandwidth-3 specialized\ngeneric path for larger shapes]
        MSOLVE[GPU inverse H/V\nordered solve direction]
        MRECON[forward reconstruction + fused metric\nblock partials，不回传完整帧]
        MREDUCE[CPU fixed-order merge partials]
    end
    BATCH -. metal .-> MPROBE --> MTILE --> MUPLOAD --> MSHAPE --> MSPEC --> MSOLVE --> MRECON --> MREDUCE --> METRIC

    %% ---------------- CUDA ----------------
    subgraph CUDA[Backend 3：CUDA profiled cpp-generic]
        CPROBE[CUDA Driver API 动态加载\n创建 dedicated context + probe device]
        CFATBIN[加载 embedded nvcc fatbin\nSM75+ native matrix + PTX fallbacks]
        CUPLOAD[pinned source staging + resident plan cache\ntap-major forward plan]
        CWORK[persistent execution slots\nfree-memory budget + candidate tiling]
        CKERNEL[axis-specific CUDA C++ kernels\nsource/local transpose + single/paired LDLT solve]
        CFUSE[horizontal / both fused reconstruction\nthreshold + p=1 partial reduction]
        CORDER[deterministic candidate result\ncurrent variant: cpp-generic]
    end
    BATCH -. cuda .-> CPROBE --> CFATBIN --> CUPLOAD --> CWORK --> CKERNEL --> CFUSE --> CORDER --> METRIC

    METRIC --> RESULTS --> RETURN
    RETURN -. future Tauri event/response .-> VIEW

    %% ---------------- status styling ----------------
    classDef live fill:#dff5e3,stroke:#287a3d,color:#143c1e;
    classDef gap fill:#fff0d6,stroke:#b36a00,color:#5b3500,stroke-dasharray: 5 5;
    classDef backend fill:#e8f0ff,stroke:#3d62a8,color:#172c55;
    classDef data fill:#f3f3f3,stroke:#777,color:#222;
    class U,CAP,GEO,VIEW,TCAP,TGEO,VALID,SPAWN,ENV,CMD,CAPCMD,GEOCMD,GEOM,JSONG,PROFILE,GRID,CROP,REQ,CACHE,BUILD,FILTER,TRANSPOSE,NORMAL,LDLT,PACK,READY,SELECT,AXES,HORIZONTAL,VERTICAL,BOTH,BATCH,METRIC,RESULTS,RETURN live;
    class ANALYZE,ANALYZECMD gap;
    class CPUSEL,CPUDESC,CPUREC,CPUMEM,CPUMETRIC,MPROBE,MTILE,MUPLOAD,MSHAPE,MSPEC,MSOLVE,MRECON,MREDUCE,CPROBE,CFATBIN,CUPLOAD,CWORK,CKERNEL,CFUSE,CORDER backend;
    class SPEC data;
```

## 当前接入状态

| 层 | 当前状态 | 代码位置 |
| --- | --- | --- |
| React -> Tauri | 已接通 `engine_capabilities`、`engine_geometry` | `app/src/App.tsx`、`app/src-tauri/src/lib.rs` |
| Tauri -> CLI | 已接通同步 sidecar/CLI 调用和 JSON 返回 | `app/src-tauri/src/lib.rs` |
| CLI geometry | 已接通，调用 `descale_geometry*` | `engine/src/cli/main.cpp` |
| Candidate / AxisPlan | 已实现，支持 cache、批量去重和并行构建 | `engine/src/planner/axis_plan.cpp`、`axis_planner.cpp` |
| CPU analysis | 已实现，作为 deterministic oracle/fallback | `engine/src/backend/cpu/cpu_analysis.cpp` |
| Metal analysis | 已实现，支持单轴/双轴、tile、specialized/generic kernel | `engine/src/backend/metal/metal_backend.mm`、`getnative.metal` |
| CUDA analysis | 已进入 profile 阶段，动态 Driver API + axis-specific/fused CUDA C++ kernels | `engine/src/backend/cuda/cuda_backend.cpp`、`getnative_cuda_baseline.cu` |
| 统一 analyze 命令 | **当前未接通**：capability 中 `analyze=false`，三种 backend 的 `analysis_command_available=false` | `engine/src/cli/main.cpp` |

## 计算核心的共同语义

每个 backend 都消费同一份不可变 `AxisPlan`。planner 负责生成并压缩
`A`、`A^T` 和 `A^T A` 的 LDLT 因子；backend 不重新生成 taps。计算结果是
每个 candidate 一个 error，metric 阶段执行 crop、严格 threshold、p-norm
和固定顺序 reduction。

- `horizontal` / `vertical`：只执行请求轴。
- `both`：执行二维 descale 和 reconstruction；CPU 的 inverse 顺序固定为
  H -> V，forward 顺序按 plan 成本选择。
- CPU 复用有限 workspace，Metal 以 candidate tile 和 block partials 控制
  显存；CUDA 复用 persistent slot，以 candidate tile 和 fused partials 控制显存。
- 缺失 GPU、未编译或设备不可用时，能力层应报告原因；当前代码不会静默
  自动切换到 CPU。
