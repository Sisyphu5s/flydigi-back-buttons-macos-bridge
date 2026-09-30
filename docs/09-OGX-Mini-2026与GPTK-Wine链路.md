# 09 OGX-Mini-2026 与 Wine/GPTK 链路（本轮新增的两块拼图）

本文回答两个问题：**① 社区成熟固件到底能给我们什么（含同板、同类设备）？② 如果目标是"在 Mac 上玩游戏"，我们的设备还要穿过 GPTK/Wine 这一层，那一层看什么？**

---

# 第一部分：`MegaCadeDev/OGX-Mini-2026`（`wiredopposite/OGX-Mini` 的活跃 fork）

## 1. 为什么它是本轮最重要的参考

| 事实 | 对我们的意义 |
|---|---|
| **Waveshare RP2350-USB-A 是它的"维护者受支持板"之一**，且有专门的 **PIO USB host 修复**（v1.0.0.11a：Switch Pro / DualShock 3 / **Xbox 360 无线接收器** / Razer Atrox XBO，都在 RP2350-USB-A 上验证） | 我们用同一块板、同一套 PIO-USB host 栈，板级坑别人已经踩过并修了 |
| **飞智 Vader 5 Pro 明确"支持"**：`Wired_Controllers.md` 写 **有线 USB 与飞智 2.4 GHz 接收器都物理确认可用**，走现有 XInput / DInput host 路径，**无需专用驱动** | **直接验证了我们的输入侧**：接收器就是个标准 XInput 设备，Pico 侧读它没问题。macOS 认不到纯粹是**主机侧**的问题（苹果不认 360/XInput 类设备） |
| 固件结构：`USBHost/HostDriver/*`（输入）+ `USBDevice/DeviceDriver/*`（输出）+ `HardwareIDs.h`（VID/PID 表）+ 可交互 build 脚本（`./scripts/build.sh`） | 我们的固件应该照这个**分层**来，而不是单文件堆叠；`HardwareIDs.h` 可直接借来索引飞智的 ID |

## 2. 它**没有**的（= 我们的增量，第二次确认）

- **没有 GIP 输出模式**。输出侧只有：XInput(360+XSM3)、DInput、PS3、PS4 gadget、STEAM(DualSense)、Switch Pro、Wii U、Wii、Xbox OG、PS Classic。
- 路线图里 `~~Xbox One~~` 被划掉并注明"**需要认证 dongle 才能作为原生 Xbox One 手柄工作**" → 即：**GIP 设备端是社区公认的"要认证挡路"的区域**。
- 结论不变：**读飞智/桥骨架 = 抄；伪装 Elite = 我们自己做。**

## 3. 可直接抄的 GIP **主机侧**工程细节（比上一版更全）

| 细节 | 内容 |
|---|---|
| 标准 Xbox One 有线 pad | 走 **64 字节** IN 读取（Linux xpad 风格 GIP 路径） |
| 街机摇杆（Razer Atrox XBO `1532:0a00`） | 走 **XBOFS** 初始化：`05 20 00 01 00`(POWER_ON) → 等 OUT → **30 字节** IN 循环；输入布局 dpad 在 byte 5、脸键/LT/RT 在 byte 22 |
| `start_xboxone()` | = **POWER_ON + S_INIT**，挂载后**延迟 ~50ms** 发送；**ANNOUNCE 会触发重初始化**（若此前没发过 power） |
| **Guide 按钮** | 是 GIP **`0x07` VIRTUAL_KEY**，**不是** `0x20` 输入报告里能自动清除的位 → 设备端必须把 Guide 当**独立命令**发（按下发一次、松开发一次） |
| 报告驱动方式 | 真机**变化驱动**：负载不变就不发。OGX-Mini 曾因"N 毫秒没报告"误判断连而掉线 → **静默 ≠ 掉线** |
| 输入报告的"keep-alive"位 | 第三方 GIP pad 的 `buttons` 低字节 **bit1 常被用作 keep-alive 标志**（Wolverine 抓包："bit1 = keep-alive, ignore"）。→ 我们发报告时也要考虑周期性置位（**待验证**：苹果是否要求） |
| 360 的认证 | 他们用 **`libxsm3`**（Xbox Security Method 3，走 **USB 控制请求** `0x81/0x82/0x83/0x86/0x87`，不是 GIP 包）实现真机级认证，于是 360 上不再需要 UsbdSecPatch —— **代价是"不再输出到 PC"** ⚠️ **重要警示：为真机做认证会牺牲 PC/Mac 侧可用性。** 我们目标主机是 macOS，**不要**照抄 360 的认证路径 |
| 参考来源 | 它把 GIP 初始化归功于 **XBOFS.win**；网表 `Wired_Controllers.md` 是按驱动类型整理的 VID/PID 清单（含 Flydigi/GameSir/PDP/Mad Catz/Hori 的 XBO 街机杆 ID） |

