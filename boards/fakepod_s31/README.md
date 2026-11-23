# FAKE POD NANO PRO S31 V1

460×460 CO5300 AMOLED、CST820 单点触摸、ESP32-S31、16 MiB Flash 和 16 MiB PSRAM。设备标识 `fakepod-s31`，应用构建目标 `riscv`。此板使用独立的 ESP-IDF 6.1 环境。资料中的 GPIO 对应 V1 接线。

## 构建

在固件仓库根目录加载 ESP-IDF 6.1 环境后执行。生产工程默认构建 Aster 系统，输出目录与鱼鹰 P4C5 分开。

```sh
idf.py --preview -C boards/fakepod_s31/idf -B "$PWD/build/fakepod-s31/system" -DIDF_TARGET=esp32s31 build
idf.py --preview -C boards/fakepod_s31/idf -B "$PWD/build/fakepod-s31/system" -p PORT flash
```

将 `PORT` 替换为下载接口的串口。设备端 Type-C 插头方向决定连接下载串口或原生 USB。USB 读卡器由板级选项 `CONFIG_ASTER_FAKEPOD_USB_STORAGE` 控制。主线默认关闭；**实验分支 `feat/fakepod-s31-usb-msc` 已开启该选项**。进入 FLUX 桌面「电源 → USB 读卡器」后，固件会先停止 APP／音频并卸载 TF 卡，再交由 Windows 独占访问，安全弹出后恢复设备访问。编译、串口直装和安全验证步骤见 [USB MSC 指南](USB_MSC_GUIDE.zh-CN.md)。

## 实机功能

- 首次联网准备 SD 字体与商城。设备运行 RISC-V AOT 应用，460×460 播放器通过商城或 SDK 串口安装。
- CS43131 有线输出支持 44.1、48、88.2 和 96 kHz 的立体声验证。系统播放器可播放本地 MP3、WAV 和 FLAC，具体位深和采样率取决于解码器与输出。
- 系统蓝牙支持手机向设备播放，以及设备向蓝牙音箱输出 SBC。播放器使用系统选定的输出。QMI8658 可用于摇动切换动态歌词。
- 上方实体键可短按熄屏或唤醒，持续按住约 3 秒关机；关机后用同一按键开机。熄屏期间可继续播放。

接线和适配器选择在 [板级注册](idf/aster_board.cmake)。诊断、已验证组合及仍待测项见 [Hi-Fi 设计与实施记录](../../docs/fakepod-s31-hifi-design.zh-CN.md)。


## HiFi 封面 JPEG 硬解＋PPA Blend（阶段 B，待实机验收）

固件在 S31 上新增可选的 `aster_img_render` V2 图像参数，**沿用已有 WASM 导入名**。V1 的 BMP/PNG/JPEG 软件渲染入口保持不变：

- 支持独立 JPEG、MP3 ID3 APIC、FLAC PICTURE 的 `src_offset + src_length` 精确读取（封面由 APP 定位，不解码整首音频）。
- JPEG 硬件引擎输出 MCU 对齐 RGB565 原图，缩放到不超过 460×460；用 PPA RGB565+A8 黑色蒙版生成压暗封面。PPA 不可用时原生 CPU 生成一次压暗图。
- **只在完整解码后**把原色和压暗两个结果从硬件专用 PSRAM 复制到受校验的 WASM 目标地址；不对 WASM 内存直接发 DMA。
- 单次硬件处理最大 2MiB 压缩 JPEG、210 万原始像素；超过限制、非基线/渐进式、硬件失败或 PSRAM 不足时，让 APP 原有 TJpgDec / V1 软件路径接管；绝不写坏旧封面。
- 对 SD 的 JPEG 读取每次最多 8KiB 并让出调度，避免长时间持有 TF 卡读取任务。UI 进度条刷新不执行解码或 Blend。
- 仅 `CONFIG_IDF_TARGET_ESP32S31` 链接 `esp_driver_jpeg`；其他板仍走 V1 软件路径。旧固件收到 V2 请求返回 `ASTER_EINVAL`，新版 APP 自动回退。

