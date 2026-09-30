/* usb_descriptors.h — 设备侧（USB-C → Mac）对外接口 */
#pragma once
#include <stdint.h>

/* 缓存最近一帧输入报告，供 GET_REPORT 与调试读取 */
void vader2pro_set_last_report(const uint8_t *rep);

/* Cache the latest standard HID Sensor reports for control GET_REPORT. */
void vader_sensor_set_last_report(uint8_t report_id, const uint8_t *rep);
#ifdef APPLE_BATTERY_PROBE
void vader_battery_set_last_report(uint8_t level);
#endif
#ifdef APPLE_BATTERY_INTERFACE_PROBE
void vader_battery_interface_set_last_report(uint8_t level);
#endif

/* WebHID configuration Feature Report bridge, implemented in main.c. */
uint16_t bridge_config_get_report(uint8_t report_id, uint8_t *buffer, uint16_t reqlen);
void bridge_config_set_report(uint8_t report_id, uint8_t report_type,
                              const uint8_t *buffer, uint16_t bufsize);
