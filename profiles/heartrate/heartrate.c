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

struct heartrate
{
    struct btd_device *device;
    struct gatt_db *db;
    struct bt_gatt_client *client;
    struct gatt_db_attribute *attr_service;

    uint16_t heart_rate_measurement_handle;
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
    DBG("RECEIVE HEARTRATE data: %d\n", *value);
}

static void hrp_io_value_cb(uint16_t value_handle, const uint8_t *value,
                             uint16_t length, void *user_data)
{
	struct heartrate *p = user_data;

	if (value_handle == p->heart_rate_measurement_handle) {
		parse_heartrate_measurement_value(p, value);
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

	DBG("Battery Level: notification enabled");
}

static void read_initial_heartrate_measurement_cb(bool success,
						uint8_t att_ecode,
						const uint8_t *value,
						uint16_t length,
						void *user_data)
{
	struct heartrate *p = user_data;

	if (!success) {
		DBG("Reading heartrate measurement failed with ATT errror: %u",
								att_ecode);
		return;
	}

	if (!length)
		return;

    parse_heartrate_measurement_value(p, value);

	bt_gatt_client_register_notify(p->client,
		                               p->heart_rate_measurement_handle,
		                               hrp_io_ccc_written_cb,
		                               hrp_io_value_cb,
		                               p,
		                               NULL);
}

static void handle_heartrate_measurement(struct heartrate *p, uint16_t value_handle)
{
	p->heart_rate_measurement_handle = value_handle;

	if (!bt_gatt_client_read_value(p->client, value_handle,
						read_initial_heartrate_measurement_cb, p, NULL))
		DBG("Failed to send request to read heartrate_measurement");
}


static bool uuid_cmp(uint16_t u16, const bt_uuid_t *uuid)
{
	bt_uuid_t lhs;

	bt_uuid16_create(&lhs, u16);

	return bt_uuid_cmp(&lhs, uuid) == 0;
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

	if (uuid_cmp(HRP_MEASUREMENT, &uuid)) {
		handle_heartrate_measurement(p, value_handle);
	} else {
		char uuid_str[MAX_LEN_UUID_STR];

		bt_uuid_to_string(&uuid, uuid_str, sizeof(uuid_str));
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