**升级次序：先备份原固件及 APP，编译/刷入新版 S31 固件，然后安装 HiFiPlayer 0.5.0-alpha.9。** 可单独将 APP 安装到旧固件验证软件回退；但未刷新版固件时不会启用硬件 JPEG/PPA。

验证命令（在 ESP-IDF 6.1 环境、固件根目录）：

```sh
python -m unittest discover -s tests -p test_hifi_cover_stage_b.py -v
idf.py --preview -C boards/fakepod_s31/idf -B "$PWD/build/fakepod-s31/system" -DIDF_TARGET=esp32s31 build
```

真机检查日志 `HiFi cover swap ... backend=JPEG_HW dim=PPA`、`hifi_cover_hw: JPEG ... PPA=yes`。单独测试普通 JPEG、MP3 APIC、FLAC PICTURE、渐进式 JPEG（回退）、无效/超大封面（回退）以及有线与蓝牙输出播放中连续切歌。**以上是代码实施记录，尚未宣称已通过板级编译、色彩、DMA 或连续音频实测。**



## 阶段 C：PPA Fill 自检与 CPU / DMA2D 复制隔离测试（性能已实测，功耗待测）

**生产渲染路径未修改。** S31 当前 LVGL/APP 的大面积不透明矩形仍由现有 PPA Fill 处理，只有通过启动校验才注册 LVGL Fill；图片/帧拷贝仍用 CPU `memcpy`。新增的 DMA2D **仅供用户手动发起的诊断**，不向 WASM 内存执行 DMA，不参与播放和正常界面刷新。

### 先确认 Fill

S31 的 `aster_ui_lvgl_init` 注册 PPA Fill；启动校验由原先的 32×16 局部 RGB565 填充扩展为两步：

1. LVGL 缓冲局部红色填充：校验区域内/外全部像素；
2. 128×64 对齐 PSRAM 缓冲里执行 96×48＝4,608 像素的 APP 大面积品红色填充，同时验证未填充的蓝色边框；这真实跨过现有 4,096 像素门槛。

失败即保留软件绘制（不能将本次判为 PPA 成功）。串口应看到：

```text
I (...) aster_ppa: RGB565/cache self-tests passed (LVGL + 4608px APP); fill ready
```

桌面空闲时输入 `aster ui bench 8`，结束时会打印 `UI PPA fills=... app_fills=... pixels=... errors=...`。请同时查看 `UI PERF` 与是否有画面异常。**计数大于 0 表示该条件下实际命中加速；计数 0 只说明该场景未命中，不代表自检失败。**

### 复制速度比较（不含功耗推断）

设备退出 APP、停止本地音乐与蓝牙音频后，在 Aster 串口控制台执行：

```text
aster ui stagec
```

诊断独立启动低优先级任务，使用 64B 对齐 PSRAM 源/目标缓冲与 ESP-IDF 6.1 **公开接口** `esp_async_color_convert_install_dma2d`、`esp_color_convert_blocking`（同格式 `ESP_COLOR_FOURCC_RGB16`，不做颜色转换）。记录 64²、128²、256²、448² 四组连续 RGB565 复制，CPU 和 DMA2D 交替测量，记录每次耗时和最小值，测量区外逐字节验证。

预期输出格式：

```text
STAGEC BENCH begin fill_ready=1 dma2d=1 memory=PSRAM
STAGEC BENCH edge=256 bytes=131072 reps=25 cpu_avg_us=... dma2d_avg_us=... cpu_min_us=... dma2d_min_us=... dma2d_available=1 pixel_match=1
STAGEC BENCH done; production copies remain CPU
```

数值必须来自实机，本文不预填任何结果。DMA2D 不可用会显示 `dma2d=0`，仍记录 CPU 基线；任意 `pixel_match=0` 视为失败，禁止启用 DMA2D 生产拷贝。该基准**仅是 PSRAM 到 PSRAM、cache-warm、固定对齐、完整矩形**的复制对比，不代表非对齐局部刷新、WASM 线性内存或 LCD SPI 传输。

