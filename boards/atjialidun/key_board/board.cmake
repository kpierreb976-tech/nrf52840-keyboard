# SPDX-License-Identifier: Apache-2.0

set(OPENOCD_NRF5_SUBFAMILY "nrf52")

# 告诉 J-Link RTT 和 Debugger 具体的芯片型号以及通讯速度，防止弹窗
board_runner_args(jlink "--device=nRF52840_xxAA" "--speed=4000")

# 顺便告诉底层的烧录工具（nrfjprog）这是 nRF52 系列芯片
board_runner_args(nrfjprog "--family=NRF52")

# DAP-Link 等其他烧录器的备用配置
board_runner_args(pyocd "--target=nrf52840" "--frequency=4000000")

# 引入官方的底层执行脚本
include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd-nrf5.board.cmake)
