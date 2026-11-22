# FAKE POD NANO PRO S31：媒体、内存、字体与 TF 卡协同实现

> 范围：Aster Firmware 的 ESP32-S31 / ESP-IDF 6.1 移植，配套 HiFiPlayer 0.4.28（ABI 4.5，RISC-V AOT）。本文基于 `fix/hifi-unlimited-autoplay` 分支的实现阅读，2026-11-22 整理。它是**源码实现说明**，不是性能保证或新的实机验收报告。
>
> APP 侧实现详见 [HiFiPlayer 技术实现说明](https://github.com/a505283042/aster-apps/blob/fix/hifi-unlimited-autoplay/HiFiPlayer/docs/implementation.zh-CN.md)。

## 1. 定位与现有文档

本文件回答：为什么可以把歌曲、字库、安装包和封面放在同一张 TF 卡，同时运行全屏动态歌词；哪些对象位于内部 RAM、PSRAM 或 TF 卡；系统如何降低并发竞争，以及尚未解决哪些问题。

已有资料：`docs/architecture.md`（跨平台架构）、`docs/fakepod-s31-hifi-design.zh-CN.md`（音频实施/实机记录）、`docs/package-installation.md`（双槽安装）、`docs/ui-display-clarity.md`（cpfont 生产）和 `docs/continuous-audio-implementation.zh-CN.md`（**另一条 Zephyr/ESP32-S3 连续音频路径**，不能直接当成 S31 原生媒体服务的读前缓存）。

## 2. 实际分层和关键接口

~~~text
TF 卡 /SD
 ├─ /music/*.mp3|*.flac|*.wav + 同名封面与歌词
 ├─ /aster/fonts/ui_XX.cpfont
 └─ /aster/apps/aster.hifi/.v0|.v1/{app.aot,manifest.json,assets/...}
                 │
      ┌──────────┴──────────┐
      │ Aster 系统           │ HiFiPlayer 独立 AOT APP
      │ 安装/隔离/ABI/调度    │ UI、歌词、封面、曲库索引
      │     │                │    │
      │     ├── 文件服务 ◀───┤ fs / assets / music
      │     ├── 字库/图形 ◀───┤ 字符绘制 / RGB565 surface
      │     └── 媒体服务 ◀───┤ play / seek / snapshot
      └──────┬───────────────┘
             ├─ 媒体任务 → 解码 → I²S DMA → CS43131
             └─ 显示快照 → 16 行 DMA bounce → QSPI → CO5300
~~~

- `systems/espidf/components/aster_runtime/runtime.c`：从 TF 卡加载 AOT、启动 WAMR、创建 APP 事件循环；显示帧有调速，不保证每帧都能达到请求 FPS。
- `systems/espidf/components/aster_runtime/files.c`：APP 文件句柄、路径解析和权限隔离。音乐/资产只读；`/assets` 映射到 APP 安装槽，不是系统根目录下同名文件夹。
- `systems/espidf/components/aster_runtime/graphics.c`：RGB565 surface、局部提交、渲染命令与 PSRAM 暂存。
- `common/audio/aster_media.c`：**APP 退出仍存活的原生媒体会话**，负责状态、解码、seek、暂停/恢复、输出。
- `systems/espidf/components/aster_audio/player.c`：向 APP 发布播放器能力；在本分支中还增加固件级自然 EOF 自动下一首（参见第 7 节）。
- `boards/fakepod_s31/idf/main/{storage.c,board_lcd.c}`：SDMMC 与 CO5300 的硬件路径。

独立 APP 通过版本化 Aster ABI 调用系统，不拥有 DAC、SDMMC 控制器或全局系统音频会话。卸载/升级 APP 不等于重刷固件。

## 3. 资源位置与预算（不可简单相加）

| 资源/缓冲 | 源码配置或容量 | 主要位置 | 说明 |
| --- | ---: | --- | --- |
| APP WAMR 内存 | `initial_memory=1,572,864`、`max_memory=2,097,152` | PSRAM 承载的线性内存 | HiFiPlayer 0.4.28 的 manifest 配置；不是整机 PSRAM 上限 |
| APP RGB565 画布 | 460×460×2 = 423,200 B（约 413.3 KiB） | APP 线性内存 | `graphics.c` 使用模块分配，属于 APP 预算的一部分 |
| LCD 保留快照 | 423,200 B | PSRAM | `board_lcd.c` 的 `snapshot`；不能与 APP 画布混为一块 |
| LCD DMA bounce | 460×16×2 = 14,720 B | 内部 DMA RAM | 逐 16 行提交；完成前不复用 |
| TF 读取 bounce | 每次 4,096 B | 内部 DMA RAM | `aster_fs_read_safe()` 在读取期间分配、使用、释放 |
| Media PCM 工作区 | 512×2×4 = 4,096 B | 外部内存 | 原生音频服务的解码输出工作区 |
| DAC 转换区 | 512×2×4 = 4,096 B | PSRAM | CS43131 native output 打开时申请 |
| 解码器动态分配 | 上限 768 KiB | 主要为 PSRAM | `DECODER_BUDGET` 上限，非恒定占用 |
| 系统 cpfont 字形数据 | 单字号 8–48 KiB，最多 192 个缓存条目 | PSRAM | 另计缓存对象、区间索引、scratch |
| AOT 文件、完整字体、歌曲 | 按文件大小 | TF 卡 | 不因存在于卡上而整体占用 RAM |

`CONFIG_SPIRAM=y`、`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`；WAMR allocator 调用 `heap_caps_aligned_alloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`。内部 RAM 仍需为任务、驱动、控制结构和 DMA 保留余量。实际 `heap_caps_get_free_size`、最大连续块和最低水位必须由**实机日志**给出，表中不是实测峰值。

## 4. TF 卡与屏幕：不同外设控制器，仍竞争系统资源

`storage.c` 使用 **4-bit SDMMC** 挂载到 `/SD`，`.max_files=32`、`.allocation_unit_size=16*1024`，挂载失败**不会自动格式化**。`board_lcd.c` 使用 **SPI2 QSPI** 驱动 CO5300（配置时钟 40 MHz）。I²S 使用 CS43131 的独立发送接口。

SDMMC 与显示 QSPI 并非同一个 SPI 外设，因此没有“SD 与屏幕独占同一个 SPI 控制器”的直接竞争；但仍共享 CPU、PSRAM 带宽、内部 DMA 资源和调度时间。

`systems/espidf/components/aster_render/port.c` 中的 `aster_fs_read_safe()`：

1. 临时分配 `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` 的 4 KiB bounce；
2. 每次最多 `fread()` 4 KiB；
3. `memcpy()` 到最终地址（允许 APP/PSRAM 目标）；
4. 完成后释放 bounce。

**此机制主要解决 DMA 内存适用性，不是异步预读取，也不提供全局音频 I/O 优先级。** 单次 `fread()` 本身仍会阻塞调用者。`files.c` 中 APP 调用的 `aster_fs_read` 也走安全读取路径。

LCD 的 `fakepod_lcd_present()` 将发生变化的画面写入 PSRAM `snapshot`，再按 16 行拷入内部 DMA bounce，提交后等待完成信号。局部刷新能够减少显示传输，但不会让 TF 变成并行设备。

## 5. 系统字库：大文件 + 小型元数据 + 按需缓存

`systems/zephyr/ui/font_sd.c` 与 `ui_font.c` 也被 ESP-IDF 渲染组件复用。

- TF 文件位置 `/SD:/aster/fonts/ui_12.cpfont` 至 `ui_48.cpfont`；请求字号匹配最近的预烘焙原生字号。
- 启动/发现阶段载入较小的“字符区间”元数据，不载入整份中文字形位图。
- 字形查找先检查 PSRAM 缓存；命中时不访问 TF 卡。未命中才读取记录与紧凑位图。
- 单字号位图缓存 `clamp(px*px*48,8192,49152)`，最多 `SPARSE_N=192` 条目，按 LRU 淘汰；另有解码 scratch、管理对象和元数据。
- `aster_font_sd_warm()` 提前加载 APP 标题、公共文本；相关接口是同步预热，不代表后台字体 I/O 线程。
- APP session 开始/结束时释放各字号字形缓存，保留稳定字体对象和区间元数据；降低桌面与 APP 累计占用。
- 对首次出现的大量陌生字符，仍可能产生同步 SD 冷读。可通过 `aster sys fonts` 检查字体命中与缓存信息。

不要把这套 cpfont 与 HiFiPlayer 自带的 `paper-serif.bin` / `paper-bold.bin` 混淆：后者是 APP 内实现的艺术歌词轮廓 + Atlas，见 APP 文档。

## 6. 原生音频：独立任务，不依赖 UI 心跳

`common/audio/aster_media.c` 启动名为 `aster_media` 的任务，独占本次播放器解码器及输出会话。工作循环：

~~~text
打开并解析 FLAC / MP3 / WAV
  → 获取源采样率/位深/长度
  → 打开目标输出端点
  → 每次解码 ≤512 帧 PCM
  → 写入 CS43131 I²S（如 EAGAIN 则有界重试）
  → 更新原子/锁保护的媒体快照
  → 自然 EOF：drain、关闭、标记 ENDED
~~~

APP 的 `play/pause/seek/stop/get_info` 不会让 UI 负责解码。实际 `aster_media` 任务使用 12,288 B 栈配置；播放任务由 `aster_os_task_start` 启动。`common/audio/aster_audio_decoder.c` 使用 `dr_flac/dr_mp3/dr_wav`；其动态内存有 768 KiB 预算，MP3 索引另有有限大小控制，避免无限增长。

S31 CS43131 原生 I²S 发送端（`systems/espidf/adapters/cs43131_i2s_pdm/port.c`）设置 `dma_desc_num=8`、`dma_frame_num=128`，每次 native write 在输出锁下送 ≤512 帧。96 kHz 时 512 帧约 5.33 ms；8×128 帧对应约 10.67 ms 的硬件 DMA 帧容量**级别**，不是实际剩余缓存时长，也不能当成抗 SD 阻塞保证。

音量控制：驱动 `volume_pending` 以原子变量接收新音量，由音频硬件持锁者在块边界前后应用 DAC 寄存器更新，UI 不应直接等待阻塞中的 DMA 发送。

**重要边界**：在这条 S31 原生 `aster_media` 路径上，当前没有“统一 SD 仲裁任务 + 大容量音频预读取环形队列”。另一份 `docs/continuous-audio-implementation.zh-CN.md` 的 16 KiB 预读是 ESP32-S3/Zephyr 连续音效路径，不能算作 S31 已实现功能。

## 7. 跨 APP 的自动下一首（功能分支）

`systems/espidf/components/aster_audio/player.c` 在 `fix/hifi-unlimited-autoplay` 分支新增 `ASTER_PLAYER_CAP_AUTO_NEXT`（bit 128）及 `hifi_autoplay` 任务：

1. 只对 `/SD:/music/` 或等价根目录音乐路径的本地媒体会话记录当前播放文件；
2. 观察到自然 `ENDED` 后延迟约 500 ms，让旧版 APP 有机会先接管；
3. 扫描 `/SD/music` **当前一层**目录查找下一首 MP3/FLAC/WAV；
4. 找到后交给 `aster_media_play()`；末曲可回到首曲，单曲不自动循环；
5. 显式停止、暂停和蓝牙接收不当成自然 EOF。

新版 APP 检测能力位后**不再自己执行 EOF 切歌**。这样退出 APP 后固件仍能继续播放。此功能的代码位于功能分支，不应将其表述成远程 `main` 已包含。

**限制**：上述后台扫描尚不递归目录；扫描顺序依赖 FAT 目录枚举，尚无通用的共享持久播放列表或排序协议。日志应区分 `system playlist enabled`、`next: ...` 与 `no successor for ...`。

## 8. 已缓解的竞争与当前风险

| 问题 | 已实现的保护 | 剩余风险 |
| --- | --- | --- |
| APP/音频生命周期耦合 | 原生 media worker 独立于 AOT APP | 下一个曲目如何选择仍受播放列表实现影响 |
| TF 直接 DMA 到不合适的地址 | 内部 4 KiB bounce 再拷贝 | `fread` 仍是同步阻塞 |
| LCD 全帧占内部 RAM | PSRAM snapshot + 16 行内部 DMA | 全屏快速刷新仍消耗显示带宽和 CPU |
| 多字号大字库 | cpfont 只保留索引、按需加载、LRU、会话回收 | 冷字形同步读取可能遇到 SD 尾延迟 |
| 音频硬件被 UI 抢用 | CS43131 独占、硬件锁、原子音量更新 | 过长阻塞仍可能导致 I²S 欠载 |
| 封面、字体、音频同时读取 | APP 端增量/切片工作，系统文件服务隔离 | 无全局 I/O 预算器，无音频缓存低水位调度 |
| 非正常安装/断电 | `.v0/.v1` 双程序槽，提交记录验证 | SD 故障、掉电与坏卡仍应实测 |

不能仅凭“播放时 UI 很流畅”就宣布 TF 卡无竞争；特别是 96 kHz FLAC、封面切换、大量冷字形以及背景资源读取同时发生时，仍需压测。

## 9. 验证与排障建议

- `aster app list`：查看已安装 APP、版本；`aster app run aster.hifi` 与 `aster app stop`：启动/停止界面。
- `aster sys mem`：关注内部/外部剩余量和最低水位；`aster sys fonts`：关注字形命中、冷读、缓存。
- `aster-audio status`（如当前固件启用诊断）：观察原生音频状态，注意不要把提交 PCM 帧数当成实际 DAC 无欠载的充分证据。
- 启动 `hifi_autoplay` 时可见 `system playlist enabled`，成功切歌可见 `next: ...`。
- 压测组合：96 kHz/24bit FLAC；每句包含新中文字；封面不同格式；反复切歌、Seek、进入/退出 APP；记录解码/SD 读取最大延迟、I²S 提交间隔、内存峰值和是否真实听到卡顿。
- 使用 TF 卡时重点看**最长一次读操作**，平均吞吐量高不代表长尾延迟低。
- 修改高采样率时钟应保留具体板级测试记录。开发者本地 96 kHz MCLK multiple 修复可能仅存在**未提交的工作区**，不要假设本分支默认包含其配置。

## 10. 后续架构建议（尚未实现）

按优先级：① 原生媒体服务增加有界音频读前缓冲和水位统计；② 统一 TF 读请求的分片/优先级与取消边界；③ 播放队列协议扩展为递归目录、统一排序和跨 APP 持久播放列表；④ 冷字形与封面 I/O 在音频低水位时主动退让；⑤ 长时间真实听音 + I/O 故障注入 + 内存回收压测。

如增加 96 kHz 双声道 S32 PCM 缓冲，200 ms 为 `96000×2×4×0.2=153600 B≈150 KiB`。这个数值只是**设计估算**，并非现有媒体路径已申请 150 KiB。

## 11. 关联代码

- `boards/fakepod_s31/idf/main/storage.c`、`board_lcd.c`、`board_pins.h`
- `systems/espidf/components/aster_runtime/{runtime.c,files.c,graphics.c}`
- `systems/espidf/components/aster_render/port.c`
- `systems/zephyr/ui/{font_sd.c,ui_font.c}`
- `common/audio/{aster_media.c,aster_audio_decoder.c,aster_audio_output.c}`
- `systems/espidf/components/aster_audio/player.c`
- `systems/espidf/adapters/cs43131_i2s_pdm/port.c`
- APP 侧 [HiFiPlayer 技术实现说明](https://github.com/a505283042/aster-apps/blob/fix/hifi-unlimited-autoplay/HiFiPlayer/docs/implementation.zh-CN.md)

> 文档维护准则：每次修改 I/O、播放器、缓存或 ABI 时，将“源码配置”“实机数据”“下一步建议”分开更新；同一段文字不要把其他板卡或未合并功能说成 S31 已有能力。
