/* tusb_config.h — RP2350 双角色
 *
 *   port 0 = 原生 USB（USB-C）→ Device：冒充 Flydigi Vader2Pro.MobileUSB
 *   port 1 = PIO-USB（USB-A） → Host  ：读飞智 2.4G 接收器
 *
 * 依据：refs/Flydigi5Pico（同板、同栈，已跑通 host 侧）+ docs/02 §1
 * 注意：本工程只用自定义类驱动读接收器，故 CFG_TUH_HID = 0。
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU            OPT_MCU_RP2040     /* RP2350 复用 rp2040 端口层 */
#define CFG_TUSB_OS             OPT_OS_PICO
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          2                  /* 日志走 CFG_TUSB_DEBUG_PRINTF → RAM 环 */
#endif
/* TinyUSB 的调试输出重定向：写进 RAM 环形缓冲，由 core0 读出来打印
 * （不能直接写 CDC：设备栈归 core0，跨核写会破坏状态机） */
int  brg_tu_log_printf(const char *fmt, ...);
#define CFG_TUSB_DEBUG_PRINTF   brg_tu_log_printf

/* ---------------- Device（USB-C → Mac） ---------------- */
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

#if defined(APPLE_BATTERY_INTERFACE_PROBE)
#define CFG_TUD_HID             4                  /* + isolated Battery System */
#else
#define CFG_TUD_HID             3                  /* gamepad + sensor + WebHID config */
#endif
#define CFG_TUD_HID_EP_BUFSIZE  64                 /* WebHID Feature/Status reports use 63 B */

#define CFG_TUD_CDC             1                  /* 控制台（可观测性） */
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  512
#define CFG_TUD_CDC_EP_BUFSIZE  64

#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

/* ---------------- Host（USB-A 走 PIO-USB → 接收器） ---------------- */
#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1                  /* 用 PIO 模拟 USB 主机 */
#define CFG_TUH_RHPORT1_MODE    OPT_MODE_HOST
#define CFG_TUH_DEVICE_MAX      3
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_API_EDPT_XFER   1                  /* usbh_edpt_xfer_with_callback */

#define CFG_TUH_HUB             0
#define CFG_TUH_HID             0                  /* 自定义类驱动接管接口 */
#define CFG_TUH_CDC             0
#define CFG_TUH_MSC             0
#define CFG_TUH_VENDOR          0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
