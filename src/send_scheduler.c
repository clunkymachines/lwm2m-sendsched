#include "send_scheduler.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/lwm2m.h>
#include <zephyr/sys/util.h>

#include "lwm2m_object.h"
#include "lwm2m_engine.h"

#define LOG_MODULE_NAME send_scheduler
LOG_MODULE_REGISTER(LOG_MODULE_NAME, LOG_LEVEL_INF);

/* Use a vendor-specific object range for custom objects. */
#define SEND_SCHED_CTRL_OBJECT_ID 20000
#define SEND_SCHED_RULES_OBJECT_ID 20001

#define SEND_SCHED_CTRL_RES_PAUSED 0
#define SEND_SCHED_CTRL_RES_MAX_SAMPLES 1
#define SEND_SCHED_CTRL_RES_MAX_AGE 2

#define SEND_SCHED_RULES_RES_PATH 0
#define SEND_SCHED_RULES_RES_RULES 1

/* Provide a small buffer for rule definitions that the engine can mutate. */
#define SEND_SCHED_MAX_RULE_STRINGS 4
#define SEND_SCHED_RULE_STRING_SIZE 64
#define SEND_SCHED_RULES_MAX_INSTANCES 4

#define SEND_SCHED_CTRL_RES_COUNT 3
#define SEND_SCHED_CTRL_RES_INST_COUNT SEND_SCHED_CTRL_RES_COUNT

#define SEND_SCHED_RULES_RES_COUNT 2
#define SEND_SCHED_RULES_RES_INST_COUNT (1 + SEND_SCHED_MAX_RULE_STRINGS)

struct send_sched_rule_entry {
	char path[SEND_SCHED_RULE_STRING_SIZE];
	char rules[SEND_SCHED_MAX_RULE_STRINGS][SEND_SCHED_RULE_STRING_SIZE];
};

static bool scheduler_paused;
static int32_t scheduler_max_samples;
static int32_t scheduler_max_age;

static struct send_sched_rule_entry rule_entries[SEND_SCHED_RULES_MAX_INSTANCES];

static struct lwm2m_engine_obj send_sched_ctrl_obj;
static struct lwm2m_engine_obj_field send_sched_ctrl_fields[] = {
	OBJ_FIELD(SEND_SCHED_CTRL_RES_PAUSED, RW, BOOL),
	OBJ_FIELD(SEND_SCHED_CTRL_RES_MAX_SAMPLES, RW, S32),
	OBJ_FIELD(SEND_SCHED_CTRL_RES_MAX_AGE, RW, S32),
};
static struct lwm2m_engine_res send_sched_ctrl_res[SEND_SCHED_CTRL_RES_COUNT];
static struct lwm2m_engine_res_inst send_sched_ctrl_res_inst[SEND_SCHED_CTRL_RES_INST_COUNT];
static struct lwm2m_engine_obj_inst send_sched_ctrl_inst;

static struct lwm2m_engine_obj_inst *send_sched_ctrl_create(uint16_t obj_inst_id)
{
	static bool created;
	int i = 0;
	int j = 0;

	if (created || obj_inst_id != 0U) {
		return NULL;
	}

	created = true;

	(void)memset(&send_sched_ctrl_inst, 0, sizeof(send_sched_ctrl_inst));
	init_res_instance(send_sched_ctrl_res_inst, ARRAY_SIZE(send_sched_ctrl_res_inst));
	(void)memset(send_sched_ctrl_res, 0, sizeof(send_sched_ctrl_res));

	INIT_OBJ_RES_DATA(SEND_SCHED_CTRL_RES_PAUSED, send_sched_ctrl_res, i,
			  send_sched_ctrl_res_inst, j,
			  &scheduler_paused, sizeof(scheduler_paused));
	INIT_OBJ_RES_DATA(SEND_SCHED_CTRL_RES_MAX_SAMPLES, send_sched_ctrl_res, i,
			  send_sched_ctrl_res_inst, j,
			  &scheduler_max_samples, sizeof(scheduler_max_samples));
	INIT_OBJ_RES_DATA(SEND_SCHED_CTRL_RES_MAX_AGE, send_sched_ctrl_res, i,
			  send_sched_ctrl_res_inst, j,
			  &scheduler_max_age, sizeof(scheduler_max_age));

	send_sched_ctrl_inst.resources = send_sched_ctrl_res;
	send_sched_ctrl_inst.resource_count = i;

	return &send_sched_ctrl_inst;
}

static struct lwm2m_engine_obj send_sched_rules_obj;
static struct lwm2m_engine_obj_field send_sched_rules_fields[] = {
	OBJ_FIELD(SEND_SCHED_RULES_RES_PATH, RW, STRING),
	OBJ_FIELD(SEND_SCHED_RULES_RES_RULES, RW, STRING),
};
static struct lwm2m_engine_res send_sched_rules_res[SEND_SCHED_RULES_MAX_INSTANCES]
						   [SEND_SCHED_RULES_RES_COUNT];
