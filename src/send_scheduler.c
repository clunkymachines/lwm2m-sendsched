#include "send_scheduler.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
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

/* Use a vendor-specific object range for custom objects */
#define SEND_SCHED_CTRL_OBJECT_ID 20000
#define SEND_SCHED_RULES_OBJECT_ID 20001

#define SEND_SCHED_CTRL_RES_PAUSED 0
#define SEND_SCHED_CTRL_RES_MAX_SAMPLES 1
#define SEND_SCHED_CTRL_RES_MAX_AGE 2
#define SEND_SCHED_CTRL_RES_FLUSH 3

#define SEND_SCHED_RULES_RES_PATH 0
#define SEND_SCHED_RULES_RES_RULES 1

/* Provide a small buffer for rule definitions that the engine can mutate */
#define SEND_SCHED_MAX_RULE_STRINGS 4
#define SEND_SCHED_RULE_STRING_SIZE 64
#define SEND_SCHED_RULES_MAX_INSTANCES 4
#define SEND_SCHED_VALUE_STR_SIZE 16

#define SEND_SCHED_CTRL_RES_COUNT 4
#define SEND_SCHED_CTRL_RES_INST_COUNT SEND_SCHED_CTRL_RES_COUNT

#define SEND_SCHED_RULES_RES_COUNT 2
#define SEND_SCHED_RULES_RES_INST_COUNT (1 + SEND_SCHED_MAX_RULE_STRINGS)

struct send_sched_rule_entry {
	char path[SEND_SCHED_RULE_STRING_SIZE];
	char rules[SEND_SCHED_MAX_RULE_STRINGS][SEND_SCHED_RULE_STRING_SIZE];
	struct lwm2m_time_series_elem last_reported;
	struct lwm2m_obj_path cached_path;
	double last_observed;
	bool has_last_reported;
	bool has_last_observed;
	bool has_cached_path;
};

static bool scheduler_paused;
static int32_t scheduler_max_samples;
static int32_t scheduler_max_age;

static struct send_sched_rule_entry rule_entries[SEND_SCHED_RULES_MAX_INSTANCES];

/* Ensure a configured path points at a valid resource */
static int send_sched_validate_path(uint16_t obj_inst_id, uint16_t res_id,
				    uint16_t res_inst_id, uint8_t *data, uint16_t data_len,
				    bool last_block, size_t total_size, size_t offset);
/* Validate rule syntax and prevent conflicts per instance */
static int send_sched_validate_rule(uint16_t obj_inst_id, uint16_t res_id,
				    uint16_t res_inst_id, uint8_t *data, uint16_t data_len,
				    bool last_block, size_t total_size, size_t offset);
static int send_sched_flush_cb(uint16_t obj_inst_id, uint8_t *args, uint16_t args_len);
static int send_sched_collect_paths(struct lwm2m_obj_path *paths, size_t max_paths);
static int send_sched_parse_path(const char *path, struct lwm2m_obj_path *out);
static bool send_sched_paths_equal(const struct lwm2m_obj_path *lhs,
				   const struct lwm2m_obj_path *rhs);
static int send_sched_find_rule_entry(const struct lwm2m_obj_path *path,
				      struct lwm2m_obj_path *parsed_path);
static bool send_sched_rule_parse_double(const char *rule, const char *attr,
					 double *out_value);
static int send_sched_rules_delete(uint16_t obj_inst_id);
static void send_sched_format_value(double value, char *buf, size_t buf_len);
static void send_sched_log_decision(const char *verb, const char *path_str,
				    double sample, const char *reason);

/* Compare two LwM2M paths for equality */
static bool send_sched_paths_equal(const struct lwm2m_obj_path *lhs,
				   const struct lwm2m_obj_path *rhs)
{
	return lhs->obj_id == rhs->obj_id &&
	       lhs->obj_inst_id == rhs->obj_inst_id &&
	       lhs->res_id == rhs->res_id &&
	       lhs->res_inst_id == rhs->res_inst_id &&
	       lhs->level == rhs->level;
}