### 外部功耗比较

```text
aster ui stagec-power idle
aster ui stagec-power cpu
aster ui stagec-power dma2d
```

每条命令各输出 `STAGEC POWER begin`、8 秒采样窗口、`STAGEC POWER end`（耗时、复制次数、处理字节和像素正确性）。在这 8 秒内用**外置供电电流表或功耗计**测平均功率；ESP-IDF 日志只记录时间和工作量，不能直接量出瓦特或毫安。保持供电电压、屏幕亮度、Wi-Fi/BT、温度和是否连接 USB 一致；每组至少重复三次，互换 CPU/DMA2D 测试顺序。

计算单位数据能量：`(P_test - P_idle) × elapsed_seconds / transferred_MiB`（J/MiB；若只测电流，先乘供电电压换算 W）。省电需比较能量/数据与整机平均功率，而不能把耗时更短等同于功耗更低。


### 阶段 C 首轮实机数据与 DMA2D 事务池修复（2026-11-23）

- S31 回报 `fill_ready=1 dma2d=1`；四组 `pixel_match=1`。平均耗时（CPU / DMA2D，微秒）：64² **46 / 150**；128² **181 / 444**；256² **2898 / 1467**；448² **9207 / 4138**。小块 CPU 更快；256² 及以上 DMA2D 约快 2 倍。此为**一次测试结果**，未测试真实整机功耗，不进入生产默认路径。
- **连续 DMA2D 压力测试异常**：`STAGEC POWER end mode=dma2d elapsed_us=473485 copies=117 err=259`，源于 ESP-IDF 6.1 的 `esp_color_convert_blocking` 在 ISR 给等待方信号后，DMA2D 驱动才回收事务对象；旧代码 backlog=1，连续提交可能在回收前触发 `ESP_ERR_INVALID_STATE`（事务池为空）。因此该次 0.47 秒样本**完全无效，不能用于能耗比较**。
- 修复后诊断改为 **2 个 DMA2D 事务槽**，只对临时 `ESP_ERR_INVALID_STATE` 最多有限重试两次、延后一个 FreeRTOS tick；打印 `dma_retries`；凡错误或未满 8 秒，统一输出 `verdict=INVALID`，禁止据此计算能耗。使用外部电流表重复空闲、CPU、DMA2D 三个模式，在每条出现 `STAGEC POWER end` 后再运行下一条（worker 并发拒绝是正常保护）。
- `aster ui bench 8` 与 `aster ui bench`（默认 8 秒）均受支持。遇到偶发 `Unrecognized command` 时，等串口日志输出完成、出现 `aster:~$` 提示符后重新输入。

**验收决策：** 只有 RGB565 全部正确、无音频/触摸/刷新退化，且目标尺寸下 DMA2D 在多轮测试中持续明显快于 CPU，并且 J/MiB 不高于 CPU 时，才考虑针对该尺寸启用。小块、非对齐、WASM 目标和失败重试继续 CPU；不能证明收益时，阶段 C 以“保留当前实现”验收。绝不以单次缓存命中成绩决定生产切换。

固件测试与构建（先别刷机）：

```powershell
cd D:\ESP32\aster-firmware
git pull --ff-only origin main
python -m unittest discover -s tests -p "test_stage_c_copy.py" -v
python -m unittest discover -s tests -p "test_hifi_cover_stage_b.py" -v
idf.py --preview -C boards/fakepod_s31/idf -B D:/ESP32/aster-firmware/build/fakepod-s31/system -DIDF_TARGET=esp32s31 build
```

如 S31 IDF 6.1 构建或公开 DMA2D 接口与已安装框架有差异，以本地编译错误为准修复，**不得在未编译成功时烧录**。

### 阶段 C Fill CPU/PPA A/B 实测结论（2026-11-23）

实机交替 8 秒基准，按后两组可比数据计算：

