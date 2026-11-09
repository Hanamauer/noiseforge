# NOISEFORGE v1.1 — 修复记录（Pulse Demon 版）

**日期**：2026-11-09
**范围**：`src/NoiseForgeCore.{h,cpp}`、`vst3/NoiseForgeVST3.cpp`、`vst3/NoiseForgeEditor.cpp`（整体重写）、`README.md`
**验证**：`selftest.exe` 全过、`host_test.exe` 35 项全过、`gui_render.exe` 渲染 + 缩放/隐藏/重载全过、离线 A/B 测量、**干净 PATH 下 `LoadLibrary` 成功**。

---

## 0. 宿主里显示 error / 类型未知（v1.1.1 追加修复）

**原因**：链接时只用了 `-static-libgcc -static-libstdc++`，DLL 仍然动态导入 MinGW 的 `libwinpthread-1.dll`。普通机器上没有这个 DLL，宿主扫描时 `LoadLibrary` 直接失败，于是 FL Studio 把它标成 `error` / 类型 `(未知)`。

**修复**：链接改成 `-static`（`build.ps1` 与 `build_local.ps1` 同步修改）。现在导入表只剩 `KERNEL32 / USER32 / GDI32 / gdiplus / ole32 / msvcrt`，没有任何 MinGW 运行时。

**验证**：把 PATH 清成 `C:\Windows\System32;C:\Windows` 后 `LoadLibraryW` 成功并找到 `GetPluginFactory`。

> 安装时注意：VST3 必须是**文件夹** `NoiseForge.vst3\Contents\x86_64-win\NoiseForge.vst3`，不能把里面的 DLL 直接改成 `NoiseForge.vst3` 平铺进去。装完在 FL 里"重新扫描插件"。

---

## 一、面板：白纸黑墨

整体改成印刷/复印件质感，不再是深色面板：

- **纸**：运行时生成的纸纹（噪点 + 每 4 行一条扫描线 + 少量墨点），叠 5 处低透明度墨污；四角有印刷套准十字。
- **字**：Impact 大标题（`NOISE` 实心 + `FORGE` 反白黑块）、Bahnschrift 标签、Consolas 数值；全部大写。
- **控件**：旋钮 = 墨线圆环 + 5 个刻度 + 3 px 值弧 + 指针，悬停时整块反白；开关 = 黑块反白（ON）或白底黑框（OFF）；选项 = 黑印章反白，`OFF` 槽除外；调制量 = 墨线滑杆 + 方块手柄。
- **图表**：示波器/频谱/波表全部白底黑线，点状网格，频谱柱带间隙；削波时标题栏出现红底 `CLIP`。
- **信息区**：条码由当前波表种子生成，随 DICE 变化。

**布局**（1280×800，四列面板，全部重排，无重叠、无出界）：

| 区域 | 位置 |
|---|---|
| 头部 | 0–58，黑规则线 58/62 |
| 示波器 | 12,66 612×174 |
| 波表 | 632,66 636×174 |
| VOICE / BAND | 12,246 288×130 / 12,384 288×380 |
| ENVELOPES / HARSH | 308,246 288×190 / 308,444 288×320 |
| WAVETABLE OSC / OUTPUT / INFO | 604,246 288×268 / 604,522 288×160 / 604,690 288×74 |
| MODULATION | 900,246 368×518 |
| 状态栏 | 770–800 |

顺带修掉的 UI 缺陷：UTF-8 乱码（"·"→"Å"）、MOD 面板压字、BAND/HARSH 末行压边框、药丸与旋钮基线不齐、点标签拖不动旋钮、丢失鼠标捕获不结束 automation 手势、滚轮命中没减 letterbox 偏移、最小尺寸过小、每帧新建 HBITMAP。

---

## 二、DSP / 外壳修复

