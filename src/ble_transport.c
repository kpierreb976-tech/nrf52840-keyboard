#define MODULE ble_transport

#include "ble_transport.h"
#include "mode_switch.h"
#include "events/ble_control_event.h"
#include "events/ble_state_event.h"
#include "events/hid_consumer_report_event.h"
#include "events/hid_kbd_report_event.h"
#include "events/mode_event.h"
#include "events/set_protocol_event.h"

#include <app_event_manager.h>
#include <caf/events/keep_alive_event.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#ifndef CONFIG_BLE_TRANSPORT_LOG_LEVEL
#define BLE_TRANSPORT_LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#else
#define BLE_TRANSPORT_LOG_LEVEL CONFIG_BLE_TRANSPORT_LOG_LEVEL
#endif

LOG_MODULE_REGISTER(MODULE, BLE_TRANSPORT_LOG_LEVEL);

#define BLE_KBD_REPORT_ID 1
#define BLE_CONSUMER_REPORT_ID 2
#define BLE_NKRO_REPORT_ID 3

#define BLE_BOOT_REPORT_SIZE 8
#define BLE_6KRO_REPORT_SIZE 8
#define BLE_CONSUMER_REPORT_SIZE 6
#define BLE_NKRO_REPORT_SIZE 32

#define BLE_HIDS_REMOTE_WAKE 0x01
#define BLE_HIDS_NORMALLY_CONNECTABLE 0x02
#define BLE_REPORT_TYPE_INPUT 0x01

#define BLE_REPORT_TYPE_KBD 0
#define BLE_REPORT_TYPE_CONSUMER 1
#define BLE_TX_RING_BUF_SIZE 512
#define BLE_NOTIFY_IN_FLIGHT_LIMIT 12
#define BLE_TX_WORK_Q_STACK_SIZE 1536
#define BLE_TX_WORK_Q_PRIO K_PRIO_PREEMPT(8)

#define BLE_ADV_RESTART_FAST_MS 500
#define BLE_ADV_RESTART_REPAIR_MS 2000
#define BLE_ADV_RESTART_STALE_HOST_MS 10000
#define BLE_ADV_RESTART_STALE_HOST_LONG_MS 30000
#define BLE_STALE_HOST_LONG_BACKOFF_THRESHOLD 3
#define BLE_KEEP_ALIVE_INTERVAL_MS 30000
#define BLE_SECURITY_REPAIR_THRESHOLD 2
#define BLE_PAIRING_MODE_TIMEOUT_MS 60000
#define BLE_BOND_CLEANUP_DELAY_MS 200
#define BLE_SECURITY_REQ_DELAY_MS 300
#define BLE_HIDS_NOTIFY_CCCD_MAX 4

typedef enum
{
	BLE_STATE_DISCONNECTED = 0,
	BLE_STATE_CONNECTED = 1,
	BLE_STATE_ENCRYPTED = 2,
	BLE_STATE_READY = 3,
} ble_state_t;

enum hid_drop_reason
{
	HID_DROP_NONE = 0,
	HID_DROP_NOT_BLE,
	HID_DROP_NO_CONN,
	HID_DROP_NO_SEC,
	HID_DROP_NO_CCCD,
};

#define HIDS_ATTR_BOOT_KBD_IN 10
#define HIDS_ATTR_REPORT_KBD 15
#define HIDS_ATTR_REPORT_NKRO 19
#define HIDS_ATTR_REPORT_CONSUMER 23

struct ble_pending_report
{
	uint8_t type;
	uint8_t format;
	uint8_t len;
	uint8_t data[BLE_NKRO_REPORT_SIZE];
};

static struct bt_conn *active_conn;
static struct k_work_q ble_tx_work_q;
K_THREAD_STACK_DEFINE(ble_tx_work_q_stack, BLE_TX_WORK_Q_STACK_SIZE);
static struct k_work ble_tx_work;
static struct k_work_delayable ble_tx_retry_work;
static struct k_work_delayable adv_restart_work;
static struct k_work_delayable ble_keep_alive_work;
static struct k_work_delayable pairing_timeout_work;
static struct k_work_delayable bond_cleanup_work;
static struct k_work_delayable user_clear_finalize_work;
static struct k_work_delayable security_request_work;
RING_BUF_DECLARE(ble_tx_ring, BLE_TX_RING_BUF_SIZE);

static atomic_t s_ble_state = ATOMIC_INIT(BLE_STATE_DISCONNECTED);
static bool ble_ready;
static bool ble_mode_active;
static bool advertising;
static bool connected;
static bool bonded;
static bool link_encrypted;
static bool bond_repair_required;
static bool pending_bond_cleanup;
static bool stale_host_key_backoff;
static bool pairing_mode_active;
static uint8_t hids_notify_cccd_count;
static uint8_t stale_host_key_failures;
static uint8_t security_repair_failures;
static atomic_t ble_notify_in_flight;
static uint8_t active_identity = BT_ID_DEFAULT;
static uint32_t next_adv_restart_delay_ms = BLE_ADV_RESTART_FAST_MS;
static const char *pending_bond_cleanup_reason;
static uint8_t protocol_mode = 1;
static bool ble_retry_pending;
static struct ble_pending_report ble_retry_report;
static enum hid_drop_reason last_drop_reason = HID_DROP_NONE;
static bool user_clear_active;
static bool ble_teardown_active;
static uint8_t last_published_state = 0xFF;
static bool last_published_connected;
static bool last_published_bonded;
static int last_published_err;

static const uint8_t hids_info[] = {
	0x11,
	0x01,
	0x00,
	BLE_HIDS_REMOTE_WAKE | BLE_HIDS_NORMALLY_CONNECTABLE,
};

static uint8_t boot_kbd_output;
static uint8_t report_kbd_output;

static const uint8_t report_ref_kbd[] = {
	BLE_KBD_REPORT_ID,
	BLE_REPORT_TYPE_INPUT,
};

static const uint8_t report_ref_kbd_output[] = {
	BLE_KBD_REPORT_ID,
	0x02, /* HID Report Type: Output */
};

static const uint8_t report_ref_consumer[] = {
	BLE_CONSUMER_REPORT_ID,
	BLE_REPORT_TYPE_INPUT,
};

static const uint8_t report_ref_nkro[] = {
	BLE_NKRO_REPORT_ID,
	BLE_REPORT_TYPE_INPUT,
};

static const uint8_t hid_report_map[] = {
	0x05,
	0x01,
	0x09,
	0x06,
	0xA1,
	0x01,
	0x85,
	BLE_KBD_REPORT_ID,
	0x05,
	0x07,
	0x19,
	0xE0,
	0x29,
	0xE7,
	0x15,
	0x00,
	0x25,
	0x01,
	0x75,
	0x01,
	0x95,
	0x08,
	0x81,
	0x02,
	0x95,
	0x01,
	0x75,
	0x08,
	0x81,
	0x03,
	0x95,
	0x05,
	0x75,
	0x01,
	0x05,
	0x08,
	0x19,
	0x01,
	0x29,
	0x05,
	0x91,
	0x02,
	0x95,
	0x01,
	0x75,
	0x03,
	0x91,
	0x03,
	0x05,
	0x07,
	0x15,
	0x00,
	0x25,
	0x65,
	0x19,
	0x00,
	0x29,
	0x65,
	0x75,
	0x08,
	0x95,
	0x06,
	0x81,
	0x00,
	0xC0,

	0x05,
	0x01,
	0x09,
	0x06,
	0xA1,
	0x01,
	0x85,
	BLE_NKRO_REPORT_ID,
	0x05,
	0x07,
	0x19,
	0xE0,
	0x29,
	0xE7,
	0x15,
	0x00,
	0x25,
	0x01,
	0x75,
	0x01,
	0x95,
	0x08,
	0x81,
	0x02,
	0x05,
	0x07,
	0x19,
	0x00,
	0x29,
	0xF7,
	0x15,
	0x00,
	0x25,
	0x01,
	0x75,
	0x01,
	0x95,
	0xF8,
	0x81,
	0x02,
	0xC0,

	0x05,
	0x0C,
	0x09,
	0x01,
	0xA1,
	0x01,
	0x85,
	BLE_CONSUMER_REPORT_ID,
	0x15,
	0x00,
	0x26,
	0x9C,
	0x02,
	0x19,
	0x00,
	0x2A,
	0x9C,
	0x02,
	0x95,
	0x03,
	0x75,
	0x10,
	0x81,
	0x00,
	0xC0,
};