| 指标 | CPU Fill（2 轮平均） | PPA Fill（2 轮平均） |
|---|---:|---:|
| render_avg_us | 44,617 | 44,157 |
| render_max_us | 66,819 | 66,628 |
| interval_p95_ms | 67.5 | 66.5 |
| FPS | 约 18.3 | 约 18.5 |
| 每轮 PPA LVGL fills | 0 | 211 |
| 每轮 PPA 像素 | 0 | 约 16,638,000 |
| errors | 0 | 0 |

PPA 比 CPU 每帧约节省 **460μs，约 1.03% 的平均 render 耗时**。这是这组实际 A/B 的观测差值，不足以宣称其它 UI 负载也有稳定 1% 收益；不同轮之间字体/艺术缓存状态存在差异。保持原有 4096 像素门槛和硬件 PPA Fill，**不扩大覆盖率，不移除 CPU 回退**。

下一阶段优先定位较大开销，不要盲目把独立 PSRAM→PSRAM DMA2D 测试结果套用到真实渲染：

- `flush_us` 约 3.38–3.40 秒/8 秒；屏幕每轮刷新约 313–316 个矩形、总计约 33.8–34.0 MB。这是**累计同步 LCD 提交时间**，与 LVGL `render_avg_us` 可能存在计时包含关系，二者不可直接相加。
- S31 `board_lcd.c` 真实路径为先复制至保留的 PSRAM 快照，再对 16 行小块做 RGB565 字节交换、SPI DMA 提交并同步等完成。建议先分开记录 snapshot-copy、byte-swap、SPI wait 的耗时及 flush stripe 次数，确定瓶颈后再讨论双缓冲流水线，不先改变 40MHz QSPI 或颜色约定。
- `ribbon_us` 约 1.54–1.55 秒/8 秒（约 10.4 ms/帧），对应 FLUX 动态丝带；应与上述 LCD 开销分别剖析。
- 此前的 S31 `stagec` 仅证明对齐 PSRAM→PSRAM 同格式整块复制 DMA2D 在 ≥128KB 较快，**不证明** RGB565 字节交换、WASM 数据或 LCD DMA 有相同收益；功耗仍缺外部测量。

**当前决策：** 阶段 C 结束。保留 PPA Fill 和 DMA2D 隔离基准；生产复制路径保持 CPU。下一步为 LCD 提交和丝带动画分段性能诊断，先不改实际显示/解码功能。

### 阶段 C 实测结论与下一步 Fill A/B（2026-11-23）

- 真实 UI 连续两次 8 秒基准：PPA Fill 命中 207/211 次，处理像素 16,399,642 / 16,640,209，错误均为 0。平均渲染耗时约 45.07 / 44.64ms，实际平均 18.3 / 18.5FPS。**说明 Fill 正确参与渲染，但还没有 CPU-only 对照，不能单凭命中判断加速收益。**
- 无 UI Benchmark 重叠的复制测量：64²（8KiB）CPU 45us / DMA2D 159us；128²（32KiB）CPU 184us / DMA2D 446us；256²（128KiB）CPU 2880us / DMA2D 1477us；448²（392KiB）CPU 8907us / DMA2D 4100us，全部 `pixel_match=1`、`dma_retries=0`。
- 448² 连续约 8 秒：CPU 916 次、367,689,728B、约 45.92 MB/s；DMA2D 1,984 次、796,393,472B、约 99.54 MB/s。两组 `verdict=VALID`，吞吐 DMA2D 约为 CPU 的 2.17 倍。但缺少外部电流/功耗，尚不能判定 DMA2D 更节能；**生产复制仍是 CPU。**
- 现有大复制热点涉及 WASM 目标、RGB565 字节交换和 LCD 传输，不能因为独立 PSRAM→PSRAM 基准就直接替换为 DMA2D。

#### 新增：同一真实 UI 场景下的 PPA Fill / CPU Fill A/B

更新固件后，设备退出 APP、停止本地和蓝牙播放，在串口命令行**逐条执行并等到 `UI BENCH done` 后再运行下一条**：

