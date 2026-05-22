config NRF_KEYBOARD_KCONFIG_TEST
	bool "NRF Keyboard Kconfig probe"
	default y
	help
	  探针：验证应用 Kconfig.app 是否被加载。
	  构建后检查 CONFIG_NRF_KEYBOARD_KCONFIG_TEST=y。

menu "应用模块日志等级"

module = BLE_TRANSPORT
module-str = ble_transport
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = USB_TRANSPORT
module-str = usb_transport
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = KEYBOARD_CORE
module-str = keyboard_core
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = MODE_SWITCH
module-str = mode_switch
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = POWER_MGMT
module-str = power_mgmt
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = ENCODER
module-str = encoder
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = ENCODER_MAPPER
module-str = encoder_mapper
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = ENCODER_BUTTON_CONTROL
module-str = encoder_button_control
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = BATTERY
module-str = battery
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

module = MAIN
module-str = main
source "$(ZEPHYR_BASE)/subsys/logging/Kconfig.template.log_config"

endmenu
