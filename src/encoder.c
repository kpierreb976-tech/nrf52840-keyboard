#define DT_DRV_COMPAT nordic_nrf_qdec
#include "encoder.h"

#include <app_event_manager.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device_runtime.h>

#include "events/encoder_event.h"

LOG_MODULE_REGISTER(encoder, LOG_LEVEL_DBG);

#define QDEC_NODE    DT_NODELABEL(qdec)
#define QDEC_STEPS   DT_PROP(QDEC_NODE, steps)
#define QDEC_LED_PRE DT_PROP(QDEC_NODE, led_pre)

/* EC11: 20 物理卡点/圈, 80 steps/圈 → 4 steps 对应 1 个卡点 */
#define EC11_DETENTS_PER_REV 20
#define DETENT_STEPS         (QDEC_STEPS / EC11_DETENTS_PER_REV)

static const struct device *const qdec_dev = DEVICE_DT_GET(QDEC_NODE);
static struct k_work encoder_work;
static struct k_work_delayable self_test_work;

/* 跨触发窗口累积子卡点余数 */
static int32_t step_remainder;
/* 触发计数器，用于诊断 */
static uint32_t trigger_cnt;
/* 自检计数器 */
static uint32_t self_test_cnt;

static void encoder_self_test_handler(struct k_work *work)
{
	struct sensor_value val;
	int ret;

	self_test_cnt++;

	ret = sensor_sample_fetch(qdec_dev);
	if (ret == -EBUSY) {
		LOG_WRN("QDEC 自检 #%u: 设备忙，200ms 后重试", self_test_cnt);
		k_work_schedule(&self_test_work, K_MSEC(200));
		return;
	}
	if (ret < 0) {
		LOG_ERR("QDEC 自检 #%u: sample_fetch 失败: %d", self_test_cnt, ret);
		return;
	}

	ret = sensor_channel_get(qdec_dev, SENSOR_CHAN_ROTATION, &val);
	if (ret < 0) {
		LOG_ERR("QDEC 自检 #%u: channel_get 失败: %d", self_test_cnt, ret);
		return;
	}

	int64_t angle_micro = val.val1 * 1000000LL + val.val2;
	LOG_INF("QDEC 自检 #%u: angle_micro=%lld | "
		"硬件链路正常，但触发机制未生效",
		self_test_cnt, angle_micro, val.val1);
}

static void encoder_work_handler(struct k_work *work)
{
	struct sensor_value val;
	int64_t angle_micro, steps_raw, total_steps, detents;
	int ret;

	ret = sensor_sample_fetch(qdec_dev);
	if (ret < 0) {
		if (ret != -EOVERFLOW) {
			LOG_ERR("QDEC 采样失败: %d", ret);
		}
		return;
	}

	ret = sensor_channel_get(qdec_dev, SENSOR_CHAN_ROTATION, &val);
	if (ret < 0) {
		LOG_ERR("QDEC 通道读取失败: %d", ret);
		return;
	}

	/* angle_micro 是自上次 sample_fetch 以来的增量角度 (Δ 值) */
	angle_micro = val.val1 * 1000000LL + val.val2;

	trigger_cnt++;

	/* 首次触发确认：QDEC 硬件中断链路正常 */
	if (trigger_cnt == 1) {
		LOG_INF("QDEC 首次触发成功，链路正常");
	}

	/* Δ 角度 → 硬件 steps (边沿数) */
	steps_raw = angle_micro * QDEC_STEPS / 360000000LL;

	/* 合并余数 */
	total_steps = steps_raw + step_remainder;
	detents     = total_steps / DETENT_STEPS;
	step_remainder = (int32_t)(total_steps % DETENT_STEPS);

	/* 每 20 次触发输出一次诊断，避免刷屏 */
	if ((trigger_cnt % 20) == 0) {
		LOG_INF("QDEC 诊断 #%u: steps_raw=%lld 余数=%ld",
			trigger_cnt, steps_raw, step_remainder);
	}

	if (detents == 0) {
		return;
	}

	struct encoder_event *event = new_encoder_event();
	if (event == NULL) {
		LOG_ERR("编码器事件内存分配失败");
		return;
	}

	event->dir   = (detents > 0) ? 1 : -1;
	event->steps = (uint8_t)((detents > 0) ? detents : -detents);

	APP_EVENT_SUBMIT(event);
}

static void qdec_trigger_handler(const struct device *dev,
				 const struct sensor_trigger *trig)
{
	k_work_submit(&encoder_work);
}

int encoder_init(void)
{
	if (!device_is_ready(qdec_dev)) {
		LOG_ERR("QDEC 设备未就绪");
		return -ENODEV;
	}

	int ret = pm_device_runtime_get(qdec_dev);
	if (ret < 0) {
		LOG_ERR("QDEC 上电失败: %d", ret);
		return ret;
	}

	k_work_init(&encoder_work, encoder_work_handler);
	k_work_init_delayable(&self_test_work, encoder_self_test_handler);

	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DATA_READY,
		.chan = SENSOR_CHAN_ROTATION,
	};
	ret = sensor_trigger_set(qdec_dev, &trig, qdec_trigger_handler);
	if (ret < 0) {
		LOG_ERR("QDEC 触发器注册失败: %d", ret);
		return ret;
	}

	LOG_INF("QDEC 就绪 — A=P0.10 B=P1.06 steps=%d led-pre=%d 卡点/圈=%d",
		QDEC_STEPS, QDEC_LED_PRE, EC11_DETENTS_PER_REV);

	/* 自检：绕开触发机制，手动采样验证 QDEC 硬件是否在运行 */
	k_work_schedule(&self_test_work, K_MSEC(200));

	return 0;
}