static void refresh_bond_state(void);
static void log_bond_state(const char *reason);
static void clear_tx_queue(void);
static void start_pairing_mode(void);
static void stop_pairing_mode(void);
static bool has_bond_for_id(uint8_t id);
static void publish_ble_state(uint8_t state, int err);
static void security_request_work_handler(struct k_work *work);
static int start_advertising(void);
static int stop_advertising(void);
static void update_link_state(const char *reason);
static void reset_hid_drop_reason(void);
static void clear_pairing_repair_state_with_reason(const char *reason);
static void schedule_user_clear_finalize(k_timeout_t delay);
static int resume_advertising_after_control(const char *reason);
static bool bond_cleanup_is_user_requested(const char *reason);

static void reset_runtime_flags(void)
{
	ble_ready = false;
	ble_mode_active = false;
	advertising = false;
	connected = false;
	atomic_set(&s_ble_state, BLE_STATE_DISCONNECTED);
	bonded = false;
	link_encrypted = false;
	bond_repair_required = false;
	pending_bond_cleanup = false;
	stale_host_key_backoff = false;
	pairing_mode_active = false;
	hids_notify_cccd_count = 0;
	stale_host_key_failures = 0;
	security_repair_failures = 0;
	atomic_set(&ble_notify_in_flight, 0);
	ble_retry_pending = false;
	active_identity = BT_ID_DEFAULT;
	next_adv_restart_delay_ms = BLE_ADV_RESTART_FAST_MS;
	pending_bond_cleanup_reason = NULL;
	protocol_mode = 1;
	last_drop_reason = HID_DROP_NONE;
	user_clear_active = false;
	ble_teardown_active = false;
	last_published_state = 0xFF;
	last_published_connected = false;
	last_published_bonded = false;
	last_published_err = 0;
}

static void reset_hid_drop_reason(void)
{
	last_drop_reason = HID_DROP_NONE;
}

static bool user_clear_in_progress(void)
{
	return user_clear_active ||
	       (pending_bond_cleanup &&
	       pending_bond_cleanup_reason != NULL &&
	       strcmp(pending_bond_cleanup_reason, "USER_CLEAR") == 0);
}

static bool repair_pending(void)
{
	return bond_repair_required || stale_host_key_backoff;
}

static uint8_t conn_id_get(struct bt_conn *conn)
{
	struct bt_conn_info info;

	if (!conn)
	{
		return BT_ID_DEFAULT;
	}

	if (bt_conn_get_info(conn, &info) == 0)
	{
		return info.id;
	}

	return BT_ID_DEFAULT;
}

static const char *hid_drop_reason_str(enum hid_drop_reason reason)
{
	switch (reason)
	{
	case HID_DROP_NOT_BLE:
		return "not_ble";
	case HID_DROP_NO_CONN:
		return "no_conn";
	case HID_DROP_NO_SEC:
		return "no_sec";
	case HID_DROP_NO_CCCD:
		return "no_cccd";
	case HID_DROP_NONE:
	default:
		return "none";
	}
}

static void log_hid_drop(enum hid_drop_reason reason)
{
	if (reason == HID_DROP_NONE || reason == last_drop_reason)
	{
		return;
	}

	last_drop_reason = reason;
	if (reason == HID_DROP_NOT_BLE)
	{
		LOG_DBG("HID_DROP reason=%s", hid_drop_reason_str(reason));
		return;
	}

	LOG_WRN("HID_DROP reason=%s", hid_drop_reason_str(reason));
}

static enum hid_drop_reason hid_drop_reason_from_state(void)
{
	uint8_t current_state = atomic_get(&s_ble_state);

	if (!ble_mode_active)
	{
		return HID_DROP_NOT_BLE;
	}

	if (!connected || active_conn == NULL)
	{
		return HID_DROP_NO_CONN;
	}

	if (current_state < BLE_STATE_ENCRYPTED || !link_encrypted)
	{
		return HID_DROP_NO_SEC;
	}

	if (hids_notify_cccd_count == 0 || current_state != BLE_STATE_READY)
	{
		return HID_DROP_NO_CCCD;
	}

	return HID_DROP_NONE;
}

static void clear_pairing_repair_state(void)
{
	bond_repair_required = false;
	stale_host_key_backoff = false;
	stop_pairing_mode();
	stale_host_key_failures = 0;
	security_repair_failures = 0;
	next_adv_restart_delay_ms = BLE_ADV_RESTART_FAST_MS;
}

static void clear_pairing_repair_state_with_reason(const char *reason)
{
	clear_pairing_repair_state();
	LOG_INF("REPAIR_STATE clear reason=%s", reason != NULL ? reason : "UNKNOWN");
}

static void schedule_user_clear_finalize(k_timeout_t delay)
{
	(void)k_work_reschedule(&user_clear_finalize_work, delay);
}

static int resume_advertising_after_control(const char *reason)
{
	int ret;

	if (!ble_mode_active)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_OFF, 0);
		return 0;
	}

	ret = start_advertising();
	if (ret == 0)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_ADVERTISING, 0);
	}
	else
	{
		publish_ble_state(BLE_TRANSPORT_STATE_OFF, ret);
		LOG_ERR("%s 后重启广播失败 err=%d", reason != NULL ? reason : "控制流程", ret);
	}

	return ret;
}

static void submit_keep_alive_once(void)
{
	struct keep_alive_event *event = new_keep_alive_event();

	if (event == NULL)
	{
		LOG_ERR("keep_alive_event 内存分配失败");
		return;
	}

	APP_EVENT_SUBMIT(event);
}

static void ble_keep_alive_work_handler(struct k_work *work)
{
	if (!ble_mode_active || !connected)
	{
		return;
	}

	submit_keep_alive_once();
	(void)k_work_reschedule(&ble_keep_alive_work,
							K_MSEC(BLE_KEEP_ALIVE_INTERVAL_MS));
}

static void start_ble_keep_alive(void)
{
	if (!ble_mode_active || !connected)
	{
		return;
	}

	submit_keep_alive_once();
	(void)k_work_reschedule(&ble_keep_alive_work,
							K_MSEC(BLE_KEEP_ALIVE_INTERVAL_MS));
}

static void stop_ble_keep_alive(void)
{
	(void)k_work_cancel_delayable(&ble_keep_alive_work);
}

static void ble_tx_retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)k_work_submit_to_queue(&ble_tx_work_q, &ble_tx_work);
}

static void pairing_timeout_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

		if (user_clear_active)
	{
		return;
	}

	if (!pairing_mode_active || connected)
	{
		return;
	}

	pairing_mode_active = false;
	bond_repair_required = true;
	stale_host_key_backoff = true;
	if (stale_host_key_failures == 0)
	{
		stale_host_key_failures = 1;
	}
	next_adv_restart_delay_ms = BLE_ADV_RESTART_STALE_HOST_MS;

	LOG_WRN("配对模式超时，请在主机蓝牙设置中删除 NRF Keyboard 后重新搜索配对，退避=%ums",
			next_adv_restart_delay_ms);

	if (advertising)
	{
		(void)stop_advertising();
	}

	if (ble_mode_active)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_ADVERTISING, 0);
		(void)k_work_reschedule(&adv_restart_work,
								K_MSEC(next_adv_restart_delay_ms));
	}
}

static void start_pairing_mode(void)
{
	pairing_mode_active = true;
	(void)k_work_reschedule(&pairing_timeout_work,
							K_MSEC(BLE_PAIRING_MODE_TIMEOUT_MS));
}

