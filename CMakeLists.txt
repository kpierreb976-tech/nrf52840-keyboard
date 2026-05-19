# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(KEYBOARD)

zephyr_include_directories(inc)

target_sources(app PRIVATE src/main.c src/battery.c src/mode_switch.c src/power_mgmt.c src/events/battery_event.c src/events/mode_event.c src/events/encoder_event.c src/events/encoder_mapper.c src/encoder.c)