static struct lwm2m_engine_res_inst send_sched_rules_res_inst[SEND_SCHED_RULES_MAX_INSTANCES]
							     [SEND_SCHED_RULES_RES_INST_COUNT];
static struct lwm2m_engine_obj_inst send_sched_rules_inst[SEND_SCHED_RULES_MAX_INSTANCES];

static struct lwm2m_engine_obj_inst *
send_sched_rules_create(uint16_t obj_inst_id)
{
	int avail = -1;
	int i = 0;
	int j = 0;

	for (int idx = 0; idx < SEND_SCHED_RULES_MAX_INSTANCES; idx++) {
		if (send_sched_rules_inst[idx].obj &&
		    send_sched_rules_inst[idx].obj_inst_id == obj_inst_id) {
			LOG_WRN("Sampling rules instance %u already exists", obj_inst_id);
			return NULL;
		}

		if (avail < 0 && send_sched_rules_inst[idx].obj == NULL) {
			avail = idx;
		}
	}

	if (avail < 0) {
		LOG_WRN("No slot available for sampling rules instance %u", obj_inst_id);
		return NULL;
	}

	(void)memset(&send_sched_rules_res[avail], 0,
		     sizeof(send_sched_rules_res[avail]));
	(void)memset(&rule_entries[avail], 0, sizeof(rule_entries[avail]));
	(void)memset(&send_sched_rules_inst[avail], 0, sizeof(send_sched_rules_inst[avail]));

	init_res_instance(send_sched_rules_res_inst[avail],
			  ARRAY_SIZE(send_sched_rules_res_inst[avail]));

	INIT_OBJ_RES_DATA_LEN(SEND_SCHED_RULES_RES_PATH,
			      send_sched_rules_res[avail],
			      i,
			      send_sched_rules_res_inst[avail],
			      j,
			      rule_entries[avail].path,
			      sizeof(rule_entries[avail].path),
			      0);

	INIT_OBJ_RES_MULTI_DATA_LEN(SEND_SCHED_RULES_RES_RULES,
				    send_sched_rules_res[avail],
				    i,
				    send_sched_rules_res_inst[avail],
				    j,
				    SEND_SCHED_MAX_RULE_STRINGS,
				    false,
				    rule_entries[avail].rules,
				    sizeof(rule_entries[avail].rules[0]),
				    0);

	send_sched_rules_inst[avail].resources = send_sched_rules_res[avail];
	send_sched_rules_inst[avail].resource_count = i;

	return &send_sched_rules_inst[avail];
}

int send_scheduler_init(void)
{
	static bool registered;
	struct lwm2m_engine_obj_inst *obj_inst = NULL;
	int ret;

	if (!registered) {
		send_sched_ctrl_obj.obj_id = SEND_SCHED_CTRL_OBJECT_ID;
		send_sched_ctrl_obj.version_major = 1;
		send_sched_ctrl_obj.version_minor = 0;
		send_sched_ctrl_obj.is_core = false;
		send_sched_ctrl_obj.fields = send_sched_ctrl_fields;
		send_sched_ctrl_obj.field_count = ARRAY_SIZE(send_sched_ctrl_fields);
		send_sched_ctrl_obj.max_instance_count = 1U;
		send_sched_ctrl_obj.create_cb = send_sched_ctrl_create;
		lwm2m_register_obj(&send_sched_ctrl_obj);

		send_sched_rules_obj.obj_id = SEND_SCHED_RULES_OBJECT_ID;
		send_sched_rules_obj.version_major = 1;
		send_sched_rules_obj.version_minor = 0;
		send_sched_rules_obj.is_core = false;
		send_sched_rules_obj.fields = send_sched_rules_fields;
		send_sched_rules_obj.field_count = ARRAY_SIZE(send_sched_rules_fields);
		send_sched_rules_obj.max_instance_count = SEND_SCHED_RULES_MAX_INSTANCES;
		send_sched_rules_obj.create_cb = send_sched_rules_create;
		lwm2m_register_obj(&send_sched_rules_obj);

		registered = true;
	}

	ret = lwm2m_create_obj_inst(SEND_SCHED_CTRL_OBJECT_ID, 0, &obj_inst);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to instantiate scheduler control object (%d)", ret);
		return ret;
	}

	obj_inst = NULL;
	ret = lwm2m_create_obj_inst(SEND_SCHED_RULES_OBJECT_ID, 0, &obj_inst);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to instantiate sampling rules object (%d)", ret);
		return ret;
	}

	return 0;
}
