# CUDA Verify / 固定雙路 NVDEC 歷史測量 — RTX 5080, 2026-09-05

此頁保留早期固定單/雙路實現的測量，不是當前自動會話控制器的效能結論。
當時的環境變數開關已刪除；當前預設自適應行為、構建選項及後續測量見
[自適應解碼進展](adaptive-decode-progress.md) 和
[Worker 協議](../worker-protocol-v1.md)。

在同一最終二進位制中，顯式啟用雙路 NVDEC 後，5001 幀雙軸 Bilinear Verify 從 **851.24 提升到 1462.22 fps（1.72×）**；整集 34072 幀從 **40.27 秒縮短至 23.04 秒（1.75×）**。完整逐幀結果保持一致，仍為 NVDEC → CUDA、zero-copy，無軟體回退。

## 測量定位

RTX 5080 有兩組 NVDEC；NVIDIA 說明單 decoding session 不能利用多組 NVDEC 的總吞吐。來源：[5080 官方規格](https://www.nvidia.com/en-us/geforce/graphics-cards/50-series/rtx-5080/)、[NVDEC Application Note](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvdec-application-note/index.html)。

| 對照 | fps | 範圍與邊界 |
|---|---:|---|
| 單路純 NVDEC | 約 863 | 原片 #0–#5000，包含解碼完成等待，無分析 |
| 單路完整 Verify | 851.24 | 同範圍，包含熱索引與規劃檢查 |
| 雙路純 NVDEC | 約 1506 | 同範圍，兩 session，包含解碼完成等待 |
| 雙路完整 Verify | 1462.22 | 同範圍，同一二進位制僅切換 session 上限 |
| 駐留幀分析 microbenchmark | 約 3213 | 預解碼原片 #1000–#1007，8 幀迴圈，共 2000 次 CUDA luma 分析，8 slot；不含解碼 |

這些是不同計時邊界的診斷對照，不應混稱為 GPU 核時間。駐留幀 microbenchmark 含每次 luma→F32、分析、結果回讀與同步，只用於判斷計算供給能力，不是整片吞吐。

## 歷史實現

當時透過環境變數顯式啟用雙路，預設單路。該機制已由共享排程器和
自動會話控制器替代，舊環境變數不再生效。

雙路只在以下條件滿足時啟用：

- CUDA 後端，共享 native context/stream；
- 選中幀是連續區間，至少 1024 幀；
- 分析併發至少 2；
- 中點之後存在合適的 indexed non-leading RAP，且第二段至少保留 256 幀。

兩個 session 分別開啟 demuxer/decoder，複用既有索引規劃器處理 leading pictures 和 extradata。第二段的區域性選擇位置對映回原始 seq；分析使用原來的有界佇列，幀結果按原身份收集。短任務、稀疏掃描、單併發和無法合適分段的任務仍使用一個 session。`telemetry.decode_sessions` 報告實際數量。

該開關不改變 Vulkan/VideoToolbox 路由。獨立 CUDA copy stream 的實驗沒有穩定收益，已撤回；最終沒有修改 CUDA 數學核心或增加 CUDA Graph。

## 主 A/B

硬體/媒體與 `vulkan-verify-rtx5080.md` 相同：Linux RTX 5080、驅動 595.84、執行時系統 FFmpeg 8.0.1-3ubuntu2，原始 1080p H.264 `00001.m2ts`。

Recipe：`h_plus_w`，Bilinear，候選 1536×864，p=1，四邊 crop=5，threshold=0.015，分析併發 8。

5001 幀視窗每個模式先預熱一次，再測三次：

| 模式 | 三次 fps | 中位數 |
|---|---|---:|
| 預設單路 | 851.34 / 851.24 / 850.96 | 851.24 |
| 雙路 | 1464.02 / 1459.19 / 1462.22 | 1462.22 |

整集每種模式預熱一次，再測一次：

| 模式 | 時間 | fps | 取樣整卡視訊記憶體峰值 |
|---|---:|---:|---:|
| 單路 | 40.2696 s | 846.10 | 895 MiB |
| 雙路 | 23.0375 s | 1478.98 | 1040 MiB |

視訊記憶體每秒取樣，包含桌面等基礎佔用，不是精確程序分配峰值。整集同時執行該輕量取樣器；5001 幀主 A/B 沒有采樣器。

## 驗證

- 5001 幀及整集 34072 幀：單/雙路 seq、frame_index、PTS、timestamp、error 完全一致，無缺幀和重複。
- 當時新增 `getnative_cuda_decode_sessions_tests`：2400 幀 open-GOP H.264，覆蓋完整區間、跨 GOP 起止、稀疏選擇、短任務、分析併發=1、取消後複用和非法開關值；當前測試改為比較兩個固定會話配置的引擎。
- 1200 幀 1080p HEVC Main10 open-GOP、p4：實際雙路執行，結果與單路一致；該合成片僅作正確性覆蓋。
- 新增測試及 media worker、media index、Vulkan analysis 共四項相關測試透過；沒有覆蓋或回退上一輪 Vulkan 修改。
- 既有 CUDA telemetry 預設關閉與舊測試斷言不一致的問題未在本輪擴大處理；不把此次相關測試透過描述為全套 CTest 全綠。
- 當時未驗證 Windows 包、其他 GPU、多影片同時執行或機械硬碟上的收益；這些歷史結果不能代替相應平臺驗收。

## 當前固定會話對照方式

分別構建 `GETNATIVE_TEST_FIXED_DECODE_SESSIONS=1` 與 `=2` 的引擎，再以
相同媒體、配方和引數執行以下命令，將引擎路徑與輸出檔案分別替換。
該構建覆蓋優先於自適應選項。它測量的是當前固定配置，不能重現上表舊二進位制
的全部實現細節，也不能用來代替預設自動配置的驗證。

```sh
python3 engine/bench/media_verify_benchmark.py \
  /path/to/getnative-engine /path/to/00001.m2ts \
  --backend cuda --frames 5001 --repeats 3 --output /path/to/dual.json
```

整集使用 `--frames 34072 --repeats 1`。報告儲存引擎路徑、請求、provenance、
telemetry 和每幀結果；應檢查 `decode_sessions` 確認實際會話數。

## 歷史證據摘要

- 媒體 SHA-256：`72938cf98d3bc333b93ae1bc73faeb4d0b2a3399d835376c057e111d855130b5`
- 整集排序並 canonical JSON 編碼的幀結果 SHA-256：`d07f2fe7cf4f45ede926e896d6ebe78d65b9350df0aa7dc8125f381a75aab994`

大型原始結果未加入倉庫；以上為歷史測量摘要。
