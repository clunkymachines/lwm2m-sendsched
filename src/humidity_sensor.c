#include "humidity_sensor.h"
#include "send_scheduler.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/lwm2m.h>
#include <zephyr/random/random.h>
#include <errno.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#define LOG_MODULE_NAME humidity_sensor
LOG_MODULE_REGISTER(LOG_MODULE_NAME, CONFIG_SEND_SCHED_LOG_LEVEL);

#define HUMIDITY_OBJECT_ID 3304
#define HUMIDITY_INSTANCE_ID 0
#define HUMIDITY_RES_VALUE 5700
#define HUMIDITY_RES_UNITS 5701

#define HUMIDITY_PERIOD K_SECONDS(3)
#define HUMIDITY_BASE 45.0
#define HUMIDITY_VARIATION 20.0

static struct k_work_delayable humidity_work;
static double humidity_value;
static struct lwm2m_time_series_elem humidity_cache[10];
static const struct lwm2m_obj_path humidity_path =
	LWM2M_OBJ(HUMIDITY_OBJECT_ID, HUMIDITY_INSTANCE_ID, HUMIDITY_RES_VALUE);

static double humidity_sensor_generate(void)
{
	uint32_t rand = sys_rand32_get();
	double scale = (double)rand / (double)UINT32_MAX;

	return HUMIDITY_BASE + (scale * HUMIDITY_VARIATION);
}

static void humidity_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	humidity_value = humidity_sensor_generate();
	(void)lwm2m_set_f64(&LWM2M_OBJ(HUMIDITY_OBJECT_ID, HUMIDITY_INSTANCE_ID,
			       HUMIDITY_RES_VALUE),
			    humidity_value);

	k_work_schedule(&humidity_work, HUMIDITY_PERIOD);
}

int humidity_sensor_init(void)
{
	int ret;
	static char units[] = "%";

	ret = lwm2m_create_object_inst(&LWM2M_OBJ(HUMIDITY_OBJECT_ID, HUMIDITY_INSTANCE_ID));
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to create humidity object instance (%d)", ret);
		return ret;
	}

	(void)lwm2m_set_string(&LWM2M_OBJ(HUMIDITY_OBJECT_ID, HUMIDITY_INSTANCE_ID,
				      HUMIDITY_RES_UNITS),
			       units);

	ret = lwm2m_enable_cache(&humidity_path, humidity_cache,
					 ARRAY_SIZE(humidity_cache));
	if (ret < 0 && ret != -ENODATA) {
		LOG_WRN("Failed to enable humidity cache (%d)", ret);
	}

	ret = lwm2m_set_cache_filter(&humidity_path, send_scheduler_cache_filter);
	if (ret < 0) {
		LOG_WRN("Failed to register humidity cache filter (%d)", ret);
	}

	k_work_init_delayable(&humidity_work, humidity_work_handler);
	humidity_value = humidity_sensor_generate();
	(void)lwm2m_set_f64(&LWM2M_OBJ(HUMIDITY_OBJECT_ID, HUMIDITY_INSTANCE_ID,
			       HUMIDITY_RES_VALUE),
			    humidity_value);
	k_work_schedule(&humidity_work, HUMIDITY_PERIOD);

	return 0;
}
