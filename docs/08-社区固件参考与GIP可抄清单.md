# 08 社区固件参考：哪些能抄、哪些不能

结论先行：**读飞智的侧（输入）和桥的骨架（Host+Device 双栈）社区已经成熟；写 GIP 设备端的侧（输出）社区没有成品，但"主机期望什么"有两份可读的实现**（Apple 本机 dext + OGX-Mini 的 TinyUSB host 驱动）。

---

## 1. 参考工程矩阵

| 工程 | 平台 | 它能干什么 | 对本项目的作用 |
|---|---|---|---|
| **`ruomox/Flydigi5Pico`** | **RP2350-USB-A（与我们同一块板）** | PIO-USB host 读飞智接收器 → TinyUSB device 伪装 **Xbox 360**；双核 seqlock；1kHz；震动回程；`usb_force_reset_bus()` | **骨架照抄对象**：板级引脚、PIO 配置、总线复位、核间通信、震动方向。唯一缺拨片 |
| **`wiredopposite/OGX-Mini`**（RP2040/RP2350） | Pico/Pico 2/RP2040-Zero… | 输出：OG Xbox / PS3 / Switch / **XInput(360)** / PS Classic / DInput（**没有 GIP 输出**）；输入：**含 Xbox One/Series/Elite 的 GIP host 驱动** | **GIP 设备端脚本的事实参考**：它的 host 代码告诉我们"主机连上后会发什么、期望收到什么形状的报告" |
| `GP2040-CE` | RP2040 | 有 **Xbox One Input Mode**，但靠**外接认证 pass-through 设备** | 关键证据：**认证是"控制台侧"强制**的（文档明说 PC 上用 PS4 模式不会触发 8 分钟超时）→ 苹果这边大概率**不查 MS 认证**（仍待反汇编确认） |
| `THLN33/RP2350-USB-A`、`ugufru/waveshare-rp2350-usb-a` | RP2350-USB-A | USB-A 收（HID 键盘 / MIDI）→ USB-C 出（UART 协议 / 标准 MIDI 设备） | **同板同架构的最小可运行范例**（比我们简单，适合先跑通） |
| `dlundqvist/xone`、SDL `SDL_hidapi_gip.c` / `SDL_hidapi_flydigi.c` | Linux / 跨平台 | GIP 与飞智的完整协议实现 | 命令表、**Elite 拨片位表**（`GIP_BTN_FMT_XBE1/XBE2_RAW/XBE2_4/XBE2_5`）、`0x4d {7,0}` 才能拿 raw report |
| `Loc15/PicoGamepadConverter` | RP2040/RP2350 | 通用手柄转换 | 低优先级，作交叉参考 |

## 2. 从 OGX-Mini 抄到的 GIP 具体事实（RP2040/RP2350 + TinyUSB，可逐行对读）

来源：`USBHost/HostDriver/XInput/tuh_xinput/tuh_xinput_cmd.h` + `Descriptors/XboxOne.h` + `XboxOne.cpp`。

**命令与标志**（与我们 docs/03 §5 的表一致，可互相印证）
```
0x01 ACK   0x02 ANNOUNCE  0x04 IDENTIFY  0x05 POWER   0x06 AUTHENTICATE
0x07 VIRTUAL_KEY  0x09 RUMBLE  0x0a LED  0x0c FIRMWARE  0x20 INPUT
GIP_OPT_ACK = 0x10        GIP_OPT_INTERNAL = 0x20
GIP_PWR_ON = 0x00   SLEEP = 0x01   OFF = 0x04   RESET = 0x07     GIP_LED_ON = 0x01
马达位：MOTOR_R = bit0, MOTOR_L = bit1, MOTOR_RT = bit2, MOTOR_LT = bit3
```

**主机侧会发出去的包（逐字节）**
```
POWER_ON              = 05 20 00 01 00
S_INIT（含认证子命令） = 05 20 00 0F 06 00 00 00 00 00 00 55 53 00 00 00 00 00 00
S_LED_INIT            = 0a 20 00 03 00 01 14
EXTRA_INPUT_PACKET_INIT = 4d 10 00 02 07 00        ← 即 SDL 的 Elite raw-report 解锁包
PDP_AUTH / PDP_LED_ON = 第三方（PDP）变体
```
→ **我们的固件必须能接受并正确回应这几条**，尤其是 `05 ... 06` 与 `4d`。

**输入报告（GIP 0x20）线上布局：4 字节头 + 14 字节负载 = 18 字节**
```
[0] command=0x20  [1] flags  [2] seq  [3] length
[4..5]  buttons[2]      buttons0: b0 SYNC, b1 GUIDE, b2 START, b3 BACK, b4 A, b5 B, b6 X, b7 Y
                        buttons1: b0..b3 DPad U/D/L/R, b4 LB, b5 RB, b6 L3, b7 R3
[6..7]  trigger_l  (uint16 LE，真机范围 0–1023 → 其实现是 >>2 得到 8 位)
[8..9]  trigger_r  (uint16 LE)
[10..17] joystick_lx / ly / rx / ry（int16 LE）
```

**两条重要行为学结论（与 Apple 侧的观测吻合）**
1. **变化驱动**：OGX-Mini 的 `process_report` 在负载 14 字节与上一包相同时直接跳过处理 → 真机不是"固定周期刷"。
2. 拨片**不在**这个驱动里（OGX-Mini 只把 Elite 当普通输入设备）→ **Elite 拨片位表的开源唯一可靠来源仍是 SDL**（4 种格式）+ 本机插件反汇编。

## 3. "17 还是 18 字节"这个矛盾解决了（推论，待硬件确认）

- 线上真实 GIP 包 = 18 字节（上面 OGX-Mini 的布局，两个独立来源都指向 18）。
- Apple 的 HID 描述符里 `95 11` = **body 17 字节**，因为**报告 ID 独占一个字节**（Report ID = GIP 命令字节 0x20），body = 18 − 1 = 17 ✓。
- 反编译里看到的 `len <= 0x11 → 丢弃`，若这个 `len` 传的是**含 report ID 的长度**，则 18 > 17 → 通过 ✓；17 字节的包（少一个字节）会被丢。
- **结论：固件按 18 字节发，问题大概率消失。** 这解释了社区两处说法为何看似矛盾 —— 它们在数不同的东西。
- 这条仍标为"待验证"（docs/06 R1），但 M3 的实验矩阵可以从"17/18 各试一轮"收敛为"确认 18 字节 + 逐位核对拨片偏移"。

## 4. 仍需自己解决的部分（没有现成实现）

1. **GIP 设备端状态机**（announce → identify/metadata → power → 0x20 流 → ack 策略）—— 只有主机侧参考，需要镜像着写。
2. **Elite 拨片位与固件版本协商**（SDL 的 4 种格式 + 苹果解码器偏移）—— 必须靠反汇编 + 实验。
3. **认证（0x06）**：苹果是否强制？GP2040-CE 的文档暗示"控制台才强制"，但必须以本机 dext 反汇编为准。
4. **RP2350-USB-A 的 host 口硬件问题**（见 docs/07）—— 这是板子层面的、与协议无关的独立风险。
