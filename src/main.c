#include <app_event_manager.h>

#define MODULE main
#include <caf/events/module_state_event.h>

#include <zephyr/logging/log.h>
#ifndef CONFIG_MAIN_LOG_LEVEL
#define MAIN_LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#else
#define MAIN_LOG_LEVEL CONFIG_MAIN_LOG_LEVEL
#endif
LOG_MODULE_REGISTER(MODULE, MAIN_LOG_LEVEL);

#include "encoder.h"
#include "ble_transport.h"
#include "keyboard_core.h"
#include "lcd_display.h"
#include "mode_switch.h"
#include "power_mgmt.h"
#include "usb_transport.h"

int main(void)
{
    if (app_event_manager_init())
    {
        LOG_ERR("Application Event Manager not initialized");
    }
    else
    {
        module_set_state(MODULE_STATE_READY);
    }

    if (power_mgmt_init() != 0)
    {
        LOG_ERR("Power management init failed");
    }

    if (encoder_init() != 0)
    {
        LOG_ERR("Encoder init failed");
    }

    if (mode_switch_init() != 0)
    {
        LOG_ERR("Mode switch detection init failed");
    }

    if (keyboard_core_init() != 0)
    {
        LOG_ERR("Keyboard core init failed");
    }

    if (lcd_display_init() != 0)
    {
        LOG_ERR("LCD display init failed");
    }

    if (usb_transport_init() != 0)
    {
        LOG_ERR("USB transport init failed");
    }

    if (ble_transport_init() != 0)
    {
        LOG_ERR("BLE transport init failed");
    }

    return 0;
}
