# 写固件前的快速验证报告（纯主机，零硬件、零系统改动）

日期：2026-09-30
命令：`cd ~/Documents/flydigi-elite-bridge/firmware && ./verify.sh`
**结果：8/8 步通过 ✔**（退出码 0；含 RP2350 交叉编译与 USB 描述符检查）

---

## 1. 结论速览

| # | 验证项 | 结果 |
|---|---|---|
| 1 | 生成「冒充 Flydigi Vader2Pro.MobileUSB」的 HID 报告描述符（97 字节） | ✔ 输入 10 字节 + vendor Output 8 字节 |
| 2 | 用**自研独立解析器**把描述符当陌生字节流解析（不用生成器的任何假设） | ✔ 输入 6 轴 8bit + hat 4bit + 20 按钮 1bit → **10 字节**；Output → **8 字节** |
| 3 | 与苹果模型文件 `Flydigi/Vader2Pro/MobileUSBWithBackButtons.plist` 的**22 条谓词**逐条比对 | ✔ 22/22 命中；**M1–M4 = Button usage 15/16/17/18（bit 70..73）** |
| 4 | Python 端到端 dry-run：报文 → 解析 → 套模型 → GC 元素（含 LocalizedNameKey） | ✔ 4 个背键元素齐全，M1/M3 按下→`BUTTON_M1/BUTTON_M3=按下` |
| 5 | **C 侧编译 + 自测**（clang -Wall -Wextra，位偏移断言 + 往返 + 全按下） | ✔ 全部通过 |
| 6 | **震动 Output 包解析自测** | ✔ 三种输入格式、零强度停止、坏包拒绝 |
| 7 | **C 与 Python 交叉比对**（描述符 + 场景报文逐字节） | ✔ `40 a0 80 80 ff 00 00 11 48 01` 两侧一致 |
| 8 | RP2350 固件编译与 USB 描述符离线检查 | ✔ 配置 107B；HID OUT `0x01` + IN `0x81` |

⇒ 固件里"输出侧"的部分（USB ID、描述符、报文打包）**已经有可编译、可回归的实现**，剩下的风险只在
"苹果侧是否真按这套模型接纳我们"与"输入侧如何取到 M1–M4"。

---

## 2. 顺手定案的一个语义问题（对固件有强制含义）

之前 `UsageTypeIndex` 有两种猜测（usage−基址 / 类内位置）。普查全部 **202 个 personality 文件**后定案：

- **`UsageType` 只取 1/2/3**，是苹果自己的**元素类别**：`1=Button`、`2=Analog(轴/扳机)`、`3=Hat/DPad`（不是 HID usage page）。
- **`UsageTypeIndex` = 该类元素在描述符中的 0-based 位置**（位置敏感）。决定性反证：
  - `v121/p6214/r256` 上出现 **`2/12 lefttrigger`、`2/16 righttrigger`、`2/20 rightx`、`2/12 righty`** ——
    若按"usage−0x30"解释，等价 usage 是 0x3C/0x40/0x44，**不是任何轴**；只有"类内位置"能解释（该设备有 20+ 个模拟元素）。
  - `RotorRiot` 上出现 **`3/1 dpad.y`** —— 同一设备有**两个** hat 类元素，而 Hat switch usage(0x39) 只有一个 ⇒ 只能是位置。

**强制含义（写固件必须遵守）**：
1. 按钮类元素的**顺序即语义**：我们要给 M1..M4 的就是第 15..18 个按钮位。
2. **不要插入任何"额外的按钮/轴/hat"**，否则后面的位置全部错位、模型谓词会指到别的控件。
   （模型自己引用的 Button 位置有缺口 3/6/9/10/19，那是真手柄的杂项键；我们声明 20 个连续按钮即可，
   模型不引用这几位，留着不冲突。）
3. 位置 19（Button 20）必须是 Home —— 模型要 `1/19` 存在，否则整张模型可能对不上。

---

## 3. 目标报文位图（固件直接照抄）

USB 身份：**idVendor `0x04B4` (1204) / idProduct `0x2412` (9234) / bcdDevice `0x0500` (1280)**
（即 Custom bundle 里 `com.Flydigi.Vader2Pro.MobileUSB` 的 `IOPropertyMatch`）