## 4. 关于飞智的其他证据

- `Flydigi_APEX4_Wukong.md`：飞智 APEX 4 的**有线/蓝牙多模式矩阵**（含"Back Remap Buttons 在 Switch 模式不工作""Home 在 Android/iOS 模式不工作""**2.4 GHz dongle 未测**"）。→ 说明**飞智不同型号/不同连接方式的行为差异很大**，我们只对 Vader 5 Pro 负责，不要泛化。
- 参考仓库里 Vader 4 Pro 被当作**普通 DInput 设备**（`0xEED1:0x04B4` 加进 `DINPUT_IDS`）——即飞智在 DInput 模式下是标准 HID。**但 DInput 模式没有 M1–M4/陀螺**（回到 docs/02 的结论：拿背键必须读扩展接口）。

---

# 第二部分：GPTK / Wine 这一层看什么（决定"游戏里能不能用"）

## 1. 链路（关键结论：**Wine 侧靠 SDL，不靠 IOKit 直读**）

```
我们的板 → macOS（Apple dext 把 GIP 转成系统级 HID）→ SDL（Wine bundle 内）
        → winebus.sys(SDL bus) → winexinput.sys → xinput1_3.dll → 游戏
```

证据：社区项目 `Tinerou/wolverine-v3-pro-macos-wine`（Razer Wolverine V3 Pro，GIP，`1532:0a3f`）的说明与代码路径明确写出上述链路，并提供了 `src/xinput_test.c` 作为**在 Windows XInput 层**验证的手段（比"进游戏看能不能动"可靠得多）。

## 2. 这个项目顺带回答了三个我们关心的问题

1. **macOS 没有 GIP 驱动时，Wine 侧拿不到输入**：他们必须**重编 SDL**（静态 libusb HIDAPI 后端 + 在自己补丁里实现 GIP 安全握手），再把 dylib 塞进 Wine bundle。
   → **反过来说，我们的方案更省事**：只要 Apple dext 接受我们伪装的 Xbox 身份，macOS 就会发布一张 HID 给系统，**Wine 里的 SDL 直接就能看到**，不需要动 Wine/SDL。**这是"伪装成 Apple 原生手柄"相对"直连 GIP"的额外红利。**
2. **Apple 发布的幻影 HID 会占坑**：他们的 `wine-registry.sh` 要设 `DisableHidraw=1`，并在环境变量里 `SDL_JOYSTICK_MFI=0` + `SDL_GAMECONTROLLER_IGNORE_DEVICES=0x045e/0x028e,...`（因为 Razer Synapse 即使没插手柄也发布 **phantom "Xbox 360 Controller" HID**）。
   → **我们自己也要小心这件事**：设备拔掉/未上电时不要残留一个半死的 HID 身份；调试时要会识别"占坑的幻影设备"。
3. **拨片在 Xbox 模式下拿不到（他们的 Wolverine 也一样）**：Wolverine 的 M1–M6 在 Xbox 模式**不暴露**（只镜像面键，除非开厂商诊断模式）。
   → **对我们的期待管理**：即便 Elite 身份让 `paddleButton1..4` 在 macOS 的 GameController 层可用，**在 GPTK/Wine 游戏里 puck 极可能仍然不可见**（XInput 没有拨片概念）。**若要在 GPTK 游戏里用背键，现实办法是把 M1–M4 映射成键盘码（D5 复合设备退路）或复制到已有按键。**

