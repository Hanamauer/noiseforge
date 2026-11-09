NOISEFORGE

一个 VST3 噪音合成器乐器插件（64 位 Windows），包含两个完全独立的噪音引擎、一个随机波表振荡器和一个 6 路调制矩阵。面板是白纸黑墨的印刷风格（xerox 颗粒、反向黑块、等宽数值），没有渐变和位图，全部由 GDI+ 的 C API 现画。

安装
把 dist\NoiseForge.vst3 整个文件夹复制到：

代码块


C:\Program Files\Common Files\VST3\
或任何宿主扫描的 VST3 目录。在宿主里它以 NoiseForge 出现在"乐器 / Generators"分类下（Instrument | Synth），可以直接用钢琴卷帘触发。

构建
需要 MSYS2 的 MinGW-w64 g++（C:\msys64\mingw64\bin），无 CMake：

powershell


powershell -ExecutionPolicy Bypass -File build.ps1            # 插件 + 全部测试
powershell -ExecutionPolicy Bypass -File build.ps1 -ToolsOnly # 只构建自测工具
构建脚本会依次运行：

build\selftest.exe —— 引擎离线自测（15 项检查）
链接 NoiseForge.vst3
build\host_test.exe —— 以宿主方式加载 DLL，检查工厂 / 总线 / 参数 / MIDI / 状态 / 编辑器（35 项检查）
build\gui_render.exe —— 离线渲染面板到 docs\noiseforge_ui.png，并验证缩放、隐藏/显示和编辑器重载后仍能正常绘制
注意：运行任何测试 exe 前需要 $env:PATH='C:\msys64\mingw64\bin;'+$env:PATH，否则缺 DLL 返回 -1073741515。

三个音源
HARSH NOISE —— 粗噪音（宽带）
8 种噪声类型：WHITE / PINK / BROWN / BLUE / VIOLET / VELVET / CRACKLE / DIGITAL，后接谐振滤波、波形折叠、位 crushers 和采样率降频。每 voice 独立 RNG，永不重复。

BAND NOISE —— 指定频段噪音（频谱合成）
直接正弦部分音合成，不是滤波：在任意频段内随机放置最多 96 个正弦部分音。因为是合成而非滤波，频段可以完全位于 20 Hz 以下（最低 0.05 Hz）——没有建立时间、没有 DC 漂移。中心频率、带宽、部分音数量、音高跟随、漂移均可调。

WAVETABLE OSC —— 随机波表振荡器
8 表 × 8 帧 × 4 级 mip，运行时由谐波谱生成（参考 Vital 的 WaveSource + Fourier frames 思路）。每个实例用非确定性种子生成自己的一套波表；DICE 按钮（或按 D）重建整套波表并原子换入，不打断音频线程。种子随工程状态保存，恢复工程即恢复同一套波表。

MIDI
Note On 触发；波表振荡器与 BAND 引擎跟随音高（BAND KEY），HARSH 引擎本身不做音高跟踪——要在粗噪音上做音高感，请在调制矩阵里把 KEY 接到 HARSH CUT。Note Off 进入 release；Poly Aftertouch 驱动 AFTER 源。8 复音；不足时偷取最旧 voice，40 ms 淡出后重新触发排队的音符。MIDI 事件按 sampleOffset 处理，不是整块对齐。

防点击设计
Attack 最低 8 ms（线性）
Release 最低 12 ms（指数，60 dB 定义）
voice 偷取 40 ms 淡出
master 20 ms 淡入
输出 soft-knee + 硬限幅， Ceiling 是真实上限
调制矩阵（6 路）
源：ENV1 / ENV2 / LFO1 / LFO2 / RAND / VEL / KEY / GATE / AFTER 目标：PITCH / WT POS / BAND C / BAND W / HARSH CUT / HARSH DRV / CRUSH / PAN / LEVEL

面板
白纸 + 黑墨：纸纹与扫描线是运行时生成的噪点，标题是黑底反白条，参数值是等宽字体；旋钮为墨线圆环 + 指针，悬停整块反白。支持 1100×688 到 2560×1600 缩放，窗口缩小时整块重采样。

参数（71 个）
分组	内容
VOICE	Glide, Vel>Amp, Vel>Env, Octave（7 档步进）
ENVELOPES	ADSR、ENV2 A/D/AMT、Gate Length
WAVETABLE OSC	On, Table, Position, Level, Pitch, Fine, Key, Pan, Drift
HARSH NOISE	On, Type, Level, Color, Cutoff, Reso, Drive, Fold, Bits, Down, Pan
BAND NOISE	On, Centre, Width, Level, Partials, Key, Drift, Pan
MODULATION	LFO1/2 Rate/Shape/Sync, Rand Rate/Smooth, 6 路路由
OUTPUT	Drive, Width, Output, Ceiling, Bypass
文件结构
代码块


noiseforge\
  src\NoiseForgeCore.h/.cpp    DSP 核心（参数表、RNG、包络、三个音源、引擎）
  vst3\NoiseForgeVST3.cpp      VST3 外壳（参数注册、MIDI、state、工厂）
  vst3\NoiseForgeEditor.h/.cpp GDI+ 编辑器（1280x800，无 VSTGUI、无资源文件）
  tools\selftest.cpp           引擎离线自测
  tools\host_test.cpp          宿主模拟测试
  tools\gui_render.cpp         面板离线渲染（PNG）
  build.ps1                    构建脚本
  docs\noiseforge_ui.png       面板截图
线程模型
音频线程：process() / noteOn() / noteOff() / setParam() —— 无分配、无锁
UI 线程：窗口过程，只通过 IParamSink 通信
宿主/自动化：setParamNormalized() → 原子变量 + 窗口消息
编辑器永远不碰引擎，音频线程永远不碰编辑器；唯一的跨线程状态是波表双缓冲（原子索引）、参数原子数组和示波器环形缓冲。