| 字节 | 位 | 内容 | 对应的 GC 元素（`LocalizedNameKey`） |
|---|---|---|---|
| 0 | 7..0 | 左摇杆 X | `LEFT_THUMBSTICK` |
| 1 | 7..0 | 左摇杆 Y | `LEFT_THUMBSTICK` |
| 2 | 7..0 | 右摇杆 X | `RIGHT_THUMBSTICK` |
| 3 | 7..0 | 右摇杆 Y | `RIGHT_THUMBSTICK` |
| 4 | 7..0 | 右扳机 RT | `RIGHT_TRIGGER`（Analog） |
| 5 | 7..0 | 左扳机 LT | `LEFT_TRIGGER`（Analog） |
| 6 | 3..0 | hat（0=上…7=左上，8=中位） | `DIRECTION_PAD` |
| 6 | 7..4 | 填充（Const） | — |
| 7 | 0..7 | 按钮 1..8 | 1=A(56)、2=B(57)、4=X(59)、5=Y(60)、7=LB(62)、8=RB(63) |
| 8 | 0..7 | 按钮 9..16 | 11=Select/BUTTON_OPTIONS(66)、12=Start/BUTTON_MENU(67)、13=L3(68)、14=R3(69)、**15=M1(70)、16=M2(71)** |
| 9 | 0..3 | 按钮 17..20 | **17=M3(72)、18=M4(73)**、20=Home/BUTTON_HOME(75) |
| 9 | 7..4 | 填充（Const） | — |

（括号内为绝对 bit 偏移，与 `src/vader2pro_hid.h` 里的 `V2P_BTN_*` 常量一一对应；已由 C 自测断言。）

---

## 4. 交付物（工作区 `firmware/`）

| 文件 | 作用 |
|---|---|
| `verify.sh` | **一键跑完整验证**（描述符/模型/位级往返/震动包/C↔Python/输入侧/RP2350 构建） |
| `hid_desc_vader2pro.py` | 描述符生成器（同时生成 `desc_vader2pro.hex` / `desc_vader2pro.h`） |
| `hid_parse.py` | 独立 HID 描述符解析器 + 位级 pack/unpack（自研，含 UsageType 类别判定） |
| `model_conform.py` | 描述符 ⇄ 苹果模型一致性检查（两种 index 解释并排） |
| `dryrun_report.py` | 端到端 dry-run：报文→解析→模型→GC 元素值 |
| `src/vader2pro_hid.h` | **固件用**：按钮位常量、状态结构、API |
| `src/report_pack.c` | **固件用**：状态 ⇄ 10 字节报文 |
| `test/report_pack_test.c` | 主机自测（位偏移断言、往返、全按下边界） |
| `desc_vader2pro.h` | 97B 描述符 C 数组（输入 10B / Output 8B，可直接被 TinyUSB 使用） |
| `src/rumble_report.c` / `test/rumble_report_test.c` | 震动 Output 解码与主机侧回归测试 |

---

## 5. 尚未覆盖（必须靠硬件/真机，主机验证不了）

1. **苹果是否真的套用这张模型**：`IOPropertyMatch` 命中只是"表里有你"，实际由
   `GenericGamepadHIDServicePlugin` 在设备插入时决定 —— 需要在真板子上 `ioreg` 看设备属性是否被采纳，
   并用一段 20 行 Swift 遍历 `GCController.physicalInputProfile.elements` 打印 identifier。
2. **同一 VID/PID 下 Personality 的取舍**（`WithBackButtons` 排在前，但实际选谁未从代码确证）——
   我们的布局声明了模型要求的全部位置，理论上两个模型都能对上；若系统选了不带背键那份，
   就需要看选择逻辑（已交给后台 agent B 反汇编）。
3. **可见性**：`BUTTON_M1..M4` 出现在 `physicalInputProfile.elements` 里之后，具体游戏是否枚举它们
   （Xbox 专属的 `GCXboxGamepad.paddleButton1..4` 属另一类别，HID 类模型不产出）。
4. **链路时序/轮询率/掉线**：RP2350 device 侧报告节奏、与 host 侧 PIO-USB 读飞智的并发。
5. **输入侧**：飞智 Vader 5 Pro 的 M1–M4 报文位置（docs/02），与本文档的输出侧无关但同为硬需求。
6. **浏览器 Gamepad API 震动**：Chromium 当前对通用 HID haptics 按 VID/PID 白名单创建 actuator；Output 报告和 OUT 端点已实现，但不能保证 `navigator.getGamepads()[i].vibrationActuator` 出现。需要用 WebHID 或浏览器支持的设备身份做端到端验证。

## 6. 固件工具链现状（写之前要补）

已有：`cmake`、`ninja`、`clang`（主机自测用）、`python3`。
缺：**arm-none-eabi-gcc**、**pico-sdk**、**picotool**（烧写/校验）、`openocd`（可选）。

补齐命令（需你确认后执行；共约 1–2 GB 下载）：
```sh
brew install --cask gcc-arm-embedded          # 或 brew install arm-none-eabi-gcc（formula 版）
git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git ~/pico/pico-sdk
git -C ~/pico/pico-sdk submodule update --init --depth 1   # 拉 TinyUSB 等
brew install picotool
export PICO_SDK_PATH=~/pico/pico-sdk
```

## 7. 复现与回归

```sh
cd ~/Documents/flydigi-elite-bridge/firmware && ./verify.sh    # 应输出「全部验证通过 ✔」
```
系统升级或苹果更新 `com_apple_MobileAsset_GameController_DB1` 资产后**重跑本脚本**：
描述符/模型一旦漂移，第 3、4 步会立刻失败（这就是回归基线）。
