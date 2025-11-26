#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/lwm2m.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/conn_mgr_connectivity.h>

#include <string.h>

#include "send_scheduler.h"
#include "temperature_sensor.h"
#include "humidity_sensor.h"
LOG_MODULE_REGISTER(lwm2m_base_client, LOG_LEVEL_INF);

#define APP_BANNER "LwM2M base client"

static struct lwm2m_ctx client_ctx;

static const char *const endpoint =
	sizeof(CONFIG_NET_SAMPLE_LWM2M_ID) > 1 ? CONFIG_NET_SAMPLE_LWM2M_ID :
						 CONFIG_BOARD;

#if defined(CONFIG_NET_CONNECTION_MANAGER)
static K_SEM_DEFINE(network_connected_sem, 0, 1);
static struct net_mgmt_event_callback l4_cb;
#endif

static K_SEM_DEFINE(quit_lock, 0, K_SEM_MAX_LIMIT);

static uint8_t bat_idx = LWM2M_DEVICE_PWR_SRC_TYPE_BAT_INT;
static int bat_mv = 3800;
static int bat_ma = 125;
static uint8_t usb_idx = LWM2M_DEVICE_PWR_SRC_TYPE_USB;
static int usb_mv = 5000;
static int usb_ma = 900;
static uint8_t bat_level = 95;
static uint8_t bat_status = LWM2M_DEVICE_BATTERY_STATUS_CHARGING;
static int mem_free = 15;
static int mem_total = 25;

static int device_reboot_cb(uint16_t obj_inst_id, uint8_t *args,
			    uint16_t args_len)
{
	ARG_UNUSED(obj_inst_id);
	ARG_UNUSED(args);
	ARG_UNUSED(args_len);

	LOG_INF("DEVICE: REBOOT");

	return 0;
}

static int lwm2m_setup(void)
{
	static char manufacturer[] = "Zephyr";
	static char model_number[] = "Base LwM2M Client";
	static char serial_number[] = "0001";
	static char firmware_version[] = "1.0";
	static char hardware_version[] = "1.0";

	int ret;

	ret = lwm2m_set_string(&LWM2M_OBJ(0, 0, 0), CONFIG_NET_SAMPLE_LWM2M_SERVER);
	if (ret < 0) {
		return ret;
	}

	lwm2m_set_u8(&LWM2M_OBJ(0, 0, 2), 3);
	lwm2m_set_u16(&LWM2M_OBJ(0, 0, 10), CONFIG_LWM2M_SERVER_DEFAULT_SSID);
	lwm2m_set_u16(&LWM2M_OBJ(1, 0, 0), CONFIG_LWM2M_SERVER_DEFAULT_SSID);

	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 0), manufacturer, sizeof(manufacturer),
			  sizeof(manufacturer), LWM2M_RES_DATA_FLAG_RO);
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 1), model_number, sizeof(model_number),
			  sizeof(model_number), LWM2M_RES_DATA_FLAG_RO);
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 2), serial_number,
			  sizeof(serial_number),
			  sizeof(serial_number), LWM2M_RES_DATA_FLAG_RO);
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 3), firmware_version,
			  sizeof(firmware_version),
			  sizeof(firmware_version), LWM2M_RES_DATA_FLAG_RO);
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 17), CONFIG_BOARD, sizeof(CONFIG_BOARD),
			  sizeof(CONFIG_BOARD), LWM2M_RES_DATA_FLAG_RO);
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 18), hardware_version,
			  sizeof(hardware_version),
			  sizeof(hardware_version), LWM2M_RES_DATA_FLAG_RO);

	lwm2m_register_exec_callback(&LWM2M_OBJ(3, 0, 4), device_reboot_cb);

	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 9), &bat_level, sizeof(bat_level),
			  sizeof(bat_level), 0);
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 10), &mem_free, sizeof(mem_free),
			  sizeof(mem_free), 0);
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 20), &bat_status, sizeof(bat_status),
			  sizeof(bat_status), 0);
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 21), &mem_total, sizeof(mem_total),
			  sizeof(mem_total), 0);

	lwm2m_create_res_inst(&LWM2M_OBJ(3, 0, 6, 0));
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 6, 0), &bat_idx, sizeof(bat_idx),
			  sizeof(bat_idx), 0);
	lwm2m_create_res_inst(&LWM2M_OBJ(3, 0, 7, 0));
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 7, 0), &bat_mv, sizeof(bat_mv),
			  sizeof(bat_mv), 0);
	lwm2m_create_res_inst(&LWM2M_OBJ(3, 0, 8, 0));
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 8, 0), &bat_ma, sizeof(bat_ma),
			  sizeof(bat_ma), 0);

	lwm2m_create_res_inst(&LWM2M_OBJ(3, 0, 6, 1));
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 6, 1), &usb_idx, sizeof(usb_idx),
			  sizeof(usb_idx), 0);
	lwm2m_create_res_inst(&LWM2M_OBJ(3, 0, 7, 1));
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 7, 1), &usb_mv, sizeof(usb_mv),
			  sizeof(usb_mv), 0);
	lwm2m_create_res_inst(&LWM2M_OBJ(3, 0, 8, 1));
	lwm2m_set_res_buf(&LWM2M_OBJ(3, 0, 8, 1), &usb_ma, sizeof(usb_ma),
			  sizeof(usb_ma), 0);

	ret = send_scheduler_init();
	if (ret < 0) {
		return ret;
	}

	ret = temperature_sensor_init();
	if (ret < 0) {
		return ret;
	}

	ret = humidity_sensor_init();
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static void rd_client_event(struct lwm2m_ctx *ctx,
			    enum lwm2m_rd_client_event event)
{
	ARG_UNUSED(ctx);

