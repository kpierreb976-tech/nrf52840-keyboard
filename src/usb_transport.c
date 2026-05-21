#define MODULE usb_transport
#include "events/hid_kbd_report_event.h"
#include "events/hid_consumer_report_event.h"
#include "events/set_protocol_event.h"

#include <app_event_manager.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/class/hid.h>

LOG_MODULE_REGISTER(MODULE, LOG_LEVEL_DBG);

/* ── 常量定义 ─────────────────────────────────────────────────── */

#define USB_RING_BUF_SIZE 2048
#define USB_TX_ITEM_SIZE 35
#define USB_TX_TYPE_KBD 0
#define USB_TX_TYPE_CONSUMER 1
#define USB_TX_STACK_SIZE 1024
#define USB_TX_PRIO K_PRIO_COOP(7)

/** 消费者控制报告固定 7 字节（含内嵌 Report ID 2） */
#define CONSUMER_REPORT_SIZE 7
#define CONSUMER_REPORT_ID 2

/** 键盘 6KRO 报告 Report ID */
#define KBD_REPORT_ID 1

/** 键盘 6KRO 报告线缆字节数（含 1 字节 Report ID 前缀） */
#define KBD_WIRE_REPORT_SIZE 9

/** 键盘 NKRO 报告 Report ID */
#define KBD_NKRO_REPORT_ID 3

/** 键盘 NKRO 报告线缆字节数（含 1 字节 Report ID 前缀） */
#define KBD_NKRO_WIRE_REPORT_SIZE 33

/** NKRO 位图字节数 */
#define KBD_NKRO_BITMAP_SIZE 31

/* ── 环形缓冲区条目结构 ──────────────────────────────────────── */

struct usb_tx_item
{
	uint8_t type;
	uint8_t format; /* 0=Boot 8B, 1=NKRO 32B (原始事件格式) */
	uint8_t len;
	uint8_t data[32];
};

/* ── 静态数据结构 ────────────────────────────────────────────── */

RING_BUF_DECLARE(usb_tx_ring, USB_RING_BUF_SIZE);
K_SEM_DEFINE(usb_tx_sem, 0, K_SEM_MAX_LIMIT);

static const struct device *hid_dev;
static struct usbd_context *usbd_ctx;
static bool usb_configured;
static uint8_t current_proto = 1; /* HID_PROTOCOL_REPORT, HID 规范默认 */
static bool nkro_enabled = true;

/* ── USB 设备上下文与描述符定义 ───────────────────────────────── */

USBD_DEVICE_DEFINE(my_usbd,
				   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
				   0x2FE3, 0xeee5);