static void stop_pairing_mode(void)
{
	pairing_mode_active = false;
	(void)k_work_cancel_delayable(&pairing_timeout_work);
}

static void select_preferred_identity(void)
{
	active_identity = BT_ID_DEFAULT;
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	const char *reason = "default";

	bt_id_get(addrs, &count);
	for (uint8_t id = 0; id < count; id++)
	{
		if (id != BT_ID_DEFAULT)
		{
			active_identity = id;
			reason = "non_default";
			break;
		}
	}

	LOG_INF("IDENTITY_LIST count=%u", (uint32_t)count);
	LOG_INF("IDENTITY_SELECT active_id=%u reason=%s", active_identity, reason);
	LOG_INF("蓝牙身份固定 id=%u", active_identity);
}

static int create_pairing_identity_if_needed(void)
{
	refresh_bond_state();
	return 0;
}

static void submit_ble_state(uint8_t state, bool is_connected, bool is_bonded, int err)
{
	struct ble_state_event *event = new_ble_state_event();

	if (event == NULL)
	{
		LOG_ERR("ble_state_event 内存分配失败");
		return;
	}

	if (state == BLE_TRANSPORT_STATE_OFF ||
	    state == BLE_TRANSPORT_STATE_ADVERTISING)
	{
		is_connected = false;
	}

	if (!is_connected &&
	    (state == BLE_TRANSPORT_STATE_ENCRYPTED ||
	     state == BLE_TRANSPORT_STATE_READY))
	{
		return;
	}

	event->state = state;
	event->connected = is_connected;
	event->bonded = is_bonded;
	event->err = err;
	APP_EVENT_SUBMIT(event);
}

static void publish_ble_state(uint8_t state, int err)
{
	bool is_connected;
	bool is_bonded;

	if (!ble_mode_active && state != BLE_TRANSPORT_STATE_OFF)
	{
		LOG_DBG("BLE_STATE_DROP state=%u reason=not_ble", state);
		return;
	}

	if ((ble_teardown_active || user_clear_active) &&
	    (state == BLE_TRANSPORT_STATE_CONNECTED ||
	     state == BLE_TRANSPORT_STATE_ENCRYPTED ||
	     state == BLE_TRANSPORT_STATE_READY))
	{
		LOG_DBG("BLE_STATE_DROP state=%u reason=teardown user_clear=%d",
			state, user_clear_active);
		return;
	}

	if (active_conn == NULL &&
	    (state == BLE_TRANSPORT_STATE_CONNECTED ||
	     state == BLE_TRANSPORT_STATE_ENCRYPTED ||
	     state == BLE_TRANSPORT_STATE_READY))
	{
		LOG_DBG("BLE_STATE_DROP state=%u reason=no_conn", state);
		return;
	}

	is_connected = active_conn != NULL;
	is_bonded = has_bond_for_id(active_identity);

	if (state == BLE_TRANSPORT_STATE_OFF ||
	    state == BLE_TRANSPORT_STATE_ADVERTISING)
	{
		is_connected = false;
	}

	if (state == last_published_state &&
	    is_connected == last_published_connected &&
	    is_bonded == last_published_bonded &&
	    err == last_published_err) {
		LOG_DBG("BLE_STATE_DROP state=%u reason=duplicate", state);
		return;
	}
	last_published_state = state;
	last_published_connected = is_connected;
	last_published_bonded = is_bonded;
	last_published_err = err;
	submit_ble_state(state, is_connected, is_bonded, err);
}

static uint8_t transport_state_from_link(ble_state_t state)
{
	switch (state)
	{
	case BLE_STATE_READY:
		return BLE_TRANSPORT_STATE_READY;
	case BLE_STATE_ENCRYPTED:
		return BLE_TRANSPORT_STATE_ENCRYPTED;
	case BLE_STATE_CONNECTED:
	default:
		return BLE_TRANSPORT_STATE_CONNECTED;
	}
}

static void update_link_state(const char *reason)
{
	ble_state_t next_state = BLE_STATE_DISCONNECTED;
	ble_state_t old_state = (ble_state_t)atomic_get(&s_ble_state);

	if (connected)
	{
		if (link_encrypted && hids_notify_cccd_count > 0)
		{
			next_state = BLE_STATE_READY;
		}
		else if (link_encrypted)
		{
			next_state = BLE_STATE_ENCRYPTED;
		}
		else
		{
			next_state = BLE_STATE_CONNECTED;
		}
	}

	if (old_state == next_state)
	{
		return;
	}

	atomic_set(&s_ble_state, next_state);
	LOG_INF("蓝牙链路状态 state=%u encrypted=%d cccd=%u 原因=%s",
			next_state,
			link_encrypted ? 1 : 0,
			hids_notify_cccd_count,
			reason != NULL ? reason : "UNKNOWN");

	if (connected)
	{
		publish_ble_state(transport_state_from_link(next_state), 0);
	}
}

static ssize_t read_hids_info(struct bt_conn *conn,
							  const struct bt_gatt_attr *attr,
							  void *buf, uint16_t len,
							  uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
							 hids_info, sizeof(hids_info));
}

static ssize_t read_report_map(struct bt_conn *conn,
							   const struct bt_gatt_attr *attr,
							   void *buf, uint16_t len,
							   uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
							 hid_report_map, sizeof(hid_report_map));
}

static ssize_t read_protocol_mode(struct bt_conn *conn,
								  const struct bt_gatt_attr *attr,
								  void *buf, uint16_t len,
								  uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
							 &protocol_mode, sizeof(protocol_mode));
}

static bool publish_protocol_mode(uint8_t proto)
{
	struct set_protocol_event *event = new_set_protocol_event();

	if (event == NULL)
	{
		LOG_ERR("set_protocol_event 内存分配失败");
		return false;
	}

	event->protocol = proto;
	APP_EVENT_SUBMIT(event);
	return true;
}

static ssize_t write_protocol_mode(struct bt_conn *conn,
								   const struct bt_gatt_attr *attr,
								   const void *buf, uint16_t len,
								   uint16_t offset, uint8_t flags)
{
	const uint8_t *value = buf;

	if (offset != 0 || len != 1 || value[0] > 1)
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (!publish_protocol_mode(value[0]))
	{
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	protocol_mode = value[0];
	LOG_INF("蓝牙协议切换为 %s", protocol_mode == 0 ? "Boot" : "Report");

	return len;
}

static ssize_t write_ctrl_point(struct bt_conn *conn,
								const struct bt_gatt_attr *attr,
								const void *buf, uint16_t len,
								uint16_t offset, uint8_t flags)
{
	if (offset != 0 || len != 1)
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	LOG_DBG("HID Control Point=%u", ((const uint8_t *)buf)[0]);
	return len;
}

static ssize_t read_boot_output(struct bt_conn *conn,
								const struct bt_gatt_attr *attr,
								void *buf, uint16_t len,
								uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
							 &boot_kbd_output, sizeof(boot_kbd_output));
}

static ssize_t write_boot_output(struct bt_conn *conn,
								 const struct bt_gatt_attr *attr,
								 const void *buf, uint16_t len,
								 uint16_t offset, uint8_t flags)
{
	if (offset != 0 || len != 1)
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	boot_kbd_output = ((const uint8_t *)buf)[0];
	LOG_DBG("Boot LED 状态: NumLock=%d CapsLock=%d ScrollLock=%d",
			(boot_kbd_output & BIT(0)) ? 1 : 0,
			(boot_kbd_output & BIT(1)) ? 1 : 0,
			(boot_kbd_output & BIT(2)) ? 1 : 0);

	return len;
}

static ssize_t read_report_output(struct bt_conn *conn,
								  const struct bt_gatt_attr *attr,
								  void *buf, uint16_t len,
								  uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
							 &report_kbd_output, sizeof(report_kbd_output));
}

