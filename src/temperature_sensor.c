#define LOG_MODULE_NAME temperature_sensor
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, LOG_LEVEL_INF);

#include <errno.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/net/lwm2m.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include "send_scheduler.h"
#include "temperature_sensor.h"

#define TEMPERATURE_OBJECT_ID 3303
#define TEMPERATURE_INSTANCE_ID 0
#define TEMPERATURE_RES_VALUE 5700
#define TEMPERATURE_RES_UNITS 5701

#define TEMPERATURE_PERIOD K_SECONDS(2)
#define TEMPERATURE_BASE 20.0
#define TEMPERATURE_VARIATION 5.0

static struct k_work_delayable temperature_work;
static double temperature_value;
static struct lwm2m_time_series_elem temperature_cache[10];
static const struct lwm2m_obj_path temperature_path =
	LWM2M_OBJ(TEMPERATURE_OBJECT_ID, TEMPERATURE_INSTANCE_ID,
		  TEMPERATURE_RES_VALUE);

static double temperature_sensor_generate(void)
{
	uint32_t rand = sys_rand32_get();
	double scale = (double)rand / (double)UINT32_MAX;

	return TEMPERATURE_BASE + (scale * TEMPERATURE_VARIATION);
}

static void temperature_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	temperature_value = temperature_sensor_generate();
	(void)lwm2m_set_f64(&LWM2M_OBJ(TEMPERATURE_OBJECT_ID, TEMPERATURE_INSTANCE_ID,
				       TEMPERATURE_RES_VALUE),
			    temperature_value);

	k_work_schedule(&temperature_work, TEMPERATURE_PERIOD);
}

int temperature_sensor_init(void)
{
	int ret;
	static char units[] = "C";

	ret = lwm2m_create_object_inst(&LWM2M_OBJ(TEMPERATURE_OBJECT_ID,
						  TEMPERATURE_INSTANCE_ID));
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to create temperature object instance (%d)", ret);
		return ret;
	}

	(void)lwm2m_set_string(&LWM2M_OBJ(TEMPERATURE_OBJECT_ID,
					  TEMPERATURE_INSTANCE_ID,
					  TEMPERATURE_RES_UNITS),
			       units);

	ret = lwm2m_enable_cache(&temperature_path, temperature_cache,
				 ARRAY_SIZE(temperature_cache));
	if (ret < 0 && ret != -ENODATA) {
		LOG_WRN("Failed to enable temperature cache (%d)", ret);
	}

	ret = lwm2m_set_cache_filter(&temperature_path, send_scheduler_cache_filter);
	if (ret < 0) {
		LOG_WRN("Failed to register temperature cache filter (%d)", ret);
	}

	k_work_init_delayable(&temperature_work, temperature_work_handler);
	temperature_value = temperature_sensor_generate();
	(void)lwm2m_set_f64(&LWM2M_OBJ(TEMPERATURE_OBJECT_ID, TEMPERATURE_INSTANCE_ID,
				       TEMPERATURE_RES_VALUE),
			    temperature_value);
	k_work_schedule(&temperature_work, TEMPERATURE_PERIOD);

	return 0;
}