## 3. GIP 安全握手：方向、两版算法、以及"能不能绕开"

来自 `xone/auth/auth.c`（Linux 内核驱动，GPL）与 `Tinerou/NOTES.md`：

```
v1（RSA 路径）: HOST_HELLO(0x01) → CLIENT_HELLO(0x02) → CLIENT_CERTIFICATE(0x03)
                → HOST_SECRET(0x05) → CLIENT_FINISH(0x08) → HOST_FINISH(0x07)
v2（ECDH 路径）: HOST_HELLO(0x21) → CLIENT_CERTIFICATE(0x23) → HOST_PUBKEY(0x25) → HOST_FINISH(0x26)
```

- **方向是"设备向主机出示微软签发的 X.509 设备证书 + 私钥"**（xone 从设备证书里抽公钥、算 pre-master-secret、用 PRF 收尾）。也就是说**设备侧需要真凭据**才能完成；这正是"原生 Xbox One 手柄输出需要认证 dongle"的原因。
- **但是（关键突破口）协议里有设备可声明的能力位**（SDL `SDL_hidapi_gip.c` 原文）：
  ```
  GIP_FEATURE_SECURITY_OPT_OUT = (1u << 4)        // 设备声明：可以跳过安全流程
  GIP_FEATURE_ELITE_BUTTONS    = (1u << 2)        // 设备声明：有 Elite 拨片
  GIP_FEATURE_DYNAMIC_LATENCY_INPUT (1<<3) / MOTOR_CONTROL (1<<5) / GUIDE_COLOR (1<<6) / EXTENDED_SET_DEVICE_STATE (1<<7)
  ```
  主机侧逻辑（SDL 原文）：
  ```c
  if (GIP_SupportsSystemMessage(attachment, GIP_CMD_SECURITY, false) &&
      !(attachment->features & GIP_FEATURE_SECURITY_OPT_OUT))
      GIP_SendSystemMessage(attachment, GIP_CMD_SECURITY, ...);
  ```
  → **设备声明 `SECURITY_OPT_OUT` 时，合规主机根本不发 0x06。**
- 另外解析 identify 时会校验**安全协议版本必须是 1.0**（`bytes[24]` / `bytes[25]`），且拨片解码整体**以 `features & GIP_FEATURE_ELITE_BUTTONS` 为门**。

### 对风险表的直接影响
**R2 从"我们要不要实现 RSA/ECDH 认证"降级为"苹果的 dext 遵不遵守 SECURITY_OPT_OUT"**。前者几乎不可行（需要微软签发凭据），后者是**一轮固件实验就能判**的问题，而且有两条退路：
- 若苹果遵守 → 直接跳过认证，M2 顺利通过；
- 若苹果不遵守（仍发 0x06 并要求成功）→ 退到 **360/DInput 身份 + 复合键盘**（D5 退路），或只服务 GPTK（那里可以走 Tinerou 的"补 SDL"路线，但他们也得有凭据…… 故这条更差）。

## 4. 本轮新增的验证手段（写进 M1/M2 验收）

| 层 | 工具 | 判据 |
|---|---|---|
| macOS 系统层 | `ioreg -p IOUSB -l` + `hidutil list` | 设备被 Xbox 驱动接管、HID 节点存在 |
| macOS GC 层 | docs/05 附录 A 的 Swift CLI | `GCXboxGamepad` + `paddleButton1..4` 有响应 |
| **Wine/GPTK 层** | Heroic/GPTK 瓶内跑 `xinput_test`（参考 Tinerou 的 `src/xinput_test.c` 思路）或 `wine control` → Game Controllers | 游戏能看到 XInput 槽位与实时值；这层不通就说明"原生 macOS 能用但游戏不能" |
| 归因纪律 | 每层独立验收 | 三层里任何一层失败，都能立刻定位，不要"进游戏试"当唯一判据 |
