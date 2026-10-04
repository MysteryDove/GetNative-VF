# Vulkan Full Video Check — RTX 5080, 2026-09-05

此頁保留 9 月 5 日曆史測量。後續修復了 FFmpeg 數值問題、幀鎖跨執行緒交接，
並發現第二條解碼佇列有顯著收益；見
[後續實驗與驗收](vulkan-optimization-experiments.md)。不要用此頁舊基線數值替代當前正確性驗收。

本輪將固定雙軸 Bilinear Verify 從 **361–362 fps 提升至 812 fps（2.25×）**，仍為 Vulkan Video → Vulkan compute、zero-copy，無軟體回退。最終沒有保留計劃快取實驗。

## 負載與計時

- 基線原始碼：`0fec15f`，最終為本輪未提交修改。
- GPU：RTX 5080，驅動 595.84；Linux。
- 原始 `00001.m2ts`，1920×1080 H.264 8-bit；對照使用 SHA-256 一致的完整輸入。
- 範圍 #0–#5000，共 5001 幀；`h_plus_w`，候選高 864（寬 1536），Bilinear，p=1，四邊 crop=5，threshold=0.015，併發=8。
- 每個程序先執行一次相同 job 預熱，再測三次；表中為 `frames_completed / job_total_ms` 的中位數，含熱索引檢查和規劃。不是 GUI progress 平均值，也不是 GPU 核時間。
- 所有 A/B 使用同一媒體、驅動和實際載入的系統 FFmpeg 8.0.1-3ubuntu2 動態庫。CMake 指向現有 FFmpeg SDK 的標頭檔案/連結輸入，但執行時 `ldd` 顯示載入 `/usr/lib/x86_64-linux-gnu/libav*.so`；這不是打包產物驗證。
- 初期 CUDA-only 實驗構建目標為 SM120，最終恢復倉庫預設 SM75/86/89/120 及 PTX75/120；Vulkan 編譯選項保持相同。CUDA 數字作為同負載參考。

## 分階段結果

| 版本 | fps | 判定 |
|---|---:|---|
| 原始基線 | 362.02 | 基準 |
| 僅每 slot 計劃快取 | 346.39 | 回退；不單獨保留 |
| 快取 + 小頻寬特化 | 約 539 | 特化有顯著收益 |
| 再加獨立計算佇列 | 約 572 | 佇列隔離有增益 |
| 再提前發出影象完成 semaphore | 812.87 | 解除解碼影象等待後續 metric 的依賴 |
| **最終（撤掉計劃快取）** | **811.85** | **保留** |
| 交錯複測原始基線 | 361.45 | 基線穩定 |
| CUDA + NVDEC 參考 | 851.16 | 最終 Vulkan 達其 95.38% |
| Vulkan decode-only 對照 | 約 828 | 包含最終 device idle，無計算；不是硬體理論極限 |

最終三次為 812.19 / 811.42 / 811.85 fps；複測基線為 361.52 / 361.34 / 361.45 fps。

快取在最終組合的 1001 幀篩選中沒有超出波動的收益；撤掉快取後重新做了完整 5001 幀驗收。不能把“省去上傳位元組”當作吞吐提升。

## 保留的實現

1. **inverse 頻寬特化**：同一 SPIR-V 透過 specialization constant 為 band=1/3/5/7 構造 pipeline，縮小 `recent` 視窗和固定迴圈。混合頻寬 tile、其他合法頻寬仍走通用 shader，保持原累加順序。
2. **分離計算佇列**：FFmpeg 仍只使用 compute family 的 queue 0。分析 slot 使用裝置支援的額外佇列；佇列數量不足時按實際數量共享，並對相同 queue 加鎖；只有一條 queue 時繼續共享 FFmpeg 的鎖。
3. **轉換後通知解碼器**：將原始影象訪問與後續 F32 分析拆成兩個 command buffer。第一段完成 luma conversion 後即可 signal AVVkFrame semaphore，FFmpeg 不再等待 inverse/forward/metric 才能繼續訪問 DPB。第二段保留正確的佇列內依賴和最終 fence。異常路徑等待已提交的 conversion，之後才銷燬 ImageView/釋放 frame。

原始碼雖然沒有 host 原始幀回傳，卻讓解碼等待了不再訪問解碼影象的後續計算。這是本次定位到的主要流水線問題。

## 驗證結果與邊界

- 所有 Vulkan 實驗階段的完整 5001 幀結果，包括 seq、frame_index、PTS、timestamp、error，排序後與基線完全一致；其中 2475 幀誤差非零。
- 高度單軸、寬度單軸、Bicubic p4、Spline64 p3、Lanczos8 p2、併發=1：全部逐幀一致；均未見吞吐回退。短視窗成績僅用於迴歸篩選，不與主表直接比較。
- 1080p HEVC Main10 合成輸入、p4、128 幀：與基線逐幀一致，僅作正確性覆蓋。
- 同一個 worker 連續三輪：取消 5001 幀任務，再完整跑 257 幀，均成功，zero-copy 保持。
- Vulkan CPU 對照測試（含新增單候選特化測試）、五份 SPIR-V 校驗、media worker 測試透過。
- 完整 CTest 預設模式 **22/24**；`GETNATIVE_GPU_STAGE_PROFILE=stages` 下 **23/24**。遺留失敗是 CUDA telemetry 測試假設：預設模式不累計 CUDA telemetry，worker 的 source residency 斷言和直接 CUDA baseline 的 staged launch 斷言因此失敗。worker 失敗已在未修改基線復現；CUDA 原始碼和測試均未修改，不宣稱全套測試透過。
- **完整 Vulkan validation 未透過**：基線與最終版本都報告 FFmpeg 共享裝置的既有錯誤，例如 synchronization2、samplerYcbcrConversion 未啟用、影片影象 usage/format 不匹配；啟用同步校驗時 worker 提前退出。本輪不聲稱透過完整 validation。其日誌與效能結果分開保留，後續需要修復共享裝置功能契約後重新驗證。
- 未驗證 Windows 打包產物、AMD/Intel 裝置或真實單佇列硬體；此次實測是 Linux RTX 5080。

## 復現

使用 `engine/bench/media_verify_benchmark.py`，分別傳入基線/修改後二進位制：

```sh
python3 engine/bench/media_verify_benchmark.py \
  /path/to/getnative-engine /path/to/00001.m2ts \
  --backend vulkan --axes h_plus_w --candidate 864 \
  --concurrency 8 --frames 5001 --repeats 3 --output /path/to/result.json
```

結果檔案儲存請求、執行時 provenance、完整逐幀結果和 telemetry，指令碼拒絕缺幀、重複幀及硬解回退。效能測量不啟用 validation/profiling。

證據摘要：

- 媒體 SHA-256：`72938cf98d3bc333b93ae1bc73faeb4d0b2a3399d835376c057e111d855130b5`
- 基線 binary SHA-256：`210f7ace42a44ecec2196d8abc03e32ffe043fe861c200c074628ed4ea625f38`
- 最終 binary SHA-256：`8d19123619226eea39964204cdeee2cb868b5212e43a428d19564d755ffe808f`
- 5001 幀排序、JSON canonical 編碼結果 SHA-256：`767af0230ef667f08fce387f902f22ae34ab2299df7eaac56844075729f788fe`

大型原始結果及影片未加入倉庫；以上保留測量摘要、輸入/二進位制雜湊和驗證邊界。
