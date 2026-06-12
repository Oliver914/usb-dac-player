# Release Notes

## v0.7

### 简体中文

**USB DAC Player v0.7** —— 把系统正在播放的媒体音频独占输出到 USB 小尾巴 DAC。

**本版更新**
- 🆕 **多架构支持**：新增 `armeabi-v7a`(32 位手机)与 `x86_64`(ChromeOS/部分平板),连同原有 `arm64-v8a`,一个包通吃,覆盖面大幅扩大。
- 🧰 **一键分享日志**:导出设备型号、探测到的格式、完整运行日志,方便反馈 bug。
- 🎚️ **自适应音质**:自动探测 DAC 支持的位深/采样率,「自动」选最高音质,也可手动选择并**按设备记住**。
- 🔊 **稳定无断续**:异步反馈闭环 + 预缓冲,消除时钟漂移导致的爆音;停止时复位设备,系统正常播放自动恢复。
- 📐 UI 适配各机型刘海/挖孔(边到边)。

**安装**:下载下方 `app-release.apk`,在手机上允许「未知来源」后安装(侧载)。仅支持 USB OTG + USB 音频设备的安卓手机(Android 10+)。

**已知限制**:音源是系统 48k 混音(再上采样),非源文件逐比特;捕获其它 App 音频属系统能力,部分禁用采集的 App 无法捕获。详见 [README](README.md)。

---

### English

**USB DAC Player v0.7** — exclusively outputs the system's media audio to a USB DAC dongle.

**What's new**
- 🆕 **Multi-ABI support**: added `armeabi-v7a` (32-bit phones) and `x86_64` (ChromeOS / some tablets), alongside the existing `arm64-v8a` — one universal APK, much broader device coverage.
- 🧰 **One-tap log sharing** for easy bug reports (device model, probed formats, full log).
- 🎚️ **Adaptive quality**: auto-detects supported bit depth / sample rate, "Auto" picks the highest, with manual selection **remembered per device**.
- 🔊 **Glitch-free**: async-feedback loop + pre-buffering; the device is reset on stop so normal playback recovers.
- 📐 Edge-to-edge UI that adapts to display cutouts (notches).

**Install**: download `app-release.apk` below and sideload it (allow "unknown sources"). Requires an Android phone (10+) with USB OTG + a USB audio device.

**Known limitations**: source is the 48 kHz system mix (upsampled), not bit-perfect; only capturable apps can be captured. See the [README](README.en.md).