```text
aster ui bench-cpu 8
aster ui bench-ppa 8
aster ui bench-cpu 8
aster ui bench-ppa 8
```

- `bench-cpu` 只在这一次 UI Benchmark 期间屏蔽 PPA Fill 的任务选择（以及原生 gfx Fill 加速入口），强制原有 CPU 绘制。
- `bench-ppa` 使用已通过 RGB565 自检的默认 PPA Fill。旧 `aster ui bench 8` 保持原行为。
- 两种模式都有 2 秒预热 + 8 秒测量；分别输出 `UI BENCH FILL mode=CPU/PPA`、`UI PERF`、`UI PPA fills` 和 `UI BENCH RESULT fill_mode=...`。结束自动恢复生产 PPA Fill。
- 对比 FPS、`render_avg_us`、`render_max_us`、`interval_p95_ms`、`flush_us`、`UI PPA errors`。CPU 模式 PPA fills=0 是**预期值**；PPA 模式应大于 0 且 errors=0。
- 四轮交替而非仅各一次，是为了控制温度、缓存与复杂动画场景导致的偏差；任何模式发生画面错误时不得继续扩大硬件使用范围。

后续决策：只有 PPA 模式在可重复的真实 UI 负载下明显优于 CPU，才保留当前门槛/评估调整阈值；无明显收益则不盲目提高加速覆盖率。


## 阶段 D：LCD Flush 分段性能诊断（实测待完成）

阶段 C 的 CPU / PPA Fill A/B 实机结果：CPU Fill 平均 44,617us/帧；PPA Fill 平均 44,157us/帧，改善约 1.03%。因此不扩大 PPA Fill 使用范围，优先定位 LCD Flush 和 FLUX 动态丝带的开销。

本阶段**只增加低侵入的定时计数**，不修改 S31 CO5300 的 40MHz QSPI、16 行 DMA bounce buffer、单事务队列、RGB565 字节顺序、屏幕唤醒及屏幕快照逻辑；未引入 DMA2D 或双缓冲。新增弱链接的 board LCD profile 钩子，仅在 `aster ui bench` 预热结束后开启，8 秒测试结束自动停止；APP 打断基准时放弃本次统计并关闭计时。其他板没有板级钩子时 UI 正常工作。

### 编译（Windows PowerShell）

```powershell
cd D:\ESP32\aster-firmware
git pull --ff-only origin main
python -m unittest discover -s tests -p "test_stage_d_lcd_profile.py" -v
python -m unittest discover -s tests -p "test_stage_c_copy.py" -v

& "D:\ESP32\aster-idf61-python\Scripts\python.exe" `
    "$env:IDF_PATH\tools\idf.py" --preview `
    -C boards/fakepod_s31/idf `
    -B D:/ESP32/aster-firmware/build/fakepod-s31/system `
    -DIDF_TARGET=esp32s31 `
    build