/* Parse a textual object path into a LwM2M obj path structure */
static int send_sched_parse_path(const char *path, struct lwm2m_obj_path *out)
{
	const char *cursor = path;
	unsigned long segments[3];
	char *end = NULL;

	if (!path || path[0] != '/') {
		return -EINVAL;
	}

	cursor++;

	for (int idx = 0; idx < ARRAY_SIZE(segments); idx++) {
		unsigned long value;

		if (*cursor == '\0') {
			return -EINVAL;
		}

		errno = 0;
		value = strtoul(cursor, &end, 10);
		if (errno == ERANGE || value > UINT16_MAX) {
			return -ERANGE;
		}

		if (end == cursor) {
			return -EINVAL;
		}

		segments[idx] = value;

		if (idx < (ARRAY_SIZE(segments) - 1)) {
			if (*end != '/') {
				return -EINVAL;
			}

			cursor = end + 1;
		} else if (*end != '\0') {
			return -EINVAL;
		}
	}

	*out = LWM2M_OBJ((uint16_t)segments[0], (uint16_t)segments[1],
			 (uint16_t)segments[2]);

	return 0;
}

/* Gather unique rule paths into the provided array */
static int send_sched_collect_paths(struct lwm2m_obj_path *paths, size_t max_paths)
{
	int count = 0;

	if (!paths || max_paths == 0U) {
		return 0;
	}

	for (int idx = 0; idx < SEND_SCHED_RULES_MAX_INSTANCES; idx++) {
		struct lwm2m_obj_path candidate;
		bool duplicate = false;
		int ret;

		if (count >= (int)max_paths) {
			LOG_WRN("Flush path list full (%zu entries)", max_paths);
			break;
		}

		if (rule_entries[idx].path[0] == '\0') {
			continue;
		}

		ret = send_sched_parse_path(rule_entries[idx].path, &candidate);
		if (ret < 0) {
			LOG_WRN("Skipping invalid rule path '%s' (%d)",
				rule_entries[idx].path, ret);
			continue;
		}

		for (int j = 0; j < count; j++) {
			if (send_sched_paths_equal(&paths[j], &candidate)) {
				duplicate = true;
				break;
			}
		}

		if (duplicate) {
			continue;
		}

		paths[count++] = candidate;
	}

	return count;
}

/* Locate the rule entry matching the given path */
static int send_sched_find_rule_entry(const struct lwm2m_obj_path *path,
				      struct lwm2m_obj_path *parsed_path)
{
	for (int idx = 0; idx < SEND_SCHED_RULES_MAX_INSTANCES; idx++) {
		struct lwm2m_obj_path candidate;
		int ret;

		if (rule_entries[idx].path[0] == '\0') {
			continue;
		}

		ret = send_sched_parse_path(rule_entries[idx].path, &candidate);
		if (ret < 0) {
			continue;
		}

		if (send_sched_paths_equal(path, &candidate)) {
			if (parsed_path) {
				*parsed_path = candidate;
			}
			return idx;
		}
	}

	return -ENOENT;
}

/* Extract a floating-point value from a rule string */
static bool send_sched_rule_parse_double(const char *rule, const char *attr,
					 double *out_value)
{
	size_t attr_len;
	char *end = NULL;
	double value;

	if (!rule || !attr || !out_value) {
		return false;
	}

	attr_len = strlen(attr);
	if (strncmp(rule, attr, attr_len) != 0) {
		return false;
	}

	if (rule[attr_len] != '=') {
		return false;
	}

	errno = 0;
	value = strtod(&rule[attr_len + 1], &end);
	if (errno == ERANGE) {
		return false;
	}

	if (end == &rule[attr_len + 1] || (end && *end != '\0')) {
		return false;
	}

	*out_value = value;

	return true;
}

