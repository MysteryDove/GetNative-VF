# CUDA Verify / 双路 NVDEC — RTX 5080, 2026-09-05

在同一最终二进制中，显式启用双路 NVDEC 后，5001 帧双轴 Bilinear Verify 从 **851.24 提升到 1462.22 fps（1.72×）**；整集 34072 帧从 **40.27 秒缩短至 23.04 秒（1.75×）**。完整逐帧结果保持一致，仍为 NVDEC → CUDA、zero-copy，无软件回退。

## 测量定位

RTX 5080 有两组 NVDEC；NVIDIA 说明单 decoding session 不能利用多组 NVDEC 的总吞吐。来源：[5080 官方规格](https://www.nvidia.com/en-us/geforce/graphics-cards/50-series/rtx-5080/)、[NVDEC Application Note](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvdec-application-note/index.html)。

| 对照 | fps | 范围与边界 |
|---|---:|---|
| 单路纯 NVDEC | 约 863 | 原片 #0–#5000，包含解码完成等待，无分析 |
| 单路完整 Verify | 851.24 | 同范围，包含热索引与规划检查 |
| 双路纯 NVDEC | 约 1506 | 同范围，两 session，包含解码完成等待 |
| 双路完整 Verify | 1462.22 | 同范围，同一二进制仅切换 session 上限 |
| 驻留帧分析 microbenchmark | 约 3213 | 预解码原片 #1000–#1007，8 帧循环，共 2000 次 CUDA luma 分析，8 slot；不含解码 |

这些是不同计时边界的诊断对照，不应混称为 GPU 核时间。驻留帧 microbenchmark 含每次 luma→F32、分析、结果回读与同步，只用于判断计算供给能力，不是整片吞吐。

## 实现与开关

启动应用或 worker 前设置：

```sh
export GETNATIVE_CUDA_DECODE_SESSIONS=2
```

默认未设置或设为 `1` 时维持单路；非法值返回 `bad_request`。这是一项显式启用的实验能力，暂未改变 GUI 默认行为。

双路只在以下条件满足时启用：

- CUDA 后端，共享 native context/stream；
- 选中帧是连续区间，至少 1024 帧；
- 分析并发至少 2；
- 中点之后存在合适的 indexed non-leading RAP，且第二段至少保留 256 帧。

两个 session 分别打开 demuxer/decoder，复用既有索引规划器处理 leading pictures 和 extradata。第二段的局部选择位置映射回原始 seq；分析使用原来的有界队列，帧结果按原身份收集。短任务、稀疏扫描、单并发和无法合适分段的任务仍使用一个 session。`telemetry.decode_sessions` 报告实际数量。

该开关不改变 Vulkan/VideoToolbox 路由。独立 CUDA copy stream 的实验没有稳定收益，已撤回；最终没有修改 CUDA 数学内核或增加 CUDA Graph。

## 主 A/B

硬件/媒体与 `vulkan-verify-rtx5080.md` 相同：Linux RTX 5080、驱动 595.84、运行时系统 FFmpeg 8.0.1-3ubuntu2，原始 1080p H.264 `00001.m2ts`。

Recipe：`h_plus_w`，Bilinear，候选 1536×864，p=1，四边 crop=5，threshold=0.015，分析并发 8。

5001 帧窗口每个模式先预热一次，再测三次：

| 模式 | 三次 fps | 中位数 |
|---|---|---:|
| 默认单路 | 851.34 / 851.24 / 850.96 | 851.24 |
| 双路 | 1464.02 / 1459.19 / 1462.22 | 1462.22 |

整集每种模式预热一次，再测一次：

| 模式 | 时间 | fps | 采样整卡显存峰值 |
|---|---:|---:|---:|
| 单路 | 40.2696 s | 846.10 | 895 MiB |
| 双路 | 23.0375 s | 1478.98 | 1040 MiB |

显存每秒采样，包含桌面等基础占用，不是精确进程分配峰值。整集同时运行该轻量采样器；5001 帧主 A/B 没有采样器。

## 验证

- 5001 帧及整集 34072 帧：单/双路 seq、frame_index、PTS、timestamp、error 完全一致，无缺帧和重复。
- 新增 `getnative_cuda_decode_sessions_tests`：2400 帧 open-GOP H.264，覆盖完整区间、跨 GOP 起止、稀疏选择、短任务、分析并发=1、取消后复用和非法开关值。
- 1200 帧 1080p HEVC Main10 open-GOP、p4：实际双路运行，结果与单路一致；该合成片仅作正确性覆盖。
- 新增测试及 media worker、media index、Vulkan analysis 共四项相关测试通过；没有覆盖或回退上一轮 Vulkan 修改。
- 既有 CUDA telemetry 默认关闭与旧测试断言不一致的问题未在本轮扩大处理；不把此次相关测试通过描述为全套 CTest 全绿。
- 未验证 Windows 包、其他 GPU、多视频同时运行或机械硬盘上的收益，所以默认仍为单路。

## 复现与证据

```sh
GETNATIVE_CUDA_DECODE_SESSIONS=2 \
python3 engine/bench/media_verify_benchmark.py \
  /path/to/getnative-engine /path/to/00001.m2ts \
  --backend cuda --frames 5001 --repeats 3 --output /path/to/dual.json
```

将开关改为 `1` 得到同二进制对照；整集使用 `--frames 34072 --repeats 1`。报告会保存实际环境开关、请求、provenance、telemetry 和每帧结果。

开发机证据：`/home/owen/tmp/gnvf-tune-20260905/cuda-evidence/`。本机在当前任务产物的 `rtx5080/cuda-evidence/` 保留副本。

- 媒体 SHA-256：`72938cf98d3bc333b93ae1bc73faeb4d0b2a3399d835376c057e111d855130b5`
- 整集排序并 canonical JSON 编码的帧结果 SHA-256：`d07f2fe7cf4f45ede926e896d6ebe78d65b9350df0aa7dc8125f381a75aab994`

`comparison.json`、各实验 JSON/日志、驻留分析/纯解码探针、整集显存采样和源码/二进制校验值随证据保存；大型原始结果没有加入仓库。
