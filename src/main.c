#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <hal/nrf_gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(doorbell, LOG_LEVEL_INF);

#define BUTTON_1_NODE DT_ALIAS(button1)
#define BUTTON_2_NODE DT_ALIAS(button2)

#define BUTTON_1_ID 1
#define BUTTON_2_ID 2
#define BUTTON_1_PIN 31
#define BUTTON_2_PIN 30

#define CLICK_WINDOW_MS 400
#define BUTTON_DEBOUNCE_MS 25
#define LONG_PRESS_MS 800
#define ADVERTISING_TIME_MS 1500
#define PACKET_ID_KEY_BASE 1

static const struct gpio_dt_spec button_1 = GPIO_DT_SPEC_GET(BUTTON_1_NODE, gpios);
static const struct gpio_dt_spec button_2 = GPIO_DT_SPEC_GET(BUTTON_2_NODE, gpios);
static const struct device *const charger =
	DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_charger));

static uint8_t service_data[9];
static struct bt_data advertising_data[3];
static struct nvs_fs packet_id_storage;
static bool packet_id_storage_ready;
static const uint8_t advertising_flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
static const struct bt_le_adv_param advertising_parameters = {
	.options = BT_LE_ADV_OPT_USE_IDENTITY,
	.interval_min = BT_GAP_ADV_FAST_INT_MIN_1,
	.interval_max = BT_GAP_ADV_FAST_INT_MAX_1,
};

static bool button_is_pressed(const struct gpio_dt_spec *button)
{
	return gpio_pin_get_dt(button) > 0;
}

static const struct gpio_dt_spec *get_button(uint8_t button_id)
{
	return button_id == BUTTON_1_ID ? &button_1 : &button_2;
}

static uint8_t identify_waking_button(uint32_t reset_cause, uint32_t latch)
{
	if (button_is_pressed(&button_1)) {
		return BUTTON_1_ID;
	}
	if (button_is_pressed(&button_2)) {
		return BUTTON_2_ID;
	}

	if ((reset_cause & RESET_LOW_POWER_WAKE) != 0U) {
		bool button_1_latched = (latch & BIT(BUTTON_1_PIN)) != 0U;
		bool button_2_latched = (latch & BIT(BUTTON_2_PIN)) != 0U;

		if (button_1_latched) {
			return BUTTON_1_ID;
		}
		if (button_2_latched) {
			return BUTTON_2_ID;
		}
	}

	return 0;
}

static bool wait_for_button_release(uint32_t press_started_at,
				    const struct gpio_dt_spec *button)
{
	while (true) {
		while (button_is_pressed(button)) {
			k_sleep(K_MSEC(5));
		}

		k_sleep(K_MSEC(BUTTON_DEBOUNCE_MS));
		if (!button_is_pressed(button)) {
			return (uint32_t)(k_uptime_get_32() - press_started_at) >= LONG_PRESS_MS;
		}
	}
}

static uint8_t read_button_event(const struct gpio_dt_spec *button)
{
	uint8_t click_count = 1;
	uint32_t first_press_started_at = k_uptime_get_32();

	if (wait_for_button_release(first_press_started_at, button)) {
		return 0x04;
	}

	while (click_count < 3) {
		uint32_t wait_started_at = k_uptime_get_32();
		bool next_click_detected = false;

		while ((uint32_t)(k_uptime_get_32() - wait_started_at) < CLICK_WINDOW_MS) {
			if (button_is_pressed(button)) {
				k_sleep(K_MSEC(BUTTON_DEBOUNCE_MS));
				if (button_is_pressed(button)) {
					uint32_t press_started_at = k_uptime_get_32();

					if (wait_for_button_release(press_started_at, button)) {
						return 0x04;
					}

					click_count++;
					next_click_detected = true;
					break;
				}
			}
			k_sleep(K_MSEC(5));
		}

		if (!next_click_detected) {
			break;
		}
	}

	return click_count;
}

static int read_battery_percentage(uint8_t *battery_percent)
{
	struct sensor_value voltage;
	int err;
	int64_t millivolts;

	if (!device_is_ready(charger)) {
		return -ENODEV;
	}

	err = sensor_sample_fetch(charger);
	if (err < 0) {
		return err;
	}

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage);
	if (err < 0) {
		return err;
	}

	millivolts = ((int64_t)voltage.val1 * 1000) + (voltage.val2 / 1000);
	if (millivolts >= 4200) {
		*battery_percent = 100;
	} else if (millivolts <= 3300) {
		*battery_percent = 0;
	} else {
		*battery_percent = (uint8_t)(((millivolts - 3300) * 100) / 900);
	}

	return 0;
}