/* Trigger a composite SEND for cached resources */
static int send_sched_flush_cb(uint16_t obj_inst_id, uint8_t *args, uint16_t args_len)
{
	struct lwm2m_ctx *ctx;
	struct lwm2m_obj_path path_list[SEND_SCHED_RULES_MAX_INSTANCES];
	int path_count;
	int ret;

	ARG_UNUSED(obj_inst_id);
	ARG_UNUSED(args);
	ARG_UNUSED(args_len);

	ctx = lwm2m_rd_client_ctx();
	if (!ctx) {
		LOG_WRN("Cannot flush caches: LwM2M context unavailable");
		return -ENODEV;
	}

	path_count = send_sched_collect_paths(path_list, ARRAY_SIZE(path_list));
	if (path_count <= 0) {
		LOG_WRN("No cached resources registered for flush");
		return -ENOENT;
	}

	if (path_count > CONFIG_LWM2M_COMPOSITE_PATH_LIST_SIZE) {
		LOG_WRN("Limiting flush to %d path(s)",
			CONFIG_LWM2M_COMPOSITE_PATH_LIST_SIZE);
		path_count = CONFIG_LWM2M_COMPOSITE_PATH_LIST_SIZE;
	}

	ret = lwm2m_send_cb(ctx, path_list, (uint8_t)path_count, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to flush cached data (%d)", ret);
		return ret;
	}

	LOG_INF("Triggered LwM2M send for %d cached path(s)", path_count);

	return 0;
}

static struct lwm2m_engine_obj send_sched_ctrl_obj;
static struct lwm2m_engine_obj_field send_sched_ctrl_fields[] = {
	OBJ_FIELD(SEND_SCHED_CTRL_RES_PAUSED, RW, BOOL),
	OBJ_FIELD(SEND_SCHED_CTRL_RES_MAX_SAMPLES, RW, S32),
	OBJ_FIELD(SEND_SCHED_CTRL_RES_MAX_AGE, RW, S32),
	OBJ_FIELD_EXECUTE(SEND_SCHED_CTRL_RES_FLUSH),
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
	INIT_OBJ_RES_EXECUTE(SEND_SCHED_CTRL_RES_FLUSH, send_sched_ctrl_res, i,
			     send_sched_flush_cb);

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

/* Find the internal rule slot used by an object instance */
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

/* Check whether the attribute expects an integer value */
static bool send_sched_attribute_requires_integer(const char *attr)
{
	return (!strcmp(attr, "pmin") || !strcmp(attr, "pmax") ||
		!strcmp(attr, "epmin") || !strcmp(attr, "epmax"));
}

/* Check whether the attribute expects a floating-point value */
static bool send_sched_attribute_requires_float(const char *attr)
{
	return (!strcmp(attr, "gt") || !strcmp(attr, "lt") || !strcmp(attr, "st"));
}

/* Validate that the string can be parsed as a decimal integer */
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

/* Validate that the string can be parsed as a floating-point number */
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

/* Determine whether the attribute is supported by the scheduler */
static bool send_sched_attribute_is_allowed(const char *attr)
{
	return send_sched_attribute_requires_integer(attr) ||
	       send_sched_attribute_requires_float(attr);
}

/* Ensure the configured path string references a resource */
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

/* Check rule syntax and enforce per-instance attribute uniqueness */
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
	struct send_sched_rule_entry *entry;

	ARG_UNUSED(res_id);
	ARG_UNUSED(last_block);
	ARG_UNUSED(total_size);
	ARG_UNUSED(offset);

	entry_idx = send_sched_rules_index_for_inst(obj_inst_id);
	if (entry_idx < 0) {
		LOG_ERR("Sampling rule instance %u not found", obj_inst_id);
		return -ENOENT;
	}

	entry = &rule_entries[entry_idx];

	if (res_inst_id < SEND_SCHED_MAX_RULE_STRINGS) {
		current_slot = res_inst_id;
	}

	if (current_slot < 0) {
		for (int idx = 0; idx < SEND_SCHED_MAX_RULE_STRINGS; idx++) {
			if (&entry->rules[idx][0] == (char *)data) {
				current_slot = idx;
				break;
			}
		}
	}

	if (current_slot < 0 || current_slot >= SEND_SCHED_MAX_RULE_STRINGS) {
		LOG_ERR("Sampling rule index out of range (%d)", current_slot);
		return -EINVAL;
	}

	if (!data) {
		return -EINVAL;
	}