static ssize_t write_report_output(struct bt_conn *conn,
								   const struct bt_gatt_attr *attr,
								   const void *buf, uint16_t len,
								   uint16_t offset, uint8_t flags)
{
	if (offset != 0 || len != 1)
	{
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	report_kbd_output = ((const uint8_t *)buf)[0];
	LOG_DBG("Report LED 状态: NumLock=%d CapsLock=%d ScrollLock=%d",
			(report_kbd_output & BIT(0)) ? 1 : 0,
			(report_kbd_output & BIT(1)) ? 1 : 0,
			(report_kbd_output & BIT(2)) ? 1 : 0);

	return len;
}

static ssize_t read_report_ref(struct bt_conn *conn,
							   const struct bt_gatt_attr *attr,
							   void *buf, uint16_t len,
							   uint16_t offset)
{
	const uint8_t *ref = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, ref, 2);
}

static ssize_t read_empty_report(struct bt_conn *conn,
								 const struct bt_gatt_attr *attr,
								 void *buf, uint16_t len,
								 uint16_t offset)
{
	static const uint8_t empty[BLE_NKRO_REPORT_SIZE];

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
							 empty, sizeof(empty));
}

static void hids_notify_enabled_cb(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	bool enabled = (value == BT_GATT_CCC_NOTIFY);

	if (enabled)
	{
		if (hids_notify_cccd_count < BLE_HIDS_NOTIFY_CCCD_MAX)
		{
			hids_notify_cccd_count++;
		}
	}
	else if (hids_notify_cccd_count > 0)
	{
		hids_notify_cccd_count--;
	}

	bool is_ready = (hids_notify_cccd_count > 0 && link_encrypted);
	bool was_ready = (atomic_get(&s_ble_state) == BLE_STATE_READY);

	reset_hid_drop_reason();
	LOG_DBG("CCCD %s (0x%04X)", enabled ? "enable" : "disable", value);
	LOG_INF("CCCD_STATE mask=0x%02x encrypted=%d ready=%d",
		(1u << hids_notify_cccd_count) - 1,
		link_encrypted ? 1 : 0, is_ready ? 1 : 0);

		if (!ble_teardown_active && !user_clear_active)
		{
			if (was_ready != is_ready)
			{
				LOG_INF("BLE_%s", is_ready ? "READY" : "NOT_READY");
			}
			update_link_state(enabled ? "CCCD_ENABLE" : "CCCD_DISABLE");
		}
}

BT_GATT_SERVICE_DEFINE(ble_hids_svc,
					   BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
											  BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
											  read_protocol_mode, write_protocol_mode, NULL),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO,
											  BT_GATT_CHRC_READ,
											  BT_GATT_PERM_READ_ENCRYPT,
											  read_hids_info, NULL, NULL),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP,
											  BT_GATT_CHRC_READ,
											  BT_GATT_PERM_READ_ENCRYPT,
											  read_report_map, NULL, NULL),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
											  BT_GATT_CHRC_WRITE_WITHOUT_RESP,
											  BT_GATT_PERM_WRITE_ENCRYPT,
											  NULL, write_ctrl_point, NULL),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_BOOT_KB_IN_REPORT,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
											  BT_GATT_PERM_READ_ENCRYPT,
											  read_empty_report, NULL, NULL),
					   BT_GATT_CCC(hids_notify_enabled_cb,
								   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_BOOT_KB_OUT_REPORT,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
											  BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
											  read_boot_output, write_boot_output, NULL),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
											  BT_GATT_PERM_READ_ENCRYPT,
											  read_empty_report, NULL, NULL),
					   BT_GATT_CCC(hids_notify_enabled_cb,
								   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
					   BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
										  BT_GATT_PERM_READ_ENCRYPT,
										  read_report_ref, NULL, (void *)report_ref_kbd),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
											  BT_GATT_PERM_READ_ENCRYPT,
											  read_empty_report, NULL, NULL),
					   BT_GATT_CCC(hids_notify_enabled_cb,
								   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
					   BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
										  BT_GATT_PERM_READ_ENCRYPT,
										  read_report_ref, NULL, (void *)report_ref_nkro),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
											  BT_GATT_PERM_READ_ENCRYPT,
											  read_empty_report, NULL, NULL),
					   BT_GATT_CCC(hids_notify_enabled_cb,
								   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
					   BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
										  BT_GATT_PERM_READ_ENCRYPT,
										  read_report_ref, NULL, (void *)report_ref_consumer),
					   BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
											  BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
											  BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
											  read_report_output, write_report_output, NULL),
						   BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
											  BT_GATT_PERM_READ_ENCRYPT,
											  read_report_ref, NULL, (void *)report_ref_kbd_output));

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
				  BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int start_advertising(void)
{
	struct bt_le_adv_param adv_param = {
		.id = active_identity,
		.sid = 0,
		.secondary_max_skip = 0,
		.options = BT_LE_ADV_OPT_CONN |
			   BT_LE_ADV_OPT_USE_IDENTITY,
		.interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
		.interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
		.peer = NULL,
	};
	int ret;

	if (!ble_ready || advertising || connected || !ble_mode_active)
	{
		return 0;
	}

	LOG_INF("ADV_START active_id=%u adv_param.id=%u bonded=%d repair=%d user_clear=%d",
			active_identity, adv_param.id, bonded ? 1 : 0,
			repair_pending() ? 1 : 0, user_clear_in_progress() ? 1 : 0);

	ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad),
						  sd, ARRAY_SIZE(sd));
	if (ret < 0 && ret != -EALREADY)
	{
		LOG_ERR("ADV_START err=%d active_id=%u adv_param.id=%u",
				ret, active_identity, adv_param.id);
		LOG_ERR("蓝牙广播启动失败 err=%d", ret);
		publish_ble_state(BLE_TRANSPORT_STATE_ERROR, ret);
		if (ret == -ENOMEM && ble_mode_active)
		{
			(void)k_work_reschedule(&adv_restart_work, K_MSEC(500));
		}
		return ret;
	}

	advertising = true;
	LOG_INF("蓝牙广播 id=%u bond=%d 配对=%d 修复=%d 旧主机=%d 延时=%ums",
			active_identity,
			bonded ? 1 : 0,
			pairing_mode_active ? 1 : 0,
			bond_repair_required ? 1 : 0,
			stale_host_key_backoff ? 1 : 0,
			next_adv_restart_delay_ms);
	return 0;
}

static void adv_restart_work_handler(struct k_work *work)
{
	if (user_clear_active || !ble_mode_active || connected || advertising)
	{
		return;
	}

	if (start_advertising() == 0)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_ADVERTISING, 0);
	}
}

static void clear_all_bonds(const char *reason)
{
	int ret;
	bool user_requested = bond_cleanup_is_user_requested(reason);
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);

	if (advertising)
	{
		ret = stop_advertising();
		if (ret < 0)
		{
			publish_ble_state(BLE_TRANSPORT_STATE_ERROR, ret);
			return;
		}
	}

	if (user_requested)
	{
		bt_id_get(addrs, &count);
		for (uint8_t id = 0; id < count; id++)
		{
			ret = bt_unpair(id, BT_ADDR_LE_ANY);
			LOG_INF("UNPAIR id=%u err=%d", id, ret);
		}
		ret = 0;
		log_bond_state("UNPAIR_ALL");
	}
	else
	{
		ret = bt_unpair(active_identity, BT_ADDR_LE_ANY);
		LOG_INF("UNPAIR id=%u err=%d", active_identity, ret);
		log_bond_state("UNPAIR_ACTIVE");
	}
	if (ret < 0)
	{
		LOG_WRN("蓝牙配对清理失败 id=%u err=%d 原因=%s",
				active_identity, ret, reason);
		return;
	}

	refresh_bond_state();
	start_pairing_mode();
	bond_repair_required = false;
	stale_host_key_backoff = false;
	stale_host_key_failures = 0;
	security_repair_failures = 0;
	next_adv_restart_delay_ms = BLE_ADV_RESTART_FAST_MS;
	LOG_INF("REPAIR_STATE clear reason=%s", reason != NULL ? reason : "UNKNOWN");
	LOG_WRN("蓝牙配对已清理 id=%u 原因=%s bond=%d，进入可发现配对广播",
			active_identity, reason, bonded ? 1 : 0);
}

