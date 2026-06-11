# 第三方组件声明 / Third-Party Notices

本项目（USB DAC Player，作者：三水深，Apache-2.0）自身代码为原创。
运行时依赖以下第三方开源组件，版权归各自作者所有：

## libusb（核心 USB 访问，必需）

- 项目主页：https://libusb.info/
- 源码仓库：https://github.com/libusb/libusb
- 许可证：**GNU LGPL-2.1-or-later**
- 使用方式：本仓库分发了**未经修改**的预编译动态库
  `usbaudio/src/main/libusbLib/arm64-v8a/libusb1.0.so` 与头文件
  `usbaudio/src/main/cpp/libusb/include/libusb.h`。
- LGPL 合规：libusb 以动态库形式链接，使用者可用自行编译的 libusb 替换该
  `.so` 文件。LGPL-2.1 全文见 https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
  （如需随仓库分发，请将官方 `COPYING` 一并放入本目录）。

> 说明：`usbaudio/src/main/libusbLib/arm64-v8a/libusb_audio_output.so` 是本项目
> 自己的 native 引擎（由 `usbaudio/src/main/cpp/usb_audio_output.c` 编译），
> **不属于** libusb，版权归三水深。

## 标准构建/运行时依赖（均为 Apache-2.0）

以下为常规 Android 依赖，随构建自动获取，非本仓库分发：

- AndroidX（androidx.core、appcompat、lifecycle）— Google，Apache-2.0
- Material Components for Android（com.google.android.material）— Google，Apache-2.0
- Kotlin / kotlinx-coroutines — JetBrains，Apache-2.0
- Gradle Wrapper — Gradle，Apache-2.0