	if (data_len == 0U) {
		entry->rules[current_slot][0] = '\0';
		send_sched_rules_res_inst[entry_idx][current_slot].data_len = 0U;
		entry->has_last_reported = false;
		entry->has_last_observed = false;
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

		existing_eq = strchr(entry->rules[idx], '=');
		if (!existing_eq) {
			continue;
		}

		if ((size_t)(existing_eq - entry->rules[idx]) == attr_len &&
		    strncmp(entry->rules[idx], attr, attr_len) == 0) {
			LOG_WRN("Sampling rule attribute '%s' already defined", attr);
			return -EEXIST;
		}
	}

	return 0;
}

/* Create a new rules object instance and wire resources */
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

/* Reset rule bookkeeping when the instance is deleted */
static int send_sched_rules_delete(uint16_t obj_inst_id)
{
	int idx = send_sched_rules_index_for_inst(obj_inst_id);

	if (idx < 0) {
		return -ENOENT;
	}

	memset(&rule_entries[idx], 0, sizeof(rule_entries[idx]));
	memset(&send_sched_rules_res[idx], 0, sizeof(send_sched_rules_res[idx]));
	memset(&send_sched_rules_inst[idx], 0, sizeof(send_sched_rules_inst[idx]));
	init_res_instance(send_sched_rules_res_inst[idx],
			  ARRAY_SIZE(send_sched_rules_res_inst[idx]));

	return 0;
}

/* Render a floating-point sample as "x.xxx" without relying on float printf */
static void send_sched_format_value(double value, char *buf, size_t buf_len)
{
	int32_t whole = (int32_t)value;
	double remainder = value - (double)whole;
	int32_t frac;

	if (remainder < 0.0) {
		remainder = -remainder;
	}

	frac = (int32_t)((remainder * 1000.0) + 0.5);

	if (frac >= 1000) {
		frac -= 1000;
		if (value >= 0.0) {
			whole += 1;
		} else {
			whole -= 1;
		}
	}

	if (whole == 0 && value < 0.0) {
		snprintk(buf, buf_len, "-0.%03d", frac);
	} else {
		snprintk(buf, buf_len, "%d.%03d", whole, frac);
	}
}

/* Emit a standardized log line describing the cache decision */
static void send_sched_log_decision(const char *verb, const char *path_str,
				    double sample, const char *reason)
{
	char sample_buf[SEND_SCHED_VALUE_STR_SIZE];

	send_sched_format_value(sample, sample_buf, sizeof(sample_buf));
	LOG_INF("%s %s sample %s: %s", verb, path_str, sample_buf, reason);
}