static int rotate_identity_for_user_clear(void)
{
	uint8_t old_id = active_identity;
	int ret;

	if (active_identity == BT_ID_DEFAULT)
	{
		ret = bt_id_create(NULL, NULL);
		LOG_INF("ID_CREATE old=%u new=%d", old_id, ret);
		if (ret < 0)
		{
			return ret;
		}

		active_identity = (uint8_t)ret;
		log_bond_state("ID_CREATE");
		return 0;
	}

	ret = bt_id_reset(active_identity, NULL, NULL);
	LOG_INF("ID_RESET id=%u ret=%d", active_identity, ret);
	if (ret >= 0)
	{
		active_identity = (uint8_t)ret;
	}
	else
	{
		LOG_ERR("ID_RESET failed id=%u err=%d", active_identity, ret);
	}
	return ret;
}

static void user_clear_finalize_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	if (!user_clear_active)
	{
		return;
	}

	if (connected || active_conn != NULL)
	{
		schedule_user_clear_finalize(K_MSEC(100));
		return;
	}

	if (advertising)
	{
		ret = stop_advertising();
		if (ret < 0)
		{
			publish_ble_state(BLE_TRANSPORT_STATE_ERROR, ret);
			return;
		}
	}

	clear_all_bonds("USER_CLEAR");
	ret = rotate_identity_for_user_clear();
	if (ret < 0)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_ERROR, ret);
		return;
	}

	refresh_bond_state();

	ble_teardown_active = false;
	ret = resume_advertising_after_control("USER_CLEAR");
	if (ret == 0)
	{
		LOG_INF("USER_CLEAR done active_id=%u", active_identity);
		user_clear_active = false;
	}
}

static bool bond_cleanup_is_user_requested(const char *reason)
{
	return reason != NULL && strcmp(reason, "USER_CLEAR") == 0;
}

static void bond_cleanup_work_handler(struct k_work *work)
{
	const char *reason;
	bool user_requested;
	uint32_t restart_delay_ms;

	ARG_UNUSED(work);

		if (user_clear_active)
	{
		return;
	}

	if (!pending_bond_cleanup)
	{
		return;
	}

	if (connected || active_conn != NULL)
	{
		(void)k_work_reschedule(&bond_cleanup_work,
								K_MSEC(BLE_BOND_CLEANUP_DELAY_MS));
		return;
	}

	reason = pending_bond_cleanup_reason != NULL ?
			 pending_bond_cleanup_reason :
			 "DELAYED_REPAIR";
	user_requested = bond_cleanup_is_user_requested(reason);
	pending_bond_cleanup = false;
	pending_bond_cleanup_reason = NULL;

	LOG_WRN("蓝牙配对清理执行 原因=%s 延时=%ums",
			reason, BLE_BOND_CLEANUP_DELAY_MS);
	clear_all_bonds(reason);

	if (user_requested)
	{
		restart_delay_ms = BLE_ADV_RESTART_FAST_MS;
	}
	else
	{
		bond_repair_required = true;
		stale_host_key_backoff = true;
		if (stale_host_key_failures == 0)
		{
			stale_host_key_failures = 1;
		}
		restart_delay_ms = BLE_ADV_RESTART_STALE_HOST_MS;
	}

	next_adv_restart_delay_ms = restart_delay_ms;
	if (ble_mode_active && !connected && !advertising)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_ADVERTISING, 0);
		(void)k_work_reschedule(&adv_restart_work,
								K_MSEC(restart_delay_ms));
	}
}

static int stop_advertising(void)
{
	int ret;

	if (!advertising)
	{
		return 0;
	}

	ret = bt_le_adv_stop();
	LOG_INF("ADV_STOP err=%d user_clear=%d repair=%d",
			(ret < 0 && ret != -EALREADY) ? ret : 0,
			user_clear_in_progress() ? 1 : 0, repair_pending() ? 1 : 0);
	if (ret < 0 && ret != -EALREADY)
	{
		LOG_ERR("蓝牙广播停止失败 err=%d", ret);
		return ret;
	}

	advertising = false;
	return 0;
}

static void request_user_bond_clear(void)
{
	int ret;

	LOG_WRN("用户清理蓝牙配对");
	LOG_INF("BLE_CONTROL clear bonds");
	(void)k_work_cancel_delayable(&adv_restart_work);
	(void)k_work_cancel_delayable(&security_request_work);
	user_clear_active = true;
	ble_teardown_active = true;
	LOG_INF("USER_CLEAR begin mode_ble=%d conn=%d",
		ble_mode_active ? 1 : 0,
		(active_conn != NULL || connected) ? 1 : 0);
	ret = stop_advertising();
	if (ret < 0)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_ERROR, ret);
	}

	if (active_conn != NULL)
	{
		ret = bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (ret < 0)
		{
			LOG_WRN("清理前断开失败 err=%d", ret);
			if (ret == -ENOTCONN || ret == -EINVAL)
			{
				bt_conn_unref(active_conn);
				active_conn = NULL;
				connected = false;
			}
		}
		schedule_user_clear_finalize(K_MSEC(100));
		return;
	}

	schedule_user_clear_finalize(K_NO_WAIT);

	if (false)
	{
		next_adv_restart_delay_ms = BLE_ADV_RESTART_FAST_MS;
		LOG_INF("用户清理配对后延迟重启广播 %ums", next_adv_restart_delay_ms);
		(void)k_work_reschedule(&adv_restart_work,
								K_MSEC(next_adv_restart_delay_ms));
	}
}

static void enter_ble_mode(void)
{
	int ret;

	ble_mode_active = true;
	reset_hid_drop_reason();
	ret = start_advertising();
	if (ret == 0)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_ADVERTISING, 0);
	}
	else
	{
		publish_ble_state(BLE_TRANSPORT_STATE_OFF, ret);
	}
}

static void leave_ble_mode(void)
{
	int ret;

	ble_mode_active = false;
	ble_teardown_active = true;
	reset_hid_drop_reason();
	atomic_set(&s_ble_state, BLE_STATE_DISCONNECTED);
	stop_ble_keep_alive();
	(void)k_work_cancel_delayable(&security_request_work);
	clear_tx_queue();
	ret = stop_advertising();
	if (ret < 0)
	{
		publish_ble_state(BLE_TRANSPORT_STATE_ERROR, ret);
	}

	if (active_conn != NULL)
	{
		ret = bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (ret < 0)
		{
			LOG_ERR("蓝牙断开失败 err=%d", ret);
			publish_ble_state(BLE_TRANSPORT_STATE_ERROR, ret);
		}
	}

	LOG_INF("蓝牙广播已停止");
	publish_ble_state(BLE_TRANSPORT_STATE_OFF, 0);
}

static void has_bond(const struct bt_bond_info *info, void *user_data)
{
	bool *found = user_data;

	ARG_UNUSED(info);
	*found = true;
}

static void refresh_bond_state(void)
{
	bool found = false;

	bt_foreach_bond(active_identity, has_bond, &found);
	bonded = found;
}

static bool has_bond_for_id(uint8_t id)
{
	bool found = false;

	bt_foreach_bond(id, has_bond, &found);
	return found;
}

static void log_bond_state(const char *reason)
{
	refresh_bond_state();
	LOG_INF("BOND_STATE id=%u bonded=%d reason=%s",
		active_identity,
		bonded ? 1 : 0,
		reason != NULL ? reason : "UNKNOWN");
}

