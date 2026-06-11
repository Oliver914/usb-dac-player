# USB DAC Player

把**系统正在播放的媒体音频**捕获下来，绕过 Android 自带的混音/重采样路径，通过 **libusb 等时传输（isochronous OUT）**直接独占输出给 USB 小尾巴 DAC。

> 作者：三水深 · 许可证：Apache-2.0 · 仅依赖一个第三方库 libusb（LGPL-2.1，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)）

```
其它 App 的媒体声音
   │  AudioPlaybackCapture (MediaProjection, float)
   ▼
AudioRecord ──► 16/24/32-bit 转换 ──► 环形缓冲
   ──► native libusb 等时 OUT（异步反馈闭环）──► USB DAC（小尾巴）
```

## 功能

- **独占输出**：libusb 直接接管 USB 音频接口，bit 数据直送 DAC。
- **自适应音质**：探测 DAC 支持的 bit 深度 / 采样率，「自动」选最高音质，也可手动选择，按设备（VID:PID）**记住选择**。
- **稳定**：异步反馈端点闭环 + 预缓冲，消除时钟漂移导致的断续。
- **停止即恢复**：停止时复位设备，系统正常播放自动恢复。
- **一键分享日志**：导出设备信息 + 探测结果 + 完整运行日志，方便反馈 bug。

## 模块结构

| 模块 | 说明 |
| --- | --- |
| `:usbaudio` | 核心库：`UsbAudioManager`（Kotlin）+ JNI 桥 + native 引擎 `usb_audio_output.c`（UAC1/UAC2 解析、采样率/音量控制、等时传输、异步反馈、格式探测）+ 预编译 libusb。 |
| `:app` | 播放器 App：`MainActivity`（设备/格式选择、授权、启停、分享日志）+ 前台服务 `AudioCaptureService`（捕获→USB 管线）。 |

## 构建

- **JDK 17**（AGP 8.2 / Gradle 8.5 要求；Android Studio 自带 JBR 最省事）。
- Android SDK：platform `android-35`、build-tools `35.x`。
- **签名**：仓库**不含** `release.keystore`（已 gitignore）。本地放一个同名 keystore，
  或改用你自己的签名配置（见 `app/build.gradle` 的 `signingConfigs`）。
- native 引擎默认以**预编译 .so** 形式打包（无需 CMake）；如需从 `cpp/usb_audio_output.c`
  重新编译，安装 CMake 后取消 `usbaudio/build.gradle` 里 `externalNativeBuild` 的注释即可。

```bash
./gradlew :app:assembleRelease    # 产物 app/build/outputs/apk/release/
./gradlew installDebug            # 装到已连接设备
```

> 仅支持 `arm64-v8a`。

## 遇到问题怎么办

App 内点 **「分享日志」** 把日志发给作者，或在 GitHub 提 Issue 时附上该日志（包含手机型号、
小尾巴型号、探测到的格式、卡在哪一步）。欢迎 Fork / 提 PR。

## 已知限制（如实说明）

- **不是位完美**：音源是 Android 的 48k 系统混音（再上采样到所选规格），并非源文件逐比特数据。
  把 DAC 跑在 32bit/高采样率主要是利用 DAC 更好的工作模式，而非凭空提升信息量。真·无损/原生 DSD
  需要「本地文件解码直推」的播放器路径（暂未实现）。
- **可捕获性**：仅能捕获允许被采集的 App（`USAGE_MEDIA/GAME/UNKNOWN`，未禁用采集的）。
- **输出功率是硬件的事**：软件已满刻度输出；更大推力靠媒体音量拉满 + 小尾巴高增益模式。
- **上架商店有难度**：捕获其它 App 音频属政策敏感行为（尤其 Google Play，且涉版权）；
  且预编译 libusb 暂未做 16KB 页对齐。**侧载分发不受影响。**

## 许可证

本项目代码（作者：三水深）以 **Apache-2.0** 许可，见 [LICENSE](LICENSE)。
第三方组件见 [NOTICE](NOTICE) 与 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
