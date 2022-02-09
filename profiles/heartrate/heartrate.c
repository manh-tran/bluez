#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/sdp.h"
#include "lib/uuid.h"

#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-client.h"
#include "src/plugin.h"
#include "src/adapter.h"
#include "src/device.h"
#include "src/profile.h"
#include "src/service.h"
#include "src/log.h"
#include "attrib/att.h"

#define HRP_UUID16 0x180d
#define HRP_MEASUREMENT 0x2a37

struct hrm_flag
{
	unsigned char hr 	: 1;
	unsigned char sc 	: 2;
	unsigned char ee 	: 1;
	unsigned char rr 	: 1;
	unsigned char rffu 	: 3;
} __attribute__((packed));

struct heartrate
{
    struct btd_device *device;
    struct gatt_db *db;
    struct bt_gatt_client *client;
    struct gatt_db_attribute *attr_service;

    uint16_t heart_rate_measurement_handle;
	int hrm_value;
	int hrm_ee_value;

	uint16_t body_sensor_location_handle;
	int bsl_value;
};

static void heartrate_free(struct heartrate *p)
{
    gatt_db_unref(p->db);
    bt_gatt_client_unref(p->client);
	btd_device_unref(p->device);
	g_free(p);
}

static void heartrate_reset(struct heartrate *p)
{
	p->attr_service = NULL;
	gatt_db_unref(p->db);
	p->db = NULL;
	bt_gatt_client_unref(p->client);
	p->client = NULL;
}

static void parse_heartrate_measurement_value(struct heartrate *p, const uint8_t *value)
{
	struct hrm_flag flag = *(struct hrm_flag *) value;
	DBG("HRM-FLAG HR : %d  -  value format is %s", flag.hr, flag.hr == 0 ? "UINT8" : "UINT16");
	switch (flag.sc) {
		case 0: case 1:
			DBG("HRM-FLAG SC : %d  -  sc is not supported", flag.sc);
			break;
		case 2:
			DBG("HRM-FLAG SC : %d  -  sc is supported, but contact is not detected", flag.sc);
			break;
		case 3:
			DBG("HRM-FLAG SC : %d  -  sc is supported and contact is detected", flag.sc);
			break;
	}
	DBG("HRM-FLAG EE : %d  -  ee is %s", flag.ee, flag.ee == 0 ? "not present" : "present");
	DBG("HRM-FLAG RR : %d  -  rr is %s", flag.rr, flag.rr == 0 ? "not present" : "present");
	DBG("HRM-FLAG RFFU : %d", flag.rffu);

	if (flag.hr == 0) {
		p->hrm_value = value[1];
		if (flag.ee) {
			p->hrm_ee_value = value[2];
		}
	} else {
		p->hrm_value = *(const uint16_t *)(value + 1);
		if (flag.ee) {
			p->hrm_ee_value = value[3];
		}
	}
	DBG("HRM VALUE: %d\n", p->hrm_value);
	DBG("HRM EE VALUE: %d\n", p->hrm_ee_value);
}

static void hrp_io_value_cb(uint16_t value_handle, const uint8_t *value,
                             uint16_t length, void *user_data)
{
	struct heartrate *p = user_data;

	if (value_handle == p->heart_rate_measurement_handle) {
		parse_heartrate_measurement_value(p, value);
	} else if (value_handle == p->body_sensor_location_handle) {

	} else {
		g_assert_not_reached();
	}
}

static void hrp_io_ccc_written_cb(uint16_t att_ecode, void *user_data)
{
	struct heartrate *p = user_data;

	if (att_ecode != 0) {
		error("Heartrate Measurement: notifications not enabled %s",
		      att_ecode2str(att_ecode));
		return;
	}

	DBG("Heart Rate: notification enabled");
}

static void handle_heartrate_measurement(struct heartrate *p, uint16_t value_handle)
{
	p->heart_rate_measurement_handle = value_handle;

	bt_gatt_client_register_notify(p->client,
		                               p->heart_rate_measurement_handle,
		                               hrp_io_ccc_written_cb,
		                               hrp_io_value_cb,
		                               p,
		                               NULL);
}

static void read_body_sensor_location_cb(bool success,
						uint8_t att_ecode,
						const uint8_t *value,
						uint16_t length,
						void *user_data)
{
	struct heartrate *p = user_data;

	if (!success) {
		DBG("Reading body sensor location failed with ATT errror: %u",
								att_ecode);
		return;
	}

	if (!length)
		return;

	p->bsl_value = value[0];
	char bsl_name[64];
	switch (value[0]) {
		case 0:
			sprintf(bsl_name, "Other");
			break;
		case 1:
			sprintf(bsl_name, "Chest");
			break;
		case 2:
			sprintf(bsl_name, "Wrist");
			break;
		case 3:
			sprintf(bsl_name, "Finger");
			break;
		case 4:
			sprintf(bsl_name, "Hand");
			break;
		case 5:
			sprintf(bsl_name, "Ear Lobe");
			break;
		case 6:
			sprintf(bsl_name, "Foot");
			break;
		default:
			sprintf(bsl_name, "Unknown");
			break;
	}
	DBG("body sensor location value: %s", bsl_name);

	bt_gatt_client_register_notify(p->client,
		                               p->body_sensor_location_handle,
		                               hrp_io_ccc_written_cb,
		                               hrp_io_value_cb,
		                               p,
		                               NULL);
}

