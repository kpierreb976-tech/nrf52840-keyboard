#define MODULE encoder_button_control

#include "events/ble_control_event.h"

#include <app_event_manager.h>
#include <caf/events/button_event.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifndef CONFIG_ENCODER_BUTTON_CONTROL_LOG_LEVEL
#define ENCODER_BUTTON_CONTROL_LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#else
#define ENCODER_BUTTON_CONTROL_LOG_LEVEL CONFIG_ENCODER_BUTTON_CONTROL_LOG_LEVEL
#endif
LOG_MODULE_REGISTER(MODULE, ENCODER_BUTTON_CONTROL_LOG_LEVEL);

#define ENCODER_BUTTON_KEY_ID 0x0180
#define BLE_BOND_CLEAR_HOLD_TIME K_SECONDS(5)

static struct k_work_delayable ble_bond_clear_work;
static bool encoder_button_pressed;
static bool work_initialized;

static void submit_clear_bonds_event(void)
{
	struct ble_control_event *event = new_ble_control_event();

	if (event == NULL)
	{
		LOG_ERR("ble_control_event 内存分配失败");
		return;
	}

	event->cmd = BLE_CONTROL_CMD_CLEAR_BONDS;
	APP_EVENT_SUBMIT(event);
}

static void ble_bond_clear_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!encoder_button_pressed)
	{
		return;
	}

	LOG_WRN("旋钮长按触发 BLE 配对清理");
	submit_clear_bonds_event();
}

static void ensure_work_initialized(void)
{
	if (work_initialized)
	{
		return;
	}

	k_work_init_delayable(&ble_bond_clear_work,
						  ble_bond_clear_work_handler);
	work_initialized = true;
}

static bool handle_button_event(const struct app_event_header *aeh)
{
	const struct button_event *event = cast_button_event(aeh);

	if (event->key_id != ENCODER_BUTTON_KEY_ID)
	{
		return false;
	}

	ensure_work_initialized();
	encoder_button_pressed = event->pressed;

	if (event->pressed)
	{
		(void)k_work_reschedule(&ble_bond_clear_work,
								BLE_BOND_CLEAR_HOLD_TIME);
	}
	else
	{
		(void)k_work_cancel_delayable(&ble_bond_clear_work);
	}

	return false;
}

APP_EVENT_LISTENER(encoder_button_control, handle_button_event);
APP_EVENT_SUBSCRIBE(encoder_button_control, button_event);