USBD_DESC_LANG_DEFINE(usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(usb_mfr, "NRF Keyboard");
USBD_DESC_PRODUCT_DEFINE(usb_product, "NRF Keypad");

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(fs_config,
						  USB_SCD_SELF_POWERED,
						  100,
						  &fs_cfg_desc);

/* ── HID 报告描述符（三通道复合：键盘 RID=1 + RID=3 + 消费者 RID=2）─── */
/*
 * 三 Report ID 复合描述符，共享一个 HID 接口：
 *
 *   Report ID 1: Standard Keyboard 6KRO (9B 含 ID，8B 载荷)
 *        修饰键 1B + 保留 1B + 6 键槽。BIOS/Boot 兼容。
 *
 *   Report ID 3: NKRO Keyboard (33B 含 ID，32B 载荷)
 *        修饰键 1B + 全键位图 31B。Report 模式下全键无冲。
 *
 *   Report ID 2: Consumer Control (7B 含 ID，6B 载荷)
 *        前置 Report ID 2，其后为 3 组 16-bit Consumer Usage。
 */
static const uint8_t hid_report_desc[] = {
	/* ------- 键盘通道 (Report ID 1) ------- */
	0x05, 0x01, // USAGE_PAGE (Generic Desktop)
	0x09, 0x06, // USAGE (Keyboard)
	0xA1, 0x01, // COLLECTION (Application)
	0x85, 0x01, //   REPORT_ID (1)

	// 修饰键 (Shift, Ctrl, Alt 等, 1字节)
	0x05, 0x07, //   USAGE_PAGE (Keyboard)
	0x19, 0xE0, //   USAGE_MINIMUM (Keyboard LeftControl)
	0x29, 0xE7, //   USAGE_MAXIMUM (Keyboard RightGUI)
	0x15, 0x00, //   LOGICAL_MINIMUM (0)
	0x25, 0x01, //   LOGICAL_MAXIMUM (1)
	0x75, 0x01, //   REPORT_SIZE (1)
	0x95, 0x08, //   REPORT_COUNT (8)
	0x81, 0x02, //   INPUT (Data,Var,Abs)

	// 保留字节 (1字节)
	0x95, 0x01, //   REPORT_COUNT (1)
	0x75, 0x08, //   REPORT_SIZE (8)
	0x81, 0x03, //   INPUT (Cnst,Var,Abs)

	// LED 输出状态 (5位LED + 3位对齐 = 1字节)
	0x95, 0x05, //   REPORT_COUNT (5)
	0x75, 0x01, //   REPORT_SIZE (1)
	0x05, 0x08, //   USAGE_PAGE (LEDs)
	0x19, 0x01, //   USAGE_MINIMUM (Num Lock)
	0x29, 0x05, //   USAGE_MAXIMUM (Kana)
	0x91, 0x02, //   OUTPUT (Data,Var,Abs)
	0x95, 0x01, //   REPORT_COUNT (1)
	0x75, 0x03, //   REPORT_SIZE (3)
	0x91, 0x03, //   OUTPUT (Cnst,Var,Abs)

	// 标准普通键码 (6字节, 支持 6KRO)
	0x05, 0x07, //   USAGE_PAGE (Keyboard)
	0x15, 0x00, //   LOGICAL_MINIMUM (0)
	// 【核心修复 1】把逻辑最大值从 255 (0xFF) 严格限制为 101 (0x65)
	0x25, 0x65, //   LOGICAL_MAXIMUM (101)
	0x19, 0x00, //   USAGE_MINIMUM (Reserved)
	// 【核心修复 2】把用途最大值也同步限制到 101 (0x65)
	0x29, 0x65, //   USAGE_MAXIMUM (Keyboard Application)
	0x75, 0x08, //   REPORT_SIZE (8)
	0x95, 0x06, //   REPORT_COUNT (6)
	0x81, 0x00, //   INPUT (Data,Ary,Abs) -- 正统的 Array 阵列格式
	0xC0,		// END_COLLECTION

	/* ------- 键盘 NKRO 全键无冲通道 (Report ID 3) ------- */
	0x05, 0x01, // USAGE_PAGE (Generic Desktop)
	0x09, 0x06, // USAGE (Keyboard)
	0xA1, 0x01, // COLLECTION (Application)
	0x85, 0x03, //   REPORT_ID (3)

	// 修饰键 (1 字节, 8 个 bit)
	0x05, 0x07, //   USAGE_PAGE (Keyboard)
	0x19, 0xE0, //   USAGE_MINIMUM (Keyboard LeftControl)
	0x29, 0xE7, //   USAGE_MAXIMUM (Keyboard RightGUI)
	0x15, 0x00, //   LOGICAL_MINIMUM (0)
	0x25, 0x01, //   LOGICAL_MAXIMUM (1)
	0x75, 0x01, //   REPORT_SIZE (1)
	0x95, 0x08, //   REPORT_COUNT (8)
	0x81, 0x02, //   INPUT (Data,Var,Abs)

	// 全键位图 (31 字节 × 8bit = 248bit, 覆盖 Usage 0x00~0xF7)
	0x05, 0x07, //   USAGE_PAGE (Keyboard)
	0x19, 0x00, //   USAGE_MINIMUM (0)
	0x29, 0xF7, //   USAGE_MAXIMUM (247)
	0x15, 0x00, //   LOGICAL_MINIMUM (0)
	0x25, 0x01, //   LOGICAL_MAXIMUM (1)
	0x75, 0x01, //   REPORT_SIZE (1)
	0x95, 0xF8, //   REPORT_COUNT (248)
	0x81, 0x02, //   INPUT (Data,Var,Abs)
	0xC0,       // END_COLLECTION

	/* ------- 消费者多媒体/旋钮通道 (Report ID 2) ------- */
	0x05, 0x0C, // USAGE_PAGE (Consumer Devices)
	0x09, 0x01, // USAGE (Consumer Control)
	0xA1, 0x01, // COLLECTION (Application)
	0x85, 0x02, //   REPORT_ID (2)

	// 支持 3 组同时按下的 16位 消费者键码 (共 6 字节)
	0x15, 0x00,		  //   LOGICAL_MINIMUM (0)
	0x26, 0x9C, 0x02, //   LOGICAL_MAXIMUM (668)
	0x19, 0x00,		  //   USAGE_MINIMUM (Unassigned)
	0x2A, 0x9C, 0x02, //   USAGE_MAXIMUM (Consumer Control)
	0x95, 0x03,		  //   REPORT_COUNT (3)
	0x75, 0x10,		  //   REPORT_SIZE (16)
	0x81, 0x00,		  //   INPUT (Data,Ary,Abs)
	0xC0			  // END_COLLECTION
};

/* ── HID 设备操作回调 ────────────────────────────────────────── */

static void hid_iface_ready_cb(const struct device *dev, const bool ready)
{
	LOG_INF("HID 接口 %s", ready ? "就绪" : "断开");
	usb_configured = ready;
}

static void hid_set_protocol_cb(const struct device *dev, const uint8_t proto)
{
	struct set_protocol_event *event = new_set_protocol_event();

	if (proto == 0)
	{
		current_proto = 0; /* HID_PROTOCOL_BOOT */
		nkro_enabled = false;
		LOG_INF("主机请求协议切换: Boot → 降级标准 8B (无 RID)");
	}
	else
	{
		current_proto = 1; /* HID_PROTOCOL_REPORT */
		nkro_enabled = true;
		LOG_INF("主机请求协议切换: Report → NKRO 通道已启用 (33B)");
	}

	if (event == NULL)
	{
		LOG_ERR("set_protocol_event 内存分配失败");
		return;
	}

	event->protocol = proto;
	APP_EVENT_SUBMIT(event);
}

static int hid_get_report_cb(const struct device *dev,
							 const uint8_t type, const uint8_t id,
							 const uint16_t len, uint8_t *const buf)
{
	/* 暂不实现 Get Report，返回 0 表示无数据 */
	return 0;
}

static int hid_set_report_cb(const struct device *dev,
							 const uint8_t type, const uint8_t id,
							 const uint16_t len, const uint8_t *const buf)
{
	/* LED 输出报告由 Report ID 1 承载（1 字节） */
	if (type == HID_REPORT_TYPE_OUTPUT && id == KBD_REPORT_ID && len >= 1)
	{
		LOG_DBG("LED 状态: NumLock=%d CapsLock=%d ScrollLock=%d",
				(buf[0] & BIT(0)) ? 1 : 0,
				(buf[0] & BIT(1)) ? 1 : 0,
				(buf[0] & BIT(2)) ? 1 : 0);
		return 0;
	}

	return -ENOTSUP;
}

static const struct hid_device_ops hid_ops = {
	.iface_ready = hid_iface_ready_cb,
	.set_protocol = hid_set_protocol_cb,
	.get_report = hid_get_report_cb,
	.set_report = hid_set_report_cb,
};

/* ── USB 状态消息回调 ─────────────────────────────────────────── */

static void usb_status_msg_cb(struct usbd_context *const ctx,
							  const struct usbd_msg *const msg)
{
	switch (msg->type)
	{
	case USBD_MSG_CONFIGURATION:
		if (msg->status > 0)
		{
			usb_configured = true;
			LOG_INF("USB 已配置 (配置值=%d)，传输就绪", msg->status);
		}
		else
		{
			usb_configured = false;
			LOG_INF("USB 取消配置");
		}
		break;
	case USBD_MSG_RESET:
		usb_configured = false;
		LOG_INF("USB 总线复位，传输暂停");
		break;
	case USBD_MSG_SUSPEND:
		usb_configured = false;
		LOG_INF("USB 挂起，传输暂停");
		break;
	case USBD_MSG_RESUME:
		LOG_INF("USB 恢复");
		break;
	case USBD_MSG_VBUS_READY:
		LOG_INF("VBUS 就绪，使能 USB 设备");
		if (usbd_enable(ctx))
		{
			LOG_ERR("VBUS 就绪后使能失败");
		}
		break;
	case USBD_MSG_VBUS_REMOVED:
		LOG_INF("VBUS 移除，禁用 USB 设备");
		usb_configured = false;
		if (usbd_disable(ctx))
		{
			LOG_ERR("VBUS 移除后禁用失败");
		}
		break;
	default:
		break;
	}
}

/* ── 环形缓冲区写入 ──────────────────────────────────────────── */

static int ring_push(uint8_t type, uint8_t format,
					 const uint8_t *data, uint8_t len)
{
	if (len > 32)
	{
		LOG_ERR("报文过长 %u > 32", len);
		return -EINVAL;
	}

	struct usb_tx_item item;

	item.type = type;
	item.format = format;
	item.len = len;
	memcpy(item.data, data, len);

	uint32_t wrote = ring_buf_put(&usb_tx_ring, (uint8_t *)&item,
								  sizeof(item));
	if (wrote < sizeof(item))
	{
		return -ENOSPC;
	}

	k_sem_give(&usb_tx_sem);
	return 0;
}

/* ── 事件监听回调（CAF 上下文，非阻塞）────────────────────────── */

static bool handle_kbd_report(const struct app_event_header *aeh)
{
	LOG_DBG("收到键盘事件，usb_configured=%d", usb_configured);

	if (!usb_configured)
	{
		return false;
	}

	const struct hid_kbd_report_event *event =
		cast_hid_kbd_report_event(aeh);

	/*
	 * 队列解耦：直接存储原始事件数据，不做预格式化。
	 * 格式化动作推迟至 TX 线程发送前，由 current_proto / nkro_enabled 动态决定。
	 */
	uint8_t raw_len = (event->format == 0) ? 8 : 32;

	int ret = ring_push(USB_TX_TYPE_KBD, event->format,
						event->raw_report, raw_len);
	if (ret < 0)
	{
		LOG_WRN("ring_buf 溢出 (kbd)");
	}

	return false;
}

static bool handle_consumer_report(const struct app_event_header *aeh)
{
	LOG_DBG("收到消费者事件，usb_configured=%d", usb_configured);

	if (!usb_configured)
	{
		return false;
	}

	const struct hid_consumer_report_event *event =
		cast_hid_consumer_report_event(aeh);

	/*
	 * 构建 7 字节线缆报文:
	 * [RID=2][usage0_lo][usage0_hi][usage1_lo][usage1_hi][usage2_lo][usage2_hi]
	 * count=0 时 usages 全零，自然产生清理报告。
	 */
	uint8_t wire[CONSUMER_REPORT_SIZE];

	wire[0] = CONSUMER_REPORT_ID;

	for (uint8_t i = 0; i < 3; i++)
	{
		uint16_t usage = (i < event->count) ? event->usages[i] : 0x0000;
		wire[1 + i * 2] = (uint8_t)(usage & 0xFF);
		wire[1 + i * 2 + 1] = (uint8_t)(usage >> 8);
	}

	int ret = ring_push(USB_TX_TYPE_CONSUMER, 0, wire,
						CONSUMER_REPORT_SIZE);
	if (ret < 0)
	{
		LOG_WRN("ring_buf 溢出 (consumer)");
	}

	return false;
}

APP_EVENT_LISTENER(usb_tx_kbd, handle_kbd_report);
APP_EVENT_SUBSCRIBE(usb_tx_kbd, hid_kbd_report_event);

APP_EVENT_LISTENER(usb_tx_consumer, handle_consumer_report);
APP_EVENT_SUBSCRIBE(usb_tx_consumer, hid_consumer_report_event);

/* ── 传输线程 ─────────────────────────────────────────────────── */

static void usb_tx_thread_fn(void *arg1, void *arg2, void *arg3)
{
	struct usb_tx_item item;
	int ret;

	while (1)
	{
		k_sem_take(&usb_tx_sem, K_FOREVER);

		while (ring_buf_get(&usb_tx_ring, (uint8_t *)&item,
							sizeof(item)) == sizeof(item))
		{

			if (!usb_configured)
			{
				continue;
			}

			if (item.type == USB_TX_TYPE_CONSUMER)
			{
				/* 消费者报告：预格式化数据直接发送 */
				uint8_t __aligned(4) aligned_buf[8];
				memcpy(aligned_buf, item.data, item.len);

				ret = hid_device_submit_report(hid_dev, item.len,
											   aligned_buf);
				if (ret < 0)
				{
					LOG_ERR("USB 发送失败: %d, type=consumer len=%u",
							ret, item.len);
				}

				LOG_DBG("TX consumer[%uB]: %02X %02X %02X %02X %02X %02X %02X",
						item.len,
						item.data[0], item.data[1],
						item.data[2], item.data[3],
						item.data[4], item.data[5],
						item.data[6]);
			}
			else /* USB_TX_TYPE_KBD */
			{
				/*
				 * 三路分流矩阵（current_proto 为第一优先级）：
				 *
				 *   current_proto==BOOT  → 无条件 8B 纯数据（无 RID）
				 *   current_proto==REPORT
				 *     ├─ format==NKRO && nkro_enabled → 33B RID=3
				 *     └─ 否则                         → 9B  RID=1
				 */
				uint8_t __aligned(4) aligned_buf[36];
				uint8_t send_len;
				const char *tag;

				if (current_proto == 0)
				{
					/* BIOS/Boot 模式：严格 8 字节，绝不能带 Report ID */
					send_len = 8;
					tag = "kbd[Boot 8B]";
					memcpy(aligned_buf, item.data, 8);
				}
				else if (item.format == 1 && nkro_enabled)
				{
					/* NKRO 全键无冲：RID=3 + 1B 修饰键 + 31B 位图 */
					send_len = KBD_NKRO_WIRE_REPORT_SIZE;
					tag = "kbd[NKRO 33B]";
					aligned_buf[0] = KBD_NKRO_REPORT_ID;
					memcpy(&aligned_buf[1], item.data, 32);
				}
				else
				{
					/* 6KRO：RID=1 + 1B 修饰键 + 1B 保留 + 6B 键码 */
					send_len = KBD_WIRE_REPORT_SIZE;
					tag = "kbd[6KRO 9B]";
					aligned_buf[0] = KBD_REPORT_ID;

					if (item.format == 0)
					{
						memcpy(&aligned_buf[1], item.data, 8);
					}
					else
					{
						/* NKRO→6KRO 转换：从 32B bitmap 提取前 6 个按下的键 */
						aligned_buf[1] = item.data[0];
						aligned_buf[2] = 0x00;
						uint8_t key_idx = 0;
						for (int byt = 0; byt < KBD_NKRO_BITMAP_SIZE && key_idx < 6; byt++)
						{
							uint8_t b = item.data[1 + byt];
							if (b == 0)
								continue;
							for (int bit = 0; bit < 8 && key_idx < 6; bit++)
							{
								if (b & BIT(bit))
								{
									uint16_t usage = (uint16_t)(byt * 8 + bit);
									if (usage >= 0x04)
									{
										aligned_buf[3 + key_idx++] = (uint8_t)usage;
									}
								}
							}
						}
						while (key_idx < 6)
						{
							aligned_buf[3 + key_idx++] = 0x00;
						}
					}
				}

				ret = hid_device_submit_report(hid_dev, send_len,
											   aligned_buf);
				if (ret < 0)
				{
					LOG_ERR("USB 发送失败: %d, %s", ret, tag);
				}

				LOG_DBG("TX %s: %02X %02X %02X %02X %02X %02X %02X %02X",
						tag,
						aligned_buf[0], aligned_buf[1],
						aligned_buf[2], aligned_buf[3],
						aligned_buf[4], aligned_buf[5],
						aligned_buf[6], aligned_buf[7]);
			}
		}
	}
}

K_THREAD_DEFINE(usb_tx_thread, USB_TX_STACK_SIZE,
				usb_tx_thread_fn, NULL, NULL, NULL,
				USB_TX_PRIO, 0, 0);

/* ── USB HID 初始化 ───────────────────────────────────────────── */

static int usb_hid_init(void)
{
	int ret;

	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
	if (!device_is_ready(hid_dev))
	{
		LOG_ERR("HID 设备未就绪");
		return -ENODEV;
	}

	ret = hid_device_register(hid_dev,
							  hid_report_desc, sizeof(hid_report_desc),
							  &hid_ops);
	if (ret < 0)
	{
		LOG_ERR("HID 设备注册失败: %d", ret);
		return ret;
	}

	usbd_ctx = &my_usbd;

	/* 注册字符串描述符 */
	ret = usbd_add_descriptor(usbd_ctx, &usb_lang);
	if (ret < 0)
	{
		LOG_ERR("语言描述符添加失败: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(usbd_ctx, &usb_mfr);
	if (ret < 0)
	{
		LOG_ERR("制造商描述符添加失败: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(usbd_ctx, &usb_product);
	if (ret < 0)
	{
		LOG_ERR("产品描述符添加失败: %d", ret);
		return ret;
	}

	/* 注册 USB 配置并绑定 HID 类实例 */
	ret = usbd_add_configuration(usbd_ctx, USBD_SPEED_FS, &fs_config);
	if (ret < 0)
	{
		LOG_ERR("USB 配置添加失败: %d", ret);
		return ret;
	}

	ret = usbd_register_all_classes(usbd_ctx, USBD_SPEED_FS, 1, NULL);
	if (ret < 0)
	{
		LOG_ERR("USB 类注册失败: %d", ret);
		return ret;
	}

	/* 注册消息回调，跟踪 USB 连接状态 */
	ret = usbd_msg_register_cb(usbd_ctx, usb_status_msg_cb);
	if (ret < 0)
	{
		LOG_ERR("USB 消息回调注册失败: %d", ret);
		return ret;
	}

	ret = usbd_init(usbd_ctx);
	if (ret < 0)
	{
		LOG_ERR("USBD 初始化失败: %d", ret);
		return ret;
	}

	if (!usbd_can_detect_vbus(usbd_ctx))
	{
		ret = usbd_enable(usbd_ctx);
		if (ret < 0)
		{
			LOG_ERR("USBD 使能失败: %d", ret);
			return ret;
		}
	}

	LOG_INF("USB HID 已注册 — 键盘 RID=1(6KRO 9B) + RID=3(NKRO 33B), 消费者 RID=2(7B)");
	return 0;
}

/* ── 模块初始化入口 ───────────────────────────────────────────── */

int usb_transport_init(void)
{
	int ret;

	ret = usb_hid_init();
	if (ret < 0)
	{
		return ret;
	}

	LOG_INF("USB 传输层就绪 — RingBuf=%uB, 线程=COOP(7)",
			USB_RING_BUF_SIZE);
	return 0;
}
