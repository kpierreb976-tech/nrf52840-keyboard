# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(KEYBOARD)

zephyr_include_directories(inc)

target_sources(app PRIVATE src/main.c src/battery.c src/mode_switch.c src/power_mgmt.c src/events/battery_event.c src/events/mode_event.c src/events/encoder_event.c src/events/encoder_mapper.c src/encoder_button_control.c src/encoder.c src/keyboard_core.c src/keymap.c src/usb_transport.c src/ble_transport.c src/events/hid_kbd_report_event.c src/events/hid_consumer_report_event.c src/events/set_protocol_event.c src/events/ble_state_event.c src/events/ble_control_event.c)