static void security_request_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (active_conn == NULL || !ble_mode_active ||
	    user_clear_active || ble_teardown_active)
	{
		LOG_DBG("SECURITY_REQ_SKIP conn=%d mode=%d user_clear=%d teardown=%d",
			active_conn != NULL, ble_mode_active,
			user_clear_active, ble_teardown_active);
		return;
	}

	int err = bt_conn_set_security(active_conn, BT_SECURITY_L2);
	if (err == -EBUSY) {
		LOG_DBG("SECURITY_REQ busy ignored err=%d", err);
	} else {
		LOG_INF("SECURITY_REQ err=%d", err);
	}
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	(void)k_work_cancel_delayable(&adv_restart_work);
	advertising = false;
	reset_hid_drop_reason();

	if (err != 0)
	{
		LOG_ERR("蓝牙连接失败 err=%u", err);
		connected = false;
		link_encrypted = false;
		hids_notify_cccd_count = 0;
		atomic_set(&s_ble_state, BLE_STATE_DISCONNECTED);
		publish_ble_state(BLE_TRANSPORT_STATE_ERROR, -err);
		if (ble_mode_active)
		{
			if (start_advertising() == 0)
			{
				publish_ble_state(BLE_TRANSPORT_STATE_ADVERTISING, 0);
			}
		}
		return;
	}

	active_conn = bt_conn_ref(conn);
	connected = true;
	link_encrypted = false;
	ble_teardown_active = false;
	refresh_bond_state();
	update_link_state("CONNECTED");
	if (pairing_mode_active)
	{
		(void)k_work_cancel_delayable(&pairing_timeout_work);
	}
	start_ble_keep_alive();
	LOG_INF("CONNECTED id=%u", conn_id_get(conn));
	log_bond_state("CONNECTED");
	(void)k_work_cancel_delayable(&security_request_work);
	(void)k_work_schedule(&security_request_work, K_MSEC(BLE_SECURITY_REQ_DELAY_MS));
	LOG_INF("SECURITY_REQ_SCHEDULE delay=%ums", BLE_SECURITY_REQ_DELAY_MS);

	publish_ble_state(BLE_TRANSPORT_STATE_CONNECTED, 0);
	LOG_INF("蓝牙已连接 id=%u bond=%d", active_identity, bonded ? 1 : 0);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	uint32_t restart_delay_ms = BLE_ADV_RESTART_FAST_MS;
	bool was_user_clear = user_clear_active;

	/* 第一时间清空连接态标志，防止后续 update_link_state / 状态事件读到旧值 */
	link_encrypted = false;
	hids_notify_cccd_count = 0;
	connected = false;
	atomic_set(&s_ble_state, BLE_STATE_DISCONNECTED);

	if (active_conn != NULL)
	{
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	reset_hid_drop_reason();
	stop_ble_keep_alive();
	(void)k_work_cancel_delayable(&ble_tx_retry_work);
	(void)k_work_cancel_delayable(&security_request_work);
	atomic_set(&ble_notify_in_flight, 0);
	clear_tx_queue();
	if (pairing_mode_active)
	{
		(void)k_work_reschedule(&pairing_timeout_work,
					K_MSEC(BLE_PAIRING_MODE_TIMEOUT_MS));
	}
	refresh_bond_state();
	LOG_INF("蓝牙已断开 reason=%u", reason);

	LOG_INF("DISCONNECTED reason=%u user_clear=%d repair=%d",
		reason, was_user_clear ? 1 : 0, repair_pending() ? 1 : 0);

	if (was_user_clear)
	{
		schedule_user_clear_finalize(K_MSEC(100));
		return;
	}

	if (reason == BT_HCI_ERR_REMOTE_USER_TERM_CONN && pairing_mode_active && !bonded)
	{
		if (stale_host_key_failures < UINT8_MAX)
		{
			stale_host_key_failures++;
		}
		bond_repair_required = true;
		stale_host_key_backoff = true;
		LOG_WRN("主机主动断开配对连接 reason=%u 次数=%u，进入旧主机退避",
			reason, stale_host_key_failures);
	}

	if (pending_bond_cleanup)
	{
		ble_teardown_active = false;
		publish_ble_state(ble_mode_active ? BLE_TRANSPORT_STATE_ADVERTISING : BLE_TRANSPORT_STATE_OFF, 0);
		(void)k_work_reschedule(&bond_cleanup_work,
					K_MSEC(BLE_BOND_CLEANUP_DELAY_MS));
		return;
	}

	ble_teardown_active = false;

	if (ble_mode_active)
	{
		if (stale_host_key_backoff)
		{
			restart_delay_ms = stale_host_key_failures >=
					   BLE_STALE_HOST_LONG_BACKOFF_THRESHOLD
				   ? BLE_ADV_RESTART_STALE_HOST_LONG_MS
				   : BLE_ADV_RESTART_STALE_HOST_MS;
		}
		else if (bond_repair_required)
		{
			restart_delay_ms = BLE_ADV_RESTART_REPAIR_MS;
		}

		next_adv_restart_delay_ms = restart_delay_ms;
		publish_ble_state(BLE_TRANSPORT_STATE_ADVERTISING, 0);
		(void)k_work_reschedule(&adv_restart_work, K_MSEC(restart_delay_ms));
	}
	else
	{
		publish_ble_state(BLE_TRANSPORT_STATE_OFF, 0);
	}
}
BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

static void security_changed_cb(struct bt_conn *conn,
								bt_security_t level,
								enum bt_security_err err)
{
	bool id_bonded = has_bond_for_id(conn_id_get(conn));

	if (err) {
		LOG_WRN("SECURITY level=%u err=%u(%s) bonded=%d",
			level, err, bt_security_err_to_str(err), id_bonded);
	} else {
		LOG_INF("SECURITY level=%u err=0 bonded=%d", level, id_bonded);
	}

	if (!err && level >= BT_SECURITY_L2) {
		if (ble_teardown_active || user_clear_active ||
		    !ble_mode_active || active_conn == NULL) {
			LOG_DBG("SECURITY_DROP level=%u teardown=%d user_clear=%d mode=%d conn=%d",
				level,
				ble_teardown_active,
				user_clear_active,
				ble_mode_active,
				active_conn != NULL);
			return;
		}

		link_encrypted = true;
		clear_pairing_repair_state_with_reason("SECURITY_OK");
		refresh_bond_state();
		start_ble_keep_alive();
		reset_hid_drop_reason();
		update_link_state("SECURITY_OK");
		LOG_INF("蓝牙加密完成 level=%u bond=%d ready=%d",
			level, bonded ? 1 : 0,
			atomic_get(&s_ble_state) == BLE_STATE_READY ? 1 : 0);
	} else {
		link_encrypted = false;
		refresh_bond_state();
		update_link_state("SECURITY_FAIL");
	}
}

BT_CONN_CB_DEFINE(security_callbacks) = {
	.security_changed = security_changed_cb,
};

static enum bt_security_err auth_pairing_accept(
	struct bt_conn *conn,
	const struct bt_conn_pairing_feat *const feat)
{
	LOG_INF("AUTH_PAIRING_ACCEPT io_cap=%u oob=%u auth_req=0x%02x "
		"max_key_size=%u init_key_dist=0x%02x resp_key_dist=0x%02x",
		feat->io_capability, feat->oob_data_flag, feat->auth_req,
		feat->max_enc_key_size, feat->init_key_dist, feat->resp_key_dist);
	return BT_SECURITY_ERR_SUCCESS;
}

static void auth_pairing_confirm(struct bt_conn *conn)
{
	int err = bt_conn_auth_pairing_confirm(conn);
	LOG_INF("AUTH_PAIRING_CONFIRM err=%d", err);
}

static void auth_cancel(struct bt_conn *conn)
{
	LOG_WRN("AUTH_CANCEL");
}

static struct bt_conn_auth_cb auth_cb = {
	.pairing_accept = auth_pairing_accept,
	.pairing_confirm = auth_pairing_confirm,
	.cancel = auth_cancel,
	.passkey_display = NULL,
	.passkey_entry = NULL,
	.passkey_confirm = NULL,
};