/* Decide whether a sample should be cached for the configured path */
bool send_scheduler_cache_filter(const struct lwm2m_obj_path *path,
				 const struct lwm2m_time_series_elem *element)
{
	int entry_idx;
	struct lwm2m_obj_path entry_path;
	struct send_sched_rule_entry *entry;
	bool has_gt = false;
	bool has_lt = false;
	bool has_st = false;
	double gt_value = 0.0;
	double lt_value = 0.0;
	double st_value = 0.0;
	double sample_value;
	bool trigger = false;
	char path_buf[LWM2M_MAX_PATH_STR_SIZE];
	struct lwm2m_obj_path path_copy;
	const char *path_str = "unknown";
	char keep_reason[96] = {0};
	char drop_reason[96] = {0};
	bool drop_reason_set = false;
	char sample_str[SEND_SCHED_VALUE_STR_SIZE];
	char prev_str[SEND_SCHED_VALUE_STR_SIZE];
	char rule_str[SEND_SCHED_VALUE_STR_SIZE];

	if (!path || !element) {
		return true;
	}

	path_copy = *path;
	path_str = lwm2m_path_log_buf(path_buf, &path_copy);
	sample_value = element->f;

	entry_idx = send_sched_find_rule_entry(path, &entry_path);
	if (entry_idx < 0) {
		send_sched_format_value(sample_value, sample_str, sizeof(sample_str));
		send_sched_log_decision("Drop", path_str, sample_value,
					"no rule entry");
		return false;
	}

	entry = &rule_entries[entry_idx];

	if (!entry->has_cached_path ||
	    !send_sched_paths_equal(&entry->cached_path, &entry_path)) {
		entry->cached_path = entry_path;
		entry->has_cached_path = true;
		entry->has_last_reported = false;
		entry->has_last_observed = false;
	}

	for (int idx = 0; idx < SEND_SCHED_MAX_RULE_STRINGS; idx++) {
		const char *rule = entry->rules[idx];

		if (rule[0] == '\0') {
			continue;
		}

		if (!has_gt && send_sched_rule_parse_double(rule, "gt", &gt_value)) {
			has_gt = true;
			continue;
		}

		if (!has_lt && send_sched_rule_parse_double(rule, "lt", &lt_value)) {
			has_lt = true;
			continue;
		}

		if (!has_st && send_sched_rule_parse_double(rule, "st", &st_value)) {
			has_st = true;
			continue;
		}
	}

	if (!has_gt && !has_lt && !has_st) {
		send_sched_log_decision("Keep", path_str, sample_value,
					"no threshold rules configured");
		return true;
	}

	if (has_gt) {
		double prev = entry->has_last_observed ? entry->last_observed : sample_value;
		send_sched_format_value(prev, prev_str, sizeof(prev_str));
		send_sched_format_value(gt_value, rule_str, sizeof(rule_str));

		if (sample_value > gt_value) {
			if (!entry->has_last_observed || entry->last_observed <= gt_value) {
				trigger = true;
				snprintk(keep_reason, sizeof(keep_reason),
					 "crossed gt %s (prev %s)", rule_str, prev_str);
			} else {
				snprintk(drop_reason, sizeof(drop_reason),
					 "above gt %s but prev %s also above",
					 rule_str, prev_str);
				drop_reason_set = true;
			}
		}
	}

	if (!trigger && has_lt) {
		double prev = entry->has_last_observed ? entry->last_observed : sample_value;
		send_sched_format_value(prev, prev_str, sizeof(prev_str));
		send_sched_format_value(lt_value, rule_str, sizeof(rule_str));

		if (sample_value < lt_value) {
			if (!entry->has_last_observed || entry->last_observed >= lt_value) {
				trigger = true;
				snprintk(keep_reason, sizeof(keep_reason),
					 "crossed lt %s (prev %s)", rule_str, prev_str);
			} else if (!drop_reason_set) {
				snprintk(drop_reason, sizeof(drop_reason),
					 "below lt %s but prev %s also below",
					 rule_str, prev_str);
				drop_reason_set = true;
			}
		}
	}

	if (!trigger && has_st) {
		if (!entry->has_last_reported) {
			trigger = true;
			send_sched_format_value(st_value, rule_str, sizeof(rule_str));
			snprintk(keep_reason, sizeof(keep_reason),
				 "no prior sample, st %s", rule_str);
		} else {
			double delta = sample_value - entry->last_reported.f;

			if (delta < 0.0) {
				delta = -delta;
			}

			if (delta >= st_value) {
				trigger = true;
				send_sched_format_value(delta, sample_str, sizeof(sample_str));
				send_sched_format_value(st_value, rule_str, sizeof(rule_str));
				snprintk(keep_reason, sizeof(keep_reason),
					 "delta %s >= st %s", sample_str, rule_str);
			} else if (!drop_reason_set) {
				send_sched_format_value(delta, sample_str, sizeof(sample_str));
				send_sched_format_value(st_value, rule_str, sizeof(rule_str));
				snprintk(drop_reason, sizeof(drop_reason),
					 "delta %s < st %s", sample_str, rule_str);
				drop_reason_set = true;
			}
		}
	}

	entry->last_observed = sample_value;
	entry->has_last_observed = true;

	if (!trigger) {
		if (!drop_reason_set) {
			snprintk(drop_reason, sizeof(drop_reason), "no rule triggered");
		}
		send_sched_log_decision("Drop", path_str, sample_value, drop_reason);
		return false;
	}

	entry->last_reported = *element;
	entry->has_last_reported = true;

	if (keep_reason[0] == '\0') {
		snprintk(keep_reason, sizeof(keep_reason), "rule triggered");
	}

	send_sched_log_decision("Keep", path_str, sample_value, keep_reason);

	return true;
}

/* Register the scheduler objects and instantiate defaults */
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
		send_sched_rules_obj.delete_cb = send_sched_rules_delete;
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