```

编译通过后刷入固件；无需修改或重编 HiFiPlayer APP。

### 实机测试

退出 APP、停止后台音频，串口执行 `aster ui bench 8`，等 `UI BENCH done` 后再执行 `aster ui bench 8` 复测。若希望同时比较 CPU/PPA Fill，分别执行 `aster ui bench-cpu 8` 与 `aster ui bench-ppa 8`，同样须逐条完成。

8 秒测量会额外输出：

```text
LCD PROFILE rects=... stripes=... snapshot_bytes=... wire_bytes=... copy_us=... swap_us=... submit_us=... wait_us=... wait_max_us=... errors=...
UI PERF frames=... render_avg_us=... flush_us=... ribbon_us=...
```

- `rects` 是提交的 LCD 脏矩形次数，`stripes` 是 16 行分块数。
- `snapshot_bytes` 是从 LVGL/native RGB565 帧缓冲复制到 PSRAM 保留快照的字节数；`copy_us` 是其累计 CPU 时间。
- `swap_us` 是读 PSRAM 快照并写入内部 DMA bounce buffer、交换 RGB565 字节序的累计 CPU 时间。
- `submit_us` 记录 `esp_lcd_panel_draw_bitmap` 调用耗时（可能包含队列/命令提交等待）。
- `wait_us` 是提交后在完成信号量上的阻塞时间，**包含 DMA 传输与任务唤醒延迟**，不能直接称为纯 SPI 线上传输时间；`wait_max_us` 是最慢一次等待。
- `wire_bytes` 是经过 2×2 像素单元对齐后实际提交给面板的 RGB565 字节数，可能高于 `snapshot_bytes`。
- `errors` 为 LCD 分块提交失败计数；正常必须等于 0。

判断下一步要根据 `copy_us` / `swap_us` / `submit_us` / `wait_us` 的比例和 `stripes` 数量，而不是仅用 `flush_us` 断言 SPI 是瓶颈。`flush_us` 是更外层调用的累计耗时，两者存在包含关系，**禁止相加**。

所有数字必须由新固件实机输出，以上只说明指标，不预填实测值。若显示异常或编译失败，保留原来的显示路径，勿直接试验双缓冲/改高 SPI 时钟。


## 阶段 E：16 行 LCD Ping-Pong 双缓冲实验（默认关闭，待实机 A/B）

### 实施范围

只在 S31 串口明确运行 `aster ui bench-pipe 8` 时，尝试申请第二块内部 DMA 缓冲（460 × 16 × 2 = **14,720 字节**）。正常开机、播放、旧 `aster ui bench 8`、`bench-cpu`、`bench-ppa` 继续原本的**单缓冲同步等待**实现。

- 单缓冲原路径、RGB565 高低字节交换、CO5300 2×2 像素对齐、40MHz QSPI、16 行条带与 **trans_queue_depth=1** 完全保留。
- 流水线一次最多只有**一个 DMA 事务在途**：准备 A → 提交 A → CPU 在独立 B 上准备下一块 → 收到 A 的完成信号 → 提交 B → 在 A 上准备下一块，依此类推。
- 第二块内存分配失败时输出 `LCD PIPE fallback=single reason=internal_dma_ram` 并使用原单缓冲。A/B 测量必须看到 `LCD PIPE enabled=1` 才能认定实际使用了流水线。
- 串口测试结束自动关闭流水线并释放第二缓冲。若 APP 启动打断测试，同样关闭。**如果 DMA 提交失败或完成回调超时，不能假设硬件停止读取缓冲：固件故障保护会停止新的 LCD 提交、保留缓冲直到重启**，而不是冒险立即释放或复用 DMA 缓冲。
- 不影响 WASM 内存，不改变音乐 APP、音频解码或 PPA Fill 的生产路径。尚未进行本地编译/实机验证，绝不能提前将流水线设为默认模式。

### 本地编译

沿用本机的 ESP-IDF 6.1 环境，在 PowerShell 执行：

```powershell
cd D:\ESP32\aster-firmware
git pull --ff-only origin main
python -m unittest discover -s tests -p "test_stage_e_lcd_pipeline.py" -v
python -m unittest discover -s tests -p "test_stage_d_lcd_profile.py" -v
python -m unittest discover -s tests -p "test_stage_c_copy.py" -v

& "D:\ESP32\aster-idf61-python\Scripts\python.exe" `
    "$env:IDF_PATH\tools\idf.py" --preview `
    -C boards/fakepod_s31/idf `
    -B D:/ESP32/aster-firmware/build/fakepod-s31/system `
    -DIDF_TARGET=esp32s31 `
    build
```

### A/B 测试

退出应用，停止后台音乐、蓝牙收发；烧录新版固件后逐条运行，并**等待每次 `UI BENCH done` 后才输入下一条**：

```text
aster ui bench 8
aster ui bench-pipe 8
aster ui bench 8
aster ui bench-pipe 8
```

每轮已有 `UI PERF`、`LCD PROFILE`；新增：

```text
LCD PIPE requested=1 enabled=1 fault=0 rects=... stripes=... fallback_rects=0 failures=0 prep_after_submit_us=...
```