static void handle_body_sensor_location(struct heartrate *p, uint16_t value_handle)
{
	p->body_sensor_location_handle = value_handle;

	if (!bt_gatt_client_read_value(p->client, p->body_sensor_location_handle,
						read_body_sensor_location_cb, p, NULL))
		DBG("Failed to send request to read body sensor location");
}

static void handle_characteristic(struct gatt_db_attribute *attr,
								void *user_data)
{
	struct heartrate *p = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle, NULL,
								NULL, &uuid)) {
		error("Failed to obtain characteristic data");
		return;
	}

	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_to_string(&uuid, uuid_str, sizeof(uuid_str));
	if (strcmp(uuid_str, HEART_RATE_MEASUREMENT_UUID) == 0) {
		handle_heartrate_measurement(p, value_handle);
	} else if (strcmp(uuid_str, BODY_SENSOR_LOCATION_UUID) == 0) {
		handle_body_sensor_location(p, value_handle);
	} else if (strcmp(uuid_str, HEART_RATE_CONTROL_POINT_UUID) == 0) {

	} else {
		DBG("Unsupported characteristic: %s", uuid_str);
	}
}

static void handle_hrp_service(struct heartrate *p)
{
	gatt_db_service_foreach_char(p->attr_service, handle_characteristic, p);
}

static int heartrate_probe(struct btd_service *service)
{
    struct btd_device *device = btd_service_get_device(service);
	struct heartrate *p = btd_service_get_user_data(service);
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("HRP profile probe (%s)", addr);

	/* Ignore, if we were probed for this device already */
	if (p) {
		error("Profile probed twice for the same device!");
		return -1;
	}

	p = g_new0(struct heartrate, 1);
	if (!p)
		return -1;

	p->device = btd_device_ref(device);
	btd_service_set_user_data(service, p);

	return 0;
}

static void heartrate_remove(struct btd_service *service)
{
    struct btd_device *device = btd_service_get_device(service);
	struct heartrate *p;
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("HRP profile remove (%s)", addr);

	p = btd_service_get_user_data(service);
	if (!p) {
		error("HRP service not handled by profile");
		return;
	}

	heartrate_free(p);
}

static void foreach_hrp_service(struct gatt_db_attribute *attr, void *user_data)
{
	struct heartrate *p = user_data;

	if (p->attr_service) {
		error("More than one HRP service exists for this device");
		return;
	}

    p->attr_service = attr;
	handle_hrp_service(p);
}

static int heartrate_accept(struct btd_service *service)
{
    struct btd_device *device = btd_service_get_device(service);
	struct gatt_db *db = btd_device_get_gatt_db(device);
	struct bt_gatt_client *client = btd_device_get_gatt_client(device);
	struct heartrate *p = btd_service_get_user_data(service);
    char addr[18];
	bt_uuid_t heartrate_uuid;

	ba2str(device_get_address(device), addr);
	DBG("HRP profile accept (%s)", addr);

	if (!p) {
		error("HRP service not handled by profile");
		return -1;
	}

    p->db = gatt_db_ref(db);
	p->client = bt_gatt_client_clone(client);

    /* Handle the HRP services */
	bt_uuid16_create(&heartrate_uuid, HRP_UUID16);
	gatt_db_foreach_service(db, &heartrate_uuid, foreach_hrp_service, p);

	if (!p->attr_service) {
		error("HRP attribute not found");
		heartrate_reset(p);
		return -1;
	}

	btd_service_connecting_complete(service, 0);

	return 0;
}

static int heartrate_disconnect(struct btd_service *service)
{
    struct heartrate *p = btd_service_get_user_data(service);
	DBG("HRP profile disconnect");

	heartrate_reset(p);
	btd_service_disconnecting_complete(service, 0);

	return 0;
}

static struct btd_profile heartrate_profile = {
	.name		= "heartrate-profile",
	.remote_uuid	= HEART_RATE_UUID,
	.device_probe	= heartrate_probe,
	.device_remove	= heartrate_remove,
	.accept		= heartrate_accept,
	.disconnect	= heartrate_disconnect,
	.external	= true,
};

static int heartrate_init(void)
{
	return btd_profile_register(&heartrate_profile);
}

static void heartrate_exit(void)
{
	btd_profile_unregister(&heartrate_profile);
}

BLUETOOTH_PLUGIN_DEFINE(heartrate, VERSION, BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
							heartrate_init, heartrate_exit)