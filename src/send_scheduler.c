#include "send_scheduler.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
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

static int send_sched_validate_path(uint16_t obj_inst_id, uint16_t res_id,
				    uint16_t res_inst_id, uint8_t *data, uint16_t data_len,
				    bool last_block, size_t total_size, size_t offset);
static int send_sched_validate_rule(uint16_t obj_inst_id, uint16_t res_id,
				    uint16_t res_inst_id, uint8_t *data, uint16_t data_len,
				    bool last_block, size_t total_size, size_t offset);
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
		LOG_WRN("Scheduler control instance %u already exists or not 0", obj_inst_id);
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

static int send_sched_rules_index_for_inst(uint16_t obj_inst_id)
{
	for (int idx = 0; idx < SEND_SCHED_RULES_MAX_INSTANCES; idx++) {
		if (send_sched_rules_inst[idx].obj &&
		    send_sched_rules_inst[idx].obj_inst_id == obj_inst_id) {
			return idx;
		}
	}

	return -1;
}

static bool send_sched_attribute_requires_integer(const char *attr)
{
	return (!strcmp(attr, "pmin") || !strcmp(attr, "pmax") ||
		!strcmp(attr, "epmin") || !strcmp(attr, "epmax"));
}

static bool send_sched_attribute_requires_float(const char *attr)
{
	return (!strcmp(attr, "gt") || !strcmp(attr, "lt") || !strcmp(attr, "st"));
}

static bool send_sched_is_valid_integer(const char *value)
{
	char *end = NULL;
	long parsed;

	if (value == NULL || *value == '\0') {
		return false;
	}

	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno == ERANGE) {
		return false;
	}

	if (end == value || (end && *end != '\0')) {
		return false;
	}

	ARG_UNUSED(parsed);

	return true;
}

static bool send_sched_is_valid_float(const char *value)
{
	char *end = NULL;
	double parsed;

	if (value == NULL || *value == '\0') {
		return false;
	}

	errno = 0;
	parsed = strtod(value, &end);
	if (errno == ERANGE) {
		return false;
	}

	if (end == value || (end && *end != '\0')) {
		return false;
	}

	ARG_UNUSED(parsed);

	return true;
}

static bool send_sched_attribute_is_allowed(const char *attr)
{
	return send_sched_attribute_requires_integer(attr) ||
	       send_sched_attribute_requires_float(attr);
}

static int send_sched_validate_path(uint16_t obj_inst_id, uint16_t res_id,
				    uint16_t res_inst_id, uint8_t *data, uint16_t data_len,
				    bool last_block, size_t total_size, size_t offset)
{
	char path_buf[SEND_SCHED_RULE_STRING_SIZE];
	size_t copy_len;
	int segments = 0;

	ARG_UNUSED(obj_inst_id);
	ARG_UNUSED(res_id);
	ARG_UNUSED(res_inst_id);
	ARG_UNUSED(last_block);
	ARG_UNUSED(total_size);
	ARG_UNUSED(offset);

	if (!data || data_len == 0) {
		LOG_WRN("Sampling rule path cannot be empty");
		return -EINVAL;
	}

	if (data_len >= sizeof(path_buf)) {
		LOG_WRN("Sampling rule path too long (%u)", data_len);
		return -ENOBUFS;
	}

	copy_len = MIN((size_t)data_len, sizeof(path_buf) - 1U);
	memcpy(path_buf, data, copy_len);
	path_buf[copy_len] = '\0';

	if (path_buf[0] != '/') {
		LOG_WRN("Sampling rule path must start with '/'");
		return -EINVAL;
	}

	for (char *cursor = path_buf + 1; *cursor != '\0';) {
		char *next = strchr(cursor, '/');
		size_t seg_len = next ? (size_t)(next - cursor) : strlen(cursor);

		if (seg_len == 0) {
			LOG_WRN("Sampling rule path contains empty segment");
			return -EINVAL;
		}

		for (size_t idx = 0; idx < seg_len; idx++) {
			if (!isdigit((unsigned char)cursor[idx])) {
				LOG_WRN("Sampling rule path segment must be numeric");
				return -EINVAL;
			}
		}

		segments++;
		if (!next) {
			break;
		}
		cursor = next + 1;
	}

	if (segments != 3) {
		LOG_WRN("Sampling rule path must reference a resource (/obj/inst/res)");
		return -EINVAL;
	}

	return 0;
}

