// debug_console.h — interactive bring-up REPL over the console UART / USB-JTAG.
//
// Commands (type `help` on the device):
//   stats   RTCM byte/frame counters + CRC fails + staleness
//   rtcm    message-type histogram (are 1005/1077/1087/1097 + 1019/1020 present?)
//   dump    hexdump the last CRC-valid frame
//   usb     USB host / CDC attach + VID/PID state
//
// Runs alongside ESP_LOG output on the same console. Start it after the sink
// and USB source are up.

#pragma once

void debug_console_start(void);