static void pairing_complete_cb(struct bt_conn *conn, bool bonded_flag)
{
	uint8_t id = conn_id_get(conn);
	LOG_INF("PAIRING_COMPLETE id=%u bonded=%d", id, bonded_flag);
	LOG_INF("BOND_STATE id=%u bonded=%d reason=PAIRING_COMPLETE",
		id, has_bond_for_id(id));
	clear_pairing_repair_state_with_reason("PAIRING_COMPLETE");
	pending_bond_cleanup = false;
	pending_bond_cleanup_reason = NULL;
	link_encrypted = (bt_conn_get_security(conn) >= BT_SECURITY_L2);
	refresh_bond_state();
	start_ble_keep_alive();
	update_link_state("PAIRING_COMPLETE");
	LOG_INF("蓝牙配对成功 bond=%d ready=%d",
			bonded_flag ? 1 : 0,
			atomic_get(&s_ble_state) == BLE_STATE_READY ? 1 : 0);
}

static void pairing_failed_cb(struct bt_conn *conn, enum bt_security_err reason)
{
	uint8_t id = conn_id_get(conn);
	LOG_WRN("PAIRING_FAILED id=%u reason=%u(%s) bonded=%d",
		id, reason, bt_security_err_to_str(reason), has_bond_for_id(id));
	link_encrypted = false;
	refresh_bond_state();
	update_link_state("PAIRING_FAIL");
	LOG_WRN("蓝牙配对失败 reason=%u bond=%d", reason, bonded ? 1 : 0);
}

static void bond_deleted_cb(uint8_t id, const bt_addr_le_t *peer)
{
	ARG_UNUSED(id);
	ARG_UNUSED(peer);

	refresh_bond_state();
	LOG_INF("蓝牙 bond 已删除 bond=%d", bonded ? 1 : 0);
}

static struct bt_conn_auth_info_cb auth_info_callbacks = {
	.pairing_complete = pairing_complete_cb,
	.pairing_failed = pairing_failed_cb,
	.bond_deleted = bond_deleted_cb,
};

static void notify_complete_cb(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_data);

	if (atomic_get(&ble_notify_in_flight) > 0)
	{
		atomic_dec(&ble_notify_in_flight);
	}

	if (ble_mode_active && atomic_get(&s_ble_state) == BLE_STATE_READY)
	{
		(void)k_work_submit_to_queue(&ble_tx_work_q, &ble_tx_work);
	}
}

static int notify_report(const struct bt_gatt_attr *attr,
						 const uint8_t *data, uint8_t len,
						 const char *tag)
{
	struct bt_gatt_notify_params params = {
		.attr = attr,
		.data = data,
		.len = len,
		.func = notify_complete_cb,
		.user_data = NULL,
	};
	int ret;
	const char *report_name = (tag != NULL && strstr(tag, "consumer") != NULL) ?
				      "consumer" : "kbd";

	if (!connected || active_conn == NULL || !ble_mode_active)
	{
		return -ENOTCONN;
	}

	atomic_inc(&ble_notify_in_flight);
	ret = bt_gatt_notify_cb(active_conn, &params);
	if (ret < 0)
	{
		if (atomic_get(&ble_notify_in_flight) > 0)
		{
			atomic_dec(&ble_notify_in_flight);
		}
		LOG_WRN("HID_TX err=%d report=%s", ret, report_name);
		LOG_WRN("蓝牙发送失败 err=%d tag=%s", ret, tag);
		return ret;
	}

	LOG_DBG("HID_TX err=0 report=%s", report_name);
	LOG_DBG("TX %s", tag);
	return 0;
}

static void clear_tx_queue(void)
{
	ring_buf_reset(&ble_tx_ring);
	ble_retry_pending = false;
}

static void build_consumer_wire(const uint8_t data[BLE_CONSUMER_REPORT_SIZE + 1],
								uint8_t wire[BLE_CONSUMER_REPORT_SIZE])
{
	memcpy(wire, &data[1], BLE_CONSUMER_REPORT_SIZE);
}

static int send_pending_report(const struct ble_pending_report *report)
{
	if (report->type == BLE_REPORT_TYPE_CONSUMER)
	{
		uint8_t wire[BLE_CONSUMER_REPORT_SIZE];

		build_consumer_wire(report->data, wire);
		LOG_HEXDUMP_DBG(wire, sizeof(wire), "BLE TX consumer[6B]");
		return notify_report(&ble_hids_svc.attrs[HIDS_ATTR_REPORT_CONSUMER],
							 wire, sizeof(wire), "consumer[6B]");
	}

	if (protocol_mode == 0)
	{
		LOG_HEXDUMP_DBG(report->data, BLE_BOOT_REPORT_SIZE, "BLE TX kbd[Boot 8B]");
		return notify_report(&ble_hids_svc.attrs[HIDS_ATTR_BOOT_KBD_IN],
							 report->data, BLE_BOOT_REPORT_SIZE, "kbd[Boot 8B]");
	}

	if (report->format == 1 && report->len >= BLE_NKRO_REPORT_SIZE)
	{
		LOG_HEXDUMP_DBG(report->data, BLE_NKRO_REPORT_SIZE, "BLE TX kbd[NKRO 32B]");
		return notify_report(&ble_hids_svc.attrs[HIDS_ATTR_REPORT_NKRO],
							 report->data, BLE_NKRO_REPORT_SIZE, "kbd[NKRO 32B]");
	}

	LOG_HEXDUMP_DBG(report->data, BLE_6KRO_REPORT_SIZE, "BLE TX kbd[6KRO 8B]");
	return notify_report(&ble_hids_svc.attrs[HIDS_ATTR_REPORT_KBD],
						 report->data, BLE_6KRO_REPORT_SIZE, "kbd[6KRO 8B]");
}

static void ble_tx_work_handler(struct k_work *work)
{
	struct ble_pending_report report;
	int ret;
	bool has_data = false;

	ARG_UNUSED(work);

	if (atomic_get(&s_ble_state) != BLE_STATE_READY || !ble_mode_active)
	{
		return;
	}

	unsigned int key = irq_lock();
	if (ble_retry_pending)
	{
		memcpy(&report, &ble_retry_report, sizeof(report));
		ble_retry_pending = false;
		has_data = true;
	}
	irq_unlock(key);

	if (!has_data)
	{
		if (ring_buf_get(&ble_tx_ring, (uint8_t *)&report,
						 sizeof(report)) == sizeof(report))
		{
			has_data = true;
		}
	}

	if (!has_data)
	{
		return;
	}

	ret = send_pending_report(&report);
	if (ret != 0) {
		LOG_ERR("bt_gatt_notify 失败，错误码: %d", ret);
	}

	if (ret == 0)
	{
		if (!ring_buf_is_empty(&ble_tx_ring))
		{
			k_work_reschedule(&ble_tx_retry_work, K_USEC(800));
		}
	}
	else if (ret == -ENOMEM || ret == -EAGAIN || ret == -EBUSY)
	{
		key = irq_lock();
		if (!ble_retry_pending)
		{
			memcpy(&ble_retry_report, &report, sizeof(report));
			ble_retry_pending = true;
		}
		irq_unlock(key);

		LOG_WRN("蓝牙发送忙，准备重试 err=%d", ret);
		k_work_reschedule(&ble_tx_retry_work, K_MSEC(1));
	}
	else if (ret == -EPERM)
	{
		LOG_WRN("蓝牙未就绪 (EPERM)，降级状态并丢弃报告");
		atomic_set(&s_ble_state, BLE_STATE_ENCRYPTED);
	}
	else
	{
		LOG_ERR("蓝牙发送不可恢复错误 err=%d，丢弃报告", ret);
	}
}

