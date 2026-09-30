# v0.1.0 - macOS Apple-compatible bridge

首个公开版本。

## 包含内容

- 预编译 RP2350 Apple 兼容 UF2（`04B4:2412:0500`，Vader 2 Pro HID 模型）
- M1-M4、传感器、WebHID 电量、震动和配置路径
- 主机验证、描述符检查、macOS 探针和完整研究文档

## 固件校验

```text
3dec2a00fec4dbd10cdd038b77571ac9e433e6d09e42c64389bacb484da31953
```

## 使用边界

刷写前请保存整片 Flash，并阅读仓库中的 [`DISCLAIMER.md`](../DISCLAIMER.md)。该固件不是 Xbox Elite 2 或 GIP 固件，也不是 Flydigi、Apple、Microsoft 或 Waveshare 官方软件。原生 `GCController` 的 haptics/battery 仍未暴露；其他平台和浏览器需要单独验证。