	switch (event) {
	case LWM2M_RD_CLIENT_EVENT_REGISTRATION_COMPLETE:
		LOG_INF("Registration complete");
		send_sched_handle_registration_event();
		break;
	case LWM2M_RD_CLIENT_EVENT_REG_UPDATE_COMPLETE:
		LOG_INF("Registration update complete");
		send_sched_handle_registration_event();
		break;
	case LWM2M_RD_CLIENT_EVENT_REG_UPDATE:
		LOG_INF("Registration update in progress");
		break;
	case LWM2M_RD_CLIENT_EVENT_DEREGISTER:
		LOG_INF("Client deregistered");
		break;
	case LWM2M_RD_CLIENT_EVENT_NETWORK_ERROR:
		LOG_ERR("Network error reported by LwM2M engine");
		break;
	default:
		break;
	}
}

#if defined(CONFIG_NET_CONNECTION_MANAGER)
static void on_net_event_l4_connected(void)
{
	LOG_INF("Connected to network");
	k_sem_give(&network_connected_sem);
	lwm2m_engine_resume();
}

static void on_net_event_l4_disconnected(void)
{
	LOG_INF("Disconnected from network");
	lwm2m_engine_pause();
}

static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		on_net_event_l4_connected();
		break;
	case NET_EVENT_L4_DISCONNECTED:
		on_net_event_l4_disconnected();
		break;
	default:
		break;
	}
}
#endif /* CONFIG_NET_CONNECTION_MANAGER */

int main(void)
{
	int ret;

	LOG_INF(APP_BANNER);

#if defined(CONFIG_NET_CONNECTION_MANAGER)
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("No network interface found");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler,
				     NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&l4_cb);

	ret = net_if_up(iface);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("net_if_up failed (%d)", ret);
		return ret;
	}

	(void)conn_mgr_if_connect(iface);

	if (net_if_is_up(iface)) {
		on_net_event_l4_connected();
	}

	LOG_INF("Waiting for network connection...");
	k_sem_take(&network_connected_sem, K_FOREVER);
#endif

	ret = lwm2m_setup();
	if (ret < 0) {
		LOG_ERR("Cannot setup LwM2M resources (%d)", ret);
		return ret;
	}

	memset(&client_ctx, 0, sizeof(client_ctx));
	lwm2m_rd_client_start(&client_ctx, endpoint, 0, rd_client_event, NULL);

	k_sem_take(&quit_lock, K_FOREVER);
	return 0;
}