static int next_packet_id(uint8_t button_id, uint8_t *packet_id)
{
	struct flash_pages_info page_info;
	uint8_t next_id;
	ssize_t bytes_read;
	ssize_t bytes_written;
	int err;

	if (!packet_id_storage_ready) {
		packet_id_storage.flash_device = PARTITION_DEVICE(storage_partition);
		if (!device_is_ready(packet_id_storage.flash_device)) {
			return -ENODEV;
		}

		packet_id_storage.offset = PARTITION_OFFSET(storage_partition);
		err = flash_get_page_info_by_offs(packet_id_storage.flash_device,
						  packet_id_storage.offset, &page_info);
		if (err < 0) {
			return err;
		}
		packet_id_storage.sector_size = page_info.size;
		packet_id_storage.sector_count =
			PARTITION_SIZE(storage_partition) / page_info.size;
		if (packet_id_storage.sector_count < 2) {
			return -EINVAL;
		}

		err = nvs_mount(&packet_id_storage);
		if (err < 0) {
			return err;
		}
		packet_id_storage_ready = true;
	}

	uint16_t key = PACKET_ID_KEY_BASE + button_id;
	bytes_read = nvs_read(&packet_id_storage, key, &next_id,
			      sizeof(next_id));
	if (bytes_read == -ENOENT) {
		*packet_id = 0;
		next_id = 1;
	} else if (bytes_read < 0) {
		return (int)bytes_read;
	} else if (bytes_read != sizeof(next_id)) {
		return -EIO;
	} else {
		*packet_id = next_id;
		next_id++;
	}

	bytes_written = nvs_write(&packet_id_storage, key, &next_id,
				  sizeof(next_id));
	if (bytes_written < 0) {
		return (int)bytes_written;
	}
	if (bytes_written != (ssize_t)sizeof(next_id)) {
		return -EIO;
	}

	return 0;
}

static int create_button_identity(uint8_t button_id, uint8_t *identity_id)
{
	uint8_t device_id[8];
	bt_addr_le_t address = {
		.type = BT_ADDR_LE_RANDOM,
	};
	ssize_t device_id_length = hwinfo_get_device_id(device_id, sizeof(device_id));
	if (device_id_length < 0) {
		return (int)device_id_length;
	}
	if (device_id_length != (ssize_t)sizeof(device_id)) {
		return -EIO;
	}

	for (size_t i = 0; i < sizeof(address.a.val); i++) {
		address.a.val[i] = device_id[i] ^ device_id[(i + 2) % sizeof(device_id)] ^
				   device_id[(i + 5) % sizeof(device_id)];
	}

	address.a.val[0] = (address.a.val[0] & 0xfc) | button_id;
	address.a.val[5] = (address.a.val[5] & 0x3f) | 0xc0;

	int id = bt_id_create(&address, NULL);
	if (id < 0) {
		return id;
	}

	char address_string[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(&address, address_string, sizeof(address_string));
	LOG_INF("Button D%u uses BLE identity %s", button_id, address_string);

	*identity_id = (uint8_t)id;
	return 0;
}

static int advertise_button_event(uint8_t button_id, uint8_t button_event,
				  bool has_battery, uint8_t battery_percent,
				  bool has_packet_id, uint8_t packet_id)
{
	static const char button_1_name[] = "Button-D1";
	static const char button_2_name[] = "Button-D2";
	const char *name = button_id == BUTTON_1_ID ? button_1_name : button_2_name;
	struct bt_le_adv_param parameters = advertising_parameters;
	uint8_t identity_id;
	uint8_t service_data_len = 0;
	int err;

	err = bt_enable(NULL);
	if (err < 0 && err != -EALREADY) {
		LOG_ERR("Bluetooth initialization failed (%d)", err);
		return err;
	}

	err = create_button_identity(button_id, &identity_id);
	if (err < 0) {
		LOG_ERR("Creating BLE identity failed (%d)", err);
		return err;
	}
	parameters.id = identity_id;

	service_data[0] = 0xd2;
	service_data[1] = 0xfc;
	service_data[2] = 0x44;
	if (has_packet_id) {
		service_data[3] = 0x00;
		service_data[4] = packet_id;
		service_data_len = 5;
	}
	if (has_battery) {
		service_data[service_data_len++] = 0x01;
		service_data[service_data_len++] = battery_percent;
	} else {
		LOG_WRN("Battery unavailable; omitting BTHome battery object");
	}
	service_data[service_data_len++] = 0x3a;
	service_data[service_data_len++] = button_event;

	advertising_data[0] = (struct bt_data)BT_DATA(BT_DATA_FLAGS, &advertising_flags,
						      sizeof(advertising_flags));
	advertising_data[1] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, service_data,
						      service_data_len);
	advertising_data[2] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name,
						      strlen(name));

	err = bt_le_adv_start(&parameters, advertising_data,
			      ARRAY_SIZE(advertising_data), NULL, 0);
	if (err < 0) {
		LOG_ERR("BLE advertising start failed (%d)", err);
		return err;
	}

	k_sleep(K_MSEC(ADVERTISING_TIME_MS));

	err = bt_le_adv_stop();
	if (err < 0) {
		LOG_ERR("BLE advertising stop failed (%d)", err);
		return err;
	}

	return 0;
}

