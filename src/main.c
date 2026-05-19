#include <app_event_manager.h>

#define MODULE main
#include <caf/events/module_state_event.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(MODULE);

#include "mode_switch.h"
#include "power_mgmt.h"

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

    if (mode_switch_init() != 0)
    {
        LOG_ERR("Mode switch detection init failed");
    }

    return 0;
}