`requested=1 enabled=0` 表示申请流水线失败、实际回退了单缓冲；该组不能当双缓冲成绩。`prep_after_submit_us` 是从 DMA 提交后、开始等待信号量前准备下一块的总耗时，**不证明每个微秒都与真实 SPI DMA 活动重叠**；重点以 `flush_us`、`render_avg_us`、FPS、`errors` 的真实 A/B 对照评价收益。

注意：流水线的 `LCD PROFILE wait_us` 只包括准备下一块*后*尚需等待的时间；因此与旧模式的 wait_us 含义略有不同，不能用它直接推断传输变快。对比外层 `flush_us` 与完整端到端帧耗时才有意义。

验收门槛：流水线连续多轮 `failures=0`、`errors=0`，无花屏、像素错位、熄屏唤醒异常、后台音乐卡顿；`flush_us` 建议至少稳定下降 10% 且 UI 帧耗时/FPS 改善，才考虑下一步生产化。否则保留原单缓冲。 

## Stage F1/F2：LCD 双缓冲默认启用，支持运行时回退

F1 已将 Stage E 的 16 行 LCD Ping-Pong 扩展到 **Aster 桌面和运行中的 WASM APP**，并通过 S31 实机基本开关及 HiFiPlayer 启用验证。F2 将它设为 **S31 启动时默认开启**；不会写 NVS，手动 `off` 在本次运行有效，**重启后重新尝试启用双缓冲**。无需重新编译 HiFiPlayer APP。

通过 S31 设备的串口控制台运行：

```text
aster ui pipe status
aster ui pipe on
aster ui pipe status
aster ui pipe off
aster ui pipe status
```

- 启动时在 UI/APP 工作线程输出 `UI PIPE boot default=ON runtime=ON requested=1 enabled=1 fault=0`。如果无法分配第二块内部 DMA 缓冲，会明确输出 fallback 并继续使用单缓冲，不阻止启动。
- 手动命令先输出 `UI PIPE queued action=...`，表示进入 mailbox；**只有 UI/APP 工作线程**会修改 LCD 双缓冲状态，切换完成后输出 `UI PIPE runtime=ON/OFF requested=... enabled=... fault=... app=... bench=...`。
- 成功启用必须看到 `runtime=ON enabled=1 fault=0`；第二缓冲使用 14,720 字节内部 DMA RAM。`off` 在同步 Flush 边界关闭并释放第二缓冲。APP 启动/退出不再改变 F1 的运行时选择。
- 申请失败将输出 `LCD PIPE fallback=single reason=internal_dma_ram`，实际运行时保持 OFF。若 DMA 完成状态未知，原 fail-closed 机制仍会禁止新 LCD 提交并保留 DMA 可能持有的缓冲，`off` **不能清除硬故障**，需要重启。
- `aster ui bench 8` 和 `aster ui bench-pipe 8` 会临时覆盖运行时模式，结束或被 APP 打断时恢复此前选择；进行 Benchmark 时拒绝新的 `pipe on/off` 请求。
- 保持 CO5300 40 MHz QSPI、16 行条带、2×2 对齐、RGB565 字节交换、`trans_queue_depth=1`，始终至多一个 DMA 提交在途。未修改音频任务、APP 接口或 Flash 数据格式。

**当前已证明桌面和 HiFiPlayer 内可手动启停，但不代表全部长时可靠性场景已完成。** 用户选择默认开启后，不再反复进行 Stage E 的 A/B Benchmark；日常使用中若出现新的花屏、唤醒异常或音频卡顿，先通过 `aster ui pipe off` 回到单缓冲确认是否为新引入。`fault=1` 时保护 DMA 缓冲，须重启，不要继续调用显示驱动。先前存在的切歌起始短暂卡顿/爆音属于独立的音频问题，不与 LCD F2 混改。

### Windows 本地检查与构建