static void enter_system_off(void)
{
	const uint32_t button_mask = BIT(BUTTON_1_PIN) | BIT(BUTTON_2_PIN);
	int err;

	err = gpio_pin_configure_dt(&button_1, GPIO_INPUT);
	if (err < 0) {
		LOG_ERR("Configuring D1 failed (%d)", err);
	}
	err = gpio_pin_configure_dt(&button_2, GPIO_INPUT);
	if (err < 0) {
		LOG_ERR("Configuring D2 failed (%d)", err);
	}

	NRF_P1->LATCH = button_mask;

	err = gpio_pin_interrupt_configure_dt(&button_1, GPIO_INT_LEVEL_ACTIVE);
	if (err < 0) {
		LOG_ERR("Configuring D1 wake failed (%d)", err);
	}
	err = gpio_pin_interrupt_configure_dt(&button_2, GPIO_INT_LEVEL_ACTIVE);
	if (err < 0) {
		LOG_ERR("Configuring D2 wake failed (%d)", err);
	}

	sys_poweroff();
	CODE_UNREACHABLE;
}

int main(void)
{
	uint32_t reset_cause = 0;
	uint32_t button_latch = NRF_P1->LATCH;
	uint8_t button_id;
	uint8_t button_event;
	uint8_t battery_percent = 0;
	uint8_t packet_id = 0;
	bool has_battery;
	bool has_packet_id;
	int err;

	err = hwinfo_get_reset_cause(&reset_cause);
	if (err < 0) {
		LOG_WRN("Could not read reset cause (%d)", err);
		reset_cause = 0;
	}

	if (!device_is_ready(button_1.port) || !device_is_ready(button_2.port)) {
		LOG_ERR("Button GPIO controller is not ready");
		enter_system_off();
	}

	err = gpio_pin_configure_dt(&button_1, GPIO_INPUT);
	if (err < 0) {
		LOG_ERR("Configuring D1 failed (%d)", err);
		enter_system_off();
	}
	err = gpio_pin_configure_dt(&button_2, GPIO_INPUT);
	if (err < 0) {
		LOG_ERR("Configuring D2 failed (%d)", err);
		enter_system_off();
	}

	button_id = identify_waking_button(reset_cause, button_latch);
	if (button_id == 0) {
		enter_system_off();
	}

	button_event = read_button_event(get_button(button_id));
	err = read_battery_percentage(&battery_percent);
	has_battery = err == 0;
	if (!has_battery) {
		LOG_ERR("Battery reading failed (%d)", err);
	}

	err = next_packet_id(button_id, &packet_id);
	has_packet_id = err == 0;
	if (!has_packet_id) {
		LOG_ERR("BTHome packet ID storage failed (%d); sending without packet ID", err);
	}

	err = advertise_button_event(button_id, button_event, has_battery, battery_percent,
				     has_packet_id, packet_id);
	if (err < 0) {
		LOG_ERR("Button event advertisement failed (%d)", err);
	}

	enter_system_off();
	return 0;
}
