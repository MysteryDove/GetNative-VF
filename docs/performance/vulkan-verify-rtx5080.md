# Vulkan Full Video Check — RTX 5080, 2026-09-05

此页保留 9 月 5 日历史测量。后续修复了 FFmpeg 数值问题、帧锁跨线程交接，
并发现第二条解码队列有显著收益；见
[后续实验与验收](vulkan-optimization-experiments.md)。不要用此页旧基线数值替代当前正确性验收。

本轮将固定双轴 Bilinear Verify 从 **361–362 fps 提升至 812 fps（2.25×）**，仍为 Vulkan Video → Vulkan compute、zero-copy，无软件回退。最终没有保留计划缓存实验。

## 负载与计时

- 基线源码：`0fec15f`，最终为本轮未提交修改。
- GPU：RTX 5080，驱动 595.84；Linux。
- 原始 `00001.m2ts`，1920×1080 H.264 8-bit；对照使用 SHA-256 一致的完整输入。
- 范围 #0–#5000，共 5001 帧；`h_plus_w`，候选高 864（宽 1536），Bilinear，p=1，四边 crop=5，threshold=0.015，并发=8。
- 每个进程先运行一次相同 job 预热，再测三次；表中为 `frames_completed / job_total_ms` 的中位数，含热索引检查和规划。不是 GUI progress 平均值，也不是 GPU 核时间。
- 所有 A/B 使用同一媒体、驱动和实际加载的系统 FFmpeg 8.0.1-3ubuntu2 动态库。CMake 指向现有 FFmpeg SDK 的头文件/链接输入，但运行时 `ldd` 显示加载 `/usr/lib/x86_64-linux-gnu/libav*.so`；这不是打包产物验证。
- 初期 CUDA-only 实验构建目标为 SM120，最终恢复仓库默认 SM75/86/89/120 及 PTX75/120；Vulkan 编译选项保持相同。CUDA 数字作为同负载参考。

## 分阶段结果

| 版本 | fps | 判定 |
|---|---:|---|
| 原始基线 | 362.02 | 基准 |
| 仅每 slot 计划缓存 | 346.39 | 回退；不单独保留 |
| 缓存 + 小带宽特化 | 约 539 | 特化有显著收益 |
| 再加独立计算队列 | 约 572 | 队列隔离有增益 |
| 再提前发出图像完成 semaphore | 812.87 | 解除解码图像等待后续 metric 的依赖 |
| **最终（撤掉计划缓存）** | **811.85** | **保留** |
| 交错复测原始基线 | 361.45 | 基线稳定 |
| CUDA + NVDEC 参考 | 851.16 | 最终 Vulkan 达其 95.38% |
| Vulkan decode-only 对照 | 约 828 | 包含最终 device idle，无计算；不是硬件理论极限 |

最终三次为 812.19 / 811.42 / 811.85 fps；复测基线为 361.52 / 361.34 / 361.45 fps。

缓存在最终组合的 1001 帧筛选中没有超出波动的收益；撤掉缓存后重新做了完整 5001 帧验收。不能把“省去上传字节”当作吞吐提升。

## 保留的实现

1. **inverse 带宽特化**：同一 SPIR-V 通过 specialization constant 为 band=1/3/5/7 构造 pipeline，缩小 `recent` 窗口和固定循环。混合带宽 tile、其他合法带宽仍走通用 shader，保持原累加顺序。
2. **分离计算队列**：FFmpeg 仍只使用 compute family 的 queue 0。分析 slot 使用设备支持的额外队列；队列数量不足时按实际数量共享，并对相同 queue 加锁；只有一条 queue 时继续共享 FFmpeg 的锁。
3. **转换后通知解码器**：将原始图像访问与后续 F32 分析拆成两个 command buffer。第一段完成 luma conversion 后即可 signal AVVkFrame semaphore，FFmpeg 不再等待 inverse/forward/metric 才能继续访问 DPB。第二段保留正确的队列内依赖和最终 fence。异常路径等待已提交的 conversion，之后才销毁 ImageView/释放 frame。

原代码虽然没有 host 原始帧回传，却让解码等待了不再访问解码图像的后续计算。这是本次定位到的主要流水线问题。

## 验证结果与边界

- 所有 Vulkan 实验阶段的完整 5001 帧结果，包括 seq、frame_index、PTS、timestamp、error，排序后与基线完全一致；其中 2475 帧误差非零。
- 高度单轴、宽度单轴、Bicubic p4、Spline64 p3、Lanczos8 p2、并发=1：全部逐帧一致；均未见吞吐回退。短窗口成绩仅用于回归筛选，不与主表直接比较。
- 1080p HEVC Main10 合成输入、p4、128 帧：与基线逐帧一致，仅作正确性覆盖。
- 同一个 worker 连续三轮：取消 5001 帧任务，再完整跑 257 帧，均成功，zero-copy 保持。
- Vulkan CPU 对照测试（含新增单候选特化测试）、五份 SPIR-V 校验、media worker 测试通过。
- 完整 CTest 默认模式 **22/24**；`GETNATIVE_GPU_STAGE_PROFILE=stages` 下 **23/24**。遗留失败是 CUDA telemetry 测试假设：默认模式不累计 CUDA telemetry，worker 的 source residency 断言和直接 CUDA baseline 的 staged launch 断言因此失败。worker 失败已在未修改基线复现；CUDA 源码和测试均未修改，不宣称全套测试通过。
- **完整 Vulkan validation 未通过**：基线与最终版本都报告 FFmpeg 共享设备的既有错误，例如 synchronization2、samplerYcbcrConversion 未启用、视频图像 usage/format 不匹配；启用同步校验时 worker 提前退出。本轮不声称通过完整 validation。其日志与性能结果分开保留，后续需要修复共享设备功能契约后重新验证。
- 未验证 Windows 打包产物、AMD/Intel 设备或真实单队列硬件；此次实测是 Linux RTX 5080。

## 复现

使用 `engine/bench/media_verify_benchmark.py`，分别传入基线/修改后二进制：

```sh
python3 engine/bench/media_verify_benchmark.py \
  /path/to/getnative-engine /path/to/00001.m2ts \
  --backend vulkan --axes h_plus_w --candidate 864 \
  --concurrency 8 --frames 5001 --repeats 3 --output /path/to/result.json
```

结果文件保存请求、运行时 provenance、完整逐帧结果和 telemetry，脚本拒绝缺帧、重复帧及硬解回退。性能测量不启用 validation/profiling。

证据摘要：

- 媒体 SHA-256：`72938cf98d3bc333b93ae1bc73faeb4d0b2a3399d835376c057e111d855130b5`
- 基线 binary SHA-256：`210f7ace42a44ecec2196d8abc03e32ffe043fe861c200c074628ed4ea625f38`
- 最终 binary SHA-256：`8d19123619226eea39964204cdeee2cb868b5212e43a428d19564d755ffe808f`
- 5001 帧排序、JSON canonical 编码结果 SHA-256：`767af0230ef667f08fce387f902f22ae34ab2299df7eaac56844075729f788fe`

大型原始结果及视频未加入仓库；以上保留测量摘要、输入/二进制哈希和验证边界。