| 编号 | 问题 | 修复 | 验证 |
|---|---|---|---|
| P0-1 | 整体音高 +9 半音 | `(v.note - 60)` → `(v.note - 69)`，滑音起点同步 | note 60：440 → **261 Hz**；note 69：740 → **440 Hz** |
| P0-2 | 同音重触发爆音 | 只有新 voice 才随机相位 | 二阶差分 0.85 → **0.057**（基线 0.055） |
| P0-3 | BAND 一动就爆 | `rebuild(..., keepPhase)`：已有 partial 保相位/幅度，新 partial 淡入 | 400↔420 Hz：0.90 → **0.041**；DRIFT 0.25：0.905 → **0.038** |
| P0-4 | 40 ms 偷取淡出是死代码 | `allocVoice` 真正置 `stealing`，排队分支生效 | 偷取点跳变 0.29 → **0.01**，第 9 音按队列重触发 |
| P0-5 | 调制矩阵 6 路全失效 | 去掉对已映射值的二次 `mapParam` | 6 条路由 `sum\|a-b\|` 0 → **1.5 万~2 万** |
| P1-1 | VEL>ENV 死旋钮 | 接到 ENV2 深度 | 0 vs 1 输出不再相同 |
| P1-2 | Bypass 硬静音 | 8 ms 增益斜坡 | 切换后 8 ms 内平滑、之后 0.0000 |
| P1-3 | 滑音起点错 | 用含移调/微调/键跟踪的目标音高初始化 | OCTAVE+1+GLIDE：470→827 变 **530→523** |
| P1-4 | 两实例噪声完全相同 | `reset()` 用实例种子混合 voice 序号 | 两个实例不再逐样本相同 |
| P1-5 | OCTAVE 连续、显示不符 | `mapParam` 对声明 steps 的旋钮量化；参数 `stepCount` 同步 | 显示值与实际值一致 |
| P2-1 | 44.1k 下 BAND 混叠 | 上界取 `min(23500, sr*0.45)` | 44.1k 不再合成到 Nyquist 以上 |
| P2-3 | 离线导出截尾 | 覆写 `getTailSamples()` = 12.5 s | 宿主知道有尾巴 |
| P2-4 | MIDI 事件整块对齐 | 按 `sampleOffset` 分段渲染后应用事件（96 事件栈缓冲 + 插入排序） | 音符起点/终点采样级精确 |
| P2-5 | 二次 initialize 返回失败 | 守卫移到基类调用之前 | 二次初始化返回 `kResultOk` |
| P2-6 | 停止时还原种子被丢弃 | 60 ms 无音频块则允许换入 | 停止状态加载工程也能拿到波表 |
| P2-7 | UTF-8 乱码 | `MultiByteToWideChar(CP_UTF8)` | 面板 "·" 正常 |
| P2-8/9 | 面板重叠/裁切 | 全部重排（见上表） | 截图确认 |
| P2-10 | 每帧新建 HBITMAP、30 fps 全量重绘 | 离屏面改成 GDI+ 共享的 DIB section，40 ms 定时，空闲降到 5 tick 一次 | 无每帧分配 |

---

## 三、没有改的（属于性能/设计取舍，不是 bug）

1. **建波表 ~90 ms**：8 表 × 8 帧 × 4 mip × 最多 256 谐波，实例化/加载/DICE 会卡一下。要降只能上 FFT 或减少帧数，会改变音色。
2. **8 voice 满负荷约 30–40% 单核**：BAND 是 96 partials × 8 voice 的加法合成，本身就是这个量级。
3. **HARSH 不做音高跟踪**：噪声源没有音高，硬加会变成另一台合成器；README 已改成"用 KEY → HARSH CUT 自己做"。键跟踪参考音仍是 C4（60），与波表的 A4（69）不同，这是历史行为，改了会让所有预置整体移调。
4. **ENV2 只作用于 BAND 的音量整形**：它仍然可以作为调制源，只是没有独立的 VCA。

---

## 四、安装 / 构建

```powershell
# 安装
copy dist\NoiseForge.vst3  "C:\Program Files\Common Files\VST3\" -Recurse

# 构建（需要 MSYS2 mingw64）
$env:PATH='C:\msys64\mingw64\bin;'+$env:PATH
powershell -ExecutionPolicy Bypass -File build.ps1
```

`build.ps1` 在非 ASCII 路径下会因 g++ 参数编码失败；本目录附带 `build_local.ps1`（相对路径 + 绝对 SDK 路径）可以在任意目录构建。
