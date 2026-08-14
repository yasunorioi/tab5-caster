// board_power.h — M5Stack Tab5 (ESP32-P4) board power rails.
//
// The USB-A host port's 5V (VBUS) is NOT on by default: it is gated by the
// PI4IOE5V6408-style I/O expander #2 (I2C 0x44), port P3 = USB5V_EN, active
// high. Until that bit is driven, a device on USB-A is only trickle-powered
// and never enumerates on the P4 HS OTG. board_power_init() brings up the
// expander (matching the M5 BSP init sequence) and enables USB5V_EN.
//
// I2C: port 0, SDA=GPIO31, SCL=GPIO32, 400kHz (Tab5 system bus).

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "driver/i2c_master.h"

// Init the Tab5 system I2C bus + PI4IOE #2 and enable USB-A 5V (USB5V_EN).
// Call before usb_cdc_source_start() so VBUS is up when the host installs.
esp_err_t board_power_init(void);

// Toggle USB-A VBUS (read-modify-write of the expander OUT_SET bit). Safe to
// call after board_power_init(). Handy from the console for power-cycling a
// wedged receiver.
esp_err_t board_usb_5v_en(bool en);

// The Tab5 system I2C bus (port 0, SDA=31/SCL=32), created by board_power_init().
// Shared with the display's LCD-reset I/O expander (PI4IOE #1 @ 0x43). Returns
// NULL before board_power_init(). Don't create a second master on this port.
i2c_master_bus_handle_t board_i2c_bus(void);
