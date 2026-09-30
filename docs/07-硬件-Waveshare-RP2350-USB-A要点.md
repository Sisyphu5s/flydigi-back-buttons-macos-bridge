# 07 硬件：Waveshare RP2350-USB-A 的坑与用法

适用前提：**目标平台已收窄为 macOS-only（决策 D1 修订）**，板子按 **Waveshare RP2350(-USB-A)** 记。

---

## 1. 板子事实

| 项 | 值 | 来源 |
|---|---|---|
| MCU | RP2350（双核 Cortex-M33 + 双核 Hazard3 RISC-V，150MHz，520KB SRAM，2MB flash） | Waveshare wiki |
| 两个 USB 口 | **USB-C = 原生 USB 控制器**（供电/烧录/device 侧）；**USB-A 母口 = 走 PIO 的软件 USB**（host 侧） | Waveshare wiki / 产品页 |
| PIO 引脚 | **DP = GP12, DM = GP13**，`PIO_USB_PINOUT_DPDM` | Flydigi5Pico 源码；Pico SDK 默认 `PIO_USB_DP_PIN_DEFAULT=12` |
| Pico SDK 板名 | `PICO_BOARD=waveshare_rp2350_usb_a` | qsantos 最小例 |
| 官方同构 demo | **`Host_hid_to_device_cdc`**（USB-A 做 host 收 HID → USB-C 做 device 出 CDC） | Waveshare wiki 的 C/C++ demo 列表 |
| 板载 LDO | RT9013，**500mA** | 产品页 |

**`Host_hid_to_device_cdc` 与我们的架构完全同构** → 这是第一份应该编译跑通的"Hello World"（M1 之前的热身）。

## 2. 两个必须知道的硬件坑（都会直接打到我们项目上）

### 坑 1：USB-A 口的 D+ 有一个 1.5kΩ 上拉（R13）——板子的 USB-A 电路是按 **device** 设计的

现象：接键盘这类**低速**设备根本枚举不了；热插拔无效；官方 wiki 里的 host 示例"跑不起来"。
权威结论（qsantos，2025-11-21）：**必须拆掉 R13**（靠近 pin 6 的那颗 0603/0402）。
与 RP2350 的 E9 errata 无关，是这块板独有的设计问题。

先例固件的**软件绕过**（`Flydigi5Pico.cpp` 的 `usb_force_reset_bus()`，注释直说 "Overriding R13"）：
把 GP12/GP13 设为输出、**驱动能力拉到 12mA**、**强拉低 200ms** 制造 SE0，然后释放交给 PIO。
→ 说明：**接收器能挂上，很大程度上靠这一段**。要照抄，并且在换板/改硬件后重新验证。

### 坑 2：缺 host 侧 15kΩ 下拉 → 拆了 R13 也**检测不到断连**

现象（qsantos 第二篇，2026-01-01）：拆掉 R13 后键盘能认、能收键，但拔掉键盘板子感知不到；再插常常失效，必须复位板子。
原因：host 侧 D+/D- 应有弱下拉，本板没有 → 拔掉后线电压不回 GND。
修法：D+ 与 D- 各加一颗 **15kΩ 到 GND**（他做成转接头验证；正式做法是贴两颗 0402）。

**对我们的实际影响**：接收器是**常插**用法 → 断连检测不关键；但"开机挂载 + 软复位"必须可靠。
→ 结论：**先软绕过（照抄 `usb_force_reset_bus`）走通全流程；只有在遇到"挂载间歇失败"时才做硬件改**（拆 R13 + 补两颗 15k）。改硬件属于不可逆操作，需单独确认。

## 3. 其他待确认项（同一块板不同来源有分歧）

| 项 | 分歧 | 怎么定 |
|---|---|---|
| Host / Device 的 **rhport 编号** | Flydigi5Pico：Host=PIO 用 **rhport 1**、Device 用 **rhport 0**；qsantos 最小例：`BOARD_TUH_RHPORT 0` + `CFG_TUH_RPI_PIO_USB` | 编译期以实际 Pico SDK 2.2.0 的 board 定义为准（`boards/waveshare_rp2350_usb_a.h` 里看 `BOARD_TUH_RHPORT`），不要照抄 |
| 板子具体型号 | 用户确认"waveshare rp2350"，但 "1c1a" 对应不到公开型号 | 需要板上丝印的照片/购买链接；若**不是** RP2350-USB-A（而是 RP2350-Zero / RP2350-USB-C 等），第 2 节的坑与引脚要重新核对 |

## 4. 同类替代板（万一 RP2350-USB-A 的 host 口修不好）

qsantos 的横向比较（2026-01-07）里，**两端口真 USB + 便宜 + 免焊**的候选：
- **Adafruit Feather RP2040 with USB Type A Host**（原生两口，作者点名推荐）
- Waveshare **RP2040-PiZero**（但他发现其 USB 电路同样有问题，需读原文）
- **nanoCH32V203 / nanoCH32V305**（约 3 欧，原生 host/device，非 PIO hack；代价是生态小众）
- 不推荐：ESP32-S3-DevKitC（第二个口是 UART 桥，不是真 USB）

## 5. 供电预算

USB-A 口的 VBUS 由 Mac 经 USB-C 口供出；板载 LDO 只有 **500mA**。板子自身约 30–60mA，飞智接收器峰值需实测（2.4G 发射瞬间是尖峰）。
→ 目标：实测接收器 + 板子的总电流峰值，确认 < 500mA 且不触发 Mac 端口限流；若超，先考虑带供电的 USB-A 延长线/hub（对 host 口注入 VBUS 是有风险的接法，必须单独评估，不要盲接）。