static int send_sched_validate_rule(uint16_t obj_inst_id, uint16_t res_id,
				    uint16_t res_inst_id, uint8_t *data, uint16_t data_len,
				    bool last_block, size_t total_size, size_t offset)
{
	char rule_buf[SEND_SCHED_RULE_STRING_SIZE];
	char *eq = NULL;
	const char *attr;
	const char *value;
	size_t attr_len;
	int entry_idx;
	int current_slot = -1;

	ARG_UNUSED(res_id);
	ARG_UNUSED(last_block);
	ARG_UNUSED(total_size);
	ARG_UNUSED(offset);

	if (!data) {
		return -EINVAL;
	}

	if (data_len == 0U) {
		/* Treat empty payload as clearing the rule instance. */
		return 0;
	}

	if (data_len >= sizeof(rule_buf)) {
		LOG_WRN("Sampling rule string too long (%u)", data_len);
		return -ENOBUFS;
	}

	memcpy(rule_buf, data, data_len);
	rule_buf[data_len] = '\0';

	eq = strchr(rule_buf, '=');
	if (!eq || strchr(eq + 1, '=')) {
		LOG_WRN("Sampling rule must be formatted as attribute=value");
		return -EINVAL;
	}

	*eq = '\0';
	attr = rule_buf;
	value = eq + 1;
	attr_len = strlen(attr);

	if (attr_len == 0U || *value == '\0') {
		LOG_WRN("Sampling rule requires both attribute and value");
		return -EINVAL;
	}

	for (size_t idx = 0; idx < attr_len; idx++) {
		if (!islower((unsigned char)attr[idx])) {
			LOG_WRN("Sampling rule attribute contains invalid characters");
			return -EINVAL;
		}
	}

	if (!send_sched_attribute_is_allowed(attr)) {
		LOG_WRN("Sampling rule attribute '%s' is not supported", attr);
		return -EINVAL;
	}

	if (send_sched_attribute_requires_integer(attr)) {
		if (!send_sched_is_valid_integer(value)) {
			LOG_WRN("Sampling rule attribute '%s' expects integer value", attr);
			return -EINVAL;
		}
	} else if (send_sched_attribute_requires_float(attr)) {
		if (!send_sched_is_valid_float(value)) {
			LOG_WRN("Sampling rule attribute '%s' expects floating-point value", attr);
			return -EINVAL;
		}
	}

	entry_idx = send_sched_rules_index_for_inst(obj_inst_id);
	if (entry_idx < 0) {
		LOG_ERR("Sampling rule instance %u not found", obj_inst_id);
		return -ENOENT;
	}

	if (res_inst_id < SEND_SCHED_MAX_RULE_STRINGS) {
		current_slot = res_inst_id;
	}

	if (current_slot < 0) {
		for (int idx = 0; idx < SEND_SCHED_MAX_RULE_STRINGS; idx++) {
			if (&rule_entries[entry_idx].rules[idx][0] == (char *)data) {
				current_slot = idx;
				break;
			}
		}
	}

	if (current_slot < 0 || current_slot >= SEND_SCHED_MAX_RULE_STRINGS) {
		LOG_ERR("Sampling rule index out of range (%d)", current_slot);
		return -EINVAL;
	}

	for (int idx = 0; idx < SEND_SCHED_MAX_RULE_STRINGS; idx++) {
		const struct lwm2m_engine_res_inst *res_inst;
		const char *existing_eq;

		if (idx == current_slot) {
			continue;
		}

		res_inst = &send_sched_rules_res_inst[entry_idx][idx];
		if (res_inst->res_inst_id == RES_INSTANCE_NOT_CREATED ||
		    res_inst->data_len == 0U) {
			continue;
		}

		existing_eq = strchr(rule_entries[entry_idx].rules[idx], '=');
		if (!existing_eq) {
			continue;
		}

		if ((size_t)(existing_eq - rule_entries[entry_idx].rules[idx]) == attr_len &&
		    strncmp(rule_entries[entry_idx].rules[idx], attr, attr_len) == 0) {
			LOG_WRN("Sampling rule attribute '%s' already defined", attr);
			return -EEXIST;
		}
	}

	return 0;
}

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

		INIT_OBJ_RES_LEN(SEND_SCHED_RULES_RES_PATH,
				 send_sched_rules_res[avail],
				 i,
				 send_sched_rules_res_inst[avail],
				 j,
				 1U,
				 false,
				 true,
				 rule_entries[avail].path,
				 sizeof(rule_entries[avail].path),
				 0,
				 NULL,
				 NULL,
				 send_sched_validate_path,
				 NULL,
				 NULL);

		INIT_OBJ_RES_LEN(SEND_SCHED_RULES_RES_RULES,
				 send_sched_rules_res[avail],
				 i,
				 send_sched_rules_res_inst[avail],
				 j,
				 SEND_SCHED_MAX_RULE_STRINGS,
				 true,
				 false,
				 rule_entries[avail].rules,
				 sizeof(rule_entries[avail].rules[0]),
				 0,
				 NULL,
				 NULL,
				 send_sched_validate_rule,
				 NULL,
				 NULL);

	send_sched_rules_inst[avail].resources = send_sched_rules_res[avail];
	send_sched_rules_inst[avail].resource_count = i;
	send_sched_rules_inst[avail].obj = &send_sched_rules_obj;
	send_sched_rules_inst[avail].obj_inst_id = obj_inst_id;

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
	} else {
		LOG_INF("already registered send scheduler objects");
	}

	ret = lwm2m_create_obj_inst(SEND_SCHED_CTRL_OBJECT_ID, 0, &obj_inst);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to instantiate scheduler control object (%d)", ret);
		return ret;
	}

/*	obj_inst = NULL;
	ret = lwm2m_create_obj_inst(SEND_SCHED_RULES_OBJECT_ID, 0, &obj_inst);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to instantiate sampling rules object (%d)", ret);
		return ret;
	}*/

	return 0;
}