static void queue_report(uint8_t type, uint8_t format,
						 const uint8_t *data, uint8_t len)
{
	struct ble_pending_report report;
	uint32_t wrote;

	if (data == NULL)
	{
		LOG_ERR("蓝牙报告数据为空 type=%u", type);
		return;
	}

	if (type != BLE_REPORT_TYPE_KBD && type != BLE_REPORT_TYPE_CONSUMER)
	{
		LOG_ERR("蓝牙报告类型非法 type=%u", type);
		return;
	}

	if (len > sizeof(report.data))
	{
		LOG_ERR("蓝牙报告过长 len=%u", len);
		return;
	}

	report.type = type;
	report.format = format;
	report.len = len;
	memset(report.data, 0, sizeof(report.data));
	memcpy(report.data, data, len);

	if (ble_retry_pending && ble_retry_report.type == type)
	{
		memcpy(&ble_retry_report, &report, sizeof(ble_retry_report));
		(void)k_work_submit_to_queue(&ble_tx_work_q, &ble_tx_work);
		return;
	}

	if (ring_buf_space_get(&ble_tx_ring) < sizeof(report))
	{
		if (type == BLE_REPORT_TYPE_KBD)
		{
			ring_buf_reset(&ble_tx_ring);
			LOG_WRN("蓝牙发送队列满，保留最新键盘状态");
		}
		else
		{
			LOG_WRN("蓝牙发送队列空间不足，丢弃整条报告 type=%u", type);
			return;
		}
	}

	if (ring_buf_space_get(&ble_tx_ring) < sizeof(report))
	{
		memcpy(&ble_retry_report, &report, sizeof(ble_retry_report));
		ble_retry_pending = true;
		(void)k_work_submit_to_queue(&ble_tx_work_q, &ble_tx_work);
		return;
	}

	wrote = ring_buf_put(&ble_tx_ring, (const uint8_t *)&report,
						 sizeof(report));
	if (wrote < sizeof(report))
	{
		LOG_ERR("蓝牙发送队列写入异常 type=%u wrote=%u need=%u",
				type, wrote, (uint32_t)sizeof(report));
		clear_tx_queue();
		return;
	}

	(void)k_work_submit_to_queue(&ble_tx_work_q, &ble_tx_work);
}

static bool handle_kbd_report(const struct app_event_header *aeh)
{
	const struct hid_kbd_report_event *event = cast_hid_kbd_report_event(aeh);
	uint8_t current_state = atomic_get(&s_ble_state);
	enum hid_drop_reason drop_reason = hid_drop_reason_from_state();

	if (drop_reason != HID_DROP_NONE)
	{
		log_hid_drop(drop_reason);
		return false;
	}

	LOG_DBG("BLE KBD 入队: state=%d fmt=%d len=%u", current_state, event->format, event->len);
	queue_report(BLE_REPORT_TYPE_KBD, event->format,
		     event->raw_report, event->len);
	return false;
}

static bool handle_consumer_report(const struct app_event_header *aeh)
{
	const struct hid_consumer_report_event *event =
		cast_hid_consumer_report_event(aeh);
	uint8_t current_state = atomic_get(&s_ble_state);
	uint8_t wire[BLE_CONSUMER_REPORT_SIZE + 1];
	enum hid_drop_reason drop_reason = hid_drop_reason_from_state();

	if (drop_reason != HID_DROP_NONE)
	{
		log_hid_drop(drop_reason);
		if (event->count == 0)
		{
			if (drop_reason == HID_DROP_NOT_BLE)
			{
				LOG_DBG("HID_DROP reason=%s report=consumer_release",
						hid_drop_reason_str(drop_reason));
			}
			else
			{
				LOG_WRN("HID_DROP reason=%s report=consumer_release",
						hid_drop_reason_str(drop_reason));
			}
		}
		return false;
	}

	wire[0] = BLE_CONSUMER_REPORT_ID;
	for (uint8_t i = 0; i < 3; i++)
	{
		uint16_t usage = (i < event->count) ? event->usages[i] : 0x0000;
		sys_put_le16(usage, &wire[1 + i * 2]);
	}

	LOG_DBG("BLE 消费者入队: state=%d count=%u usages=[%04X %04X %04X]",
		current_state, event->count,
		event->count > 0 ? event->usages[0] : 0x0000,
		event->count > 1 ? event->usages[1] : 0x0000,
		event->count > 2 ? event->usages[2] : 0x0000);
	queue_report(BLE_REPORT_TYPE_CONSUMER, 0, wire, sizeof(wire));
	return false;
}

static bool handle_mode_event(const struct app_event_header *aeh)
{
	const struct mode_event *event = cast_mode_event(aeh);

	if (event->mode == MODE_SWITCH_POS_BLE)
	{
		enter_ble_mode();
	}
	else if (ble_mode_active)
	{
		leave_ble_mode();
	}

	return false;
}

static bool handle_ble_control_event(const struct app_event_header *aeh)
{
	const struct ble_control_event *event = cast_ble_control_event(aeh);

	if (event->cmd == BLE_CONTROL_CMD_CLEAR_BONDS)
	{
		request_user_bond_clear();
	}

	return false;
}

APP_EVENT_LISTENER(ble_tx_kbd, handle_kbd_report);
APP_EVENT_SUBSCRIBE(ble_tx_kbd, hid_kbd_report_event);

APP_EVENT_LISTENER(ble_tx_consumer, handle_consumer_report);
APP_EVENT_SUBSCRIBE(ble_tx_consumer, hid_consumer_report_event);

APP_EVENT_LISTENER(ble_mode_listener, handle_mode_event);
APP_EVENT_SUBSCRIBE(ble_mode_listener, mode_event);

APP_EVENT_LISTENER(ble_control_listener, handle_ble_control_event);
APP_EVENT_SUBSCRIBE(ble_control_listener, ble_control_event);

int ble_transport_init(void)
{
	enum mode_switch_position pos = MODE_SWITCH_POS_UNKNOWN;
	int ret;

	k_work_queue_start(&ble_tx_work_q,
					   ble_tx_work_q_stack,
					   K_THREAD_STACK_SIZEOF(ble_tx_work_q_stack),
					   BLE_TX_WORK_Q_PRIO,
					   NULL);
	k_work_init(&ble_tx_work, ble_tx_work_handler);
	k_work_init_delayable(&ble_tx_retry_work, ble_tx_retry_work_handler);
	k_work_init_delayable(&adv_restart_work, adv_restart_work_handler);
	k_work_init_delayable(&ble_keep_alive_work, ble_keep_alive_work_handler);
	k_work_init_delayable(&pairing_timeout_work, pairing_timeout_work_handler);
	k_work_init_delayable(&bond_cleanup_work, bond_cleanup_work_handler);
	k_work_init_delayable(&user_clear_finalize_work, user_clear_finalize_work_handler);
	k_work_init_delayable(&security_request_work, security_request_work_handler);

	reset_runtime_flags();

	ret = bt_conn_auth_info_cb_register(&auth_info_callbacks);
	if (ret < 0)
	{
		LOG_ERR("蓝牙配对回调注册失败 err=%d", ret);
		return ret;
	}

	ret = bt_conn_auth_cb_register(&auth_cb);
	LOG_INF("AUTH_CB_REGISTER err=%d", ret);
	if (ret < 0)
	{
		return ret;
	}

	ret = bt_enable(NULL);
	if (ret < 0)
	{
		LOG_ERR("蓝牙初始化失败 err=%d", ret);
		return ret;
	}

	ble_ready = true;

	ret = settings_load();
	if (ret < 0)
	{
		LOG_ERR("蓝牙配置加载失败 err=%d", ret);
		return ret;
	}

	select_preferred_identity();
	ret = create_pairing_identity_if_needed();
	if (ret < 0)
	{
		return ret;
	}

	refresh_bond_state();
	LOG_INF("蓝牙就绪 id=%u bond=%d HIDS=1",
			active_identity, bonded ? 1 : 0);

	ret = mode_switch_get_position(&pos);
	if (ret == 0 && pos == MODE_SWITCH_POS_BLE)
	{
		enter_ble_mode();
	}
	else
	{
		publish_ble_state(BLE_TRANSPORT_STATE_OFF, ret < 0 ? ret : 0);
	}

	return 0;
}