```powershell
cd D:\ESP32\aster-firmware
git fetch origin
git switch -c feat/s31-stage-f1-ui-pipe-runtime-20261123 --track origin/feat/s31-stage-f1-ui-pipe-runtime-20261123
python -m unittest discover -s tests -p "test_stage_e_lcd_pipeline.py" -v
python -m unittest discover -s tests -p "test_stage_f1_lcd_runtime.py" -v

& "D:\ESP32\aster-idf61-python\Scripts\python.exe" `
    "$env:IDF_PATH\tools\idf.py" --preview `
    -C boards/fakepod_s31/idf `
    -B D:/ESP32/aster-firmware/build/fakepod-s31/system `
    -DIDF_TARGET=esp32s31 build
```

如果本地已经建立同名分支，请先 `git switch` 到该分支，再执行 `git pull --ff-only`；不要在有未提交修改的目录里覆盖源码。更新后只需一次构建、烧录与开机 `aster ui pipe status` 冒烟确认，不要求重新做多轮 A/B。


## Stage H1：FLAC 压缩音频预取环，供后台封面调度使用

参考 FakePod_S31 的 buffered_sd_audio_source.cpp 与 artwork_loader.cpp，先把 Aster 的 S31 FLAC **直接读取 TF 卡**改为后台预取方式。本阶段是实际代码改动，并非仅调整封面延迟参数。

### 实现

- `hifi_preload` 工作任务使用**独立文件句柄**，每块读取至多 8 KiB，累计 4 块后让出一个 RTOS tick；将压缩 FLAC 字节写入 128 KiB PSRAM 环。现有 dr_flac 解码器只读环，不会与预取任务同时使用相同文件句柄。
- 任务优先级采用 Aster OS 数值 19（现有 media 解码工作线程为 18，即音频解码更优先），不增加 PCM 队列；环锁不会跨越文件系统读取持有。
- FLAC Seek 会终止并安全 join 旧生产者，然后在原有 decoder 文件上执行 seek；下次读取 PCM 时按新位置重新启动预取。切歌和关流同样先 join，再回收 PSRAM/文件资源。**禁止强删正在读取 FATFS 的任务**。
- 内部 DMA/PSRAM 分配或创建任务失败时自动回退原始直接读卡方式。MP3/WAV、蓝牙发送/接收、HiFiPlayer WASM、JPEG/PPA、默认 LCD 双缓冲路径均未修改。
- `aster_audio_prefetch_window()` 用原子快照返回缓存字节数、容量、EOF、等待次数与最长等待，以及错误状态；下个阶段后台 ArtworkTask 将据此控制可读取 TF 卡的窗口。**目前尚未接入封面任务**。
- 串口命令 `aster-media prefetch` 可查看 `active`、`buffered/131072`、百分比、`waits`、`wait_max_ms` 和 `error`。此状态查询无文件 I/O；播放 MP3/WAV 或音乐停止时 `active=0`。
- 缓存空时解码读取最多等待 2000 ms，超时报错以免永久阻塞；若读取速度长期低于消费速度，仍可能音频断供，后续应以实际低水位与等待计数确认。

### 构建与快速验证（保留 main 不变）

```powershell
cd D:\ESP32\aster-firmware
git fetch origin
git switch -c feat/s31-h1-audio-prefetch-ring-20261123 --track origin/feat/s31-h1-audio-prefetch-ring-20261123
python -m unittest discover -s tests -p "test_s31_audio_prefetch.py" -v
```

在已配置的 ESP-IDF 6.1 / S31 环境下按照原流程 build、flash。先做本地 FLAC 顺序播放、Seek、连续切歌，以及停止音乐/返回桌面即可。此阶段不要求重复 LCD A/B。测试是源码约束 + host 实际环形数据/取消/EOF/资源释放，Windows 上如无本机 C 编译器则使用现有 RISC-V GCC 做语法检查，不能视作实机功能已通过。

后续 H2 要在固件中把 ArtworkTask 与硬件 JPEG/PPA 放进独立后台任务，通过水位控制 SD 分块读取，并提供 APP `submit/poll/acquire` 接口。必须避免后台线程保存已退出的 WASM 线性内存指针。
