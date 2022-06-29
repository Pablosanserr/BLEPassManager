/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Bluetooth Hardware Password Manager
 */
#include "uart_async_adapter.h"
#include "storage_manager.h"

#include <zephyr/types.h>
#include <zephyr.h>
#include <drivers/uart.h>
#include <usb/usb_device.h>

#include <device.h>
#include <soc.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>
#include <bluetooth/hci.h>

#include <bluetooth/services/nus.h>

#include <dk_buttons_and_leds.h>

#include <settings/settings.h>

#include <stdio.h>

#include <logging/log.h>

#include <cJSON.h>
#include <cJSON_os.h>

#define LOG_MODULE_NAME peripheral_uart
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#define STACKSIZE CONFIG_BT_NUS_THREAD_STACK_SIZE
#define PRIORITY 7

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN	(sizeof(DEVICE_NAME) - 1)

#define RUN_STATUS_LED DK_LED1
#define RUN_LED_BLINK_INTERVAL 1000

#define CON_STATUS_LED DK_LED2

#define KEY_PASSKEY_ACCEPT DK_BTN1_MSK
#define KEY_PASSKEY_REJECT DK_BTN2_MSK

#define UART_BUF_SIZE CONFIG_BT_NUS_UART_BUFFER_SIZE
#define UART_WAIT_FOR_BUF_DELAY K_MSEC(50)
#define UART_WAIT_FOR_RX CONFIG_BT_NUS_UART_RX_WAIT_TIME

static K_SEM_DEFINE(ble_init_ok, 0, 1);

static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;

static const struct device *uart;
static struct k_work_delayable uart_work;

struct uart_data_t {
	void *fifo_reserved;
	uint8_t data[UART_BUF_SIZE];
	uint16_t len;
};

static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

#if CONFIG_BT_NUS_UART_ASYNC_ADAPTER
UART_ASYNC_ADAPTER_INST_DEFINE(async_adapter);
#else
static const struct device *const async_adapter;
#endif

#define ERR_OK "{\"err\":\"ok\"}"
#define ERR_PWD_NOT_FOUND "{\"err\":\"pwd not found\"}"
#define ERR_OPERATION_REJECTED "{\"err\":\"operation rejected\"}"
#define ERR_WRONG_FORMAT "{\"err\":\"wrong msg format\"}"
#define ERR_COMPLETE_STORAGE "{\"err\":\"storage is full\"}"

struct k_sem sem;
struct k_mutex state_mutex;
struct TPassword pwdStruct;

enum CURRENT_STATE {IDLE, WAITING_GET_PWD_CONF, WAITING_STORE_PWD_CONF, WAITING_DELETE_ALL, DELETE_ALL_CONFIRMED, WAITING_SHOW_LIST, WAITING_REQUEST_ERROR};
int state = IDLE;

char msg_rcv_buff[256];

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);

	static uint8_t *current_buf;
	static size_t aborted_len;
	static bool buf_release;
	struct uart_data_t *buf;
	static uint8_t *aborted_buf;

	switch (evt->type) {
	case UART_TX_DONE:
		LOG_DBG("tx_done");
		if ((evt->data.tx.len == 0) ||
		    (!evt->data.tx.buf)) {
			return;
		}

		if (aborted_buf) {
			buf = CONTAINER_OF(aborted_buf, struct uart_data_t,
					   data);
			aborted_buf = NULL;
			aborted_len = 0;
		} else {
			buf = CONTAINER_OF(evt->data.tx.buf, struct uart_data_t,
					   data);
		}

		k_free(buf);

		buf = k_fifo_get(&fifo_uart_tx_data, K_NO_WAIT);
		if (!buf) {
			return;
		}

		if (uart_tx(uart, buf->data, buf->len, SYS_FOREVER_MS)) {
			LOG_WRN("Failed to send data over UART (%d)", 99);
		}

		break;

	case UART_RX_RDY:
		LOG_DBG("rx_rdy");
		buf = CONTAINER_OF(evt->data.rx.buf, struct uart_data_t, data);
		buf->len += evt->data.rx.len;
		buf_release = false;

		if (buf->len == UART_BUF_SIZE) {
			k_fifo_put(&fifo_uart_rx_data, buf);
		} else if ((evt->data.rx.buf[buf->len - 1] == '\n') ||
			  (evt->data.rx.buf[buf->len - 1] == '\r')) {
			k_fifo_put(&fifo_uart_rx_data, buf);
			current_buf = evt->data.rx.buf;
			buf_release = true;
			uart_rx_disable(uart);
		}

		break;

	case UART_RX_DISABLED:
		LOG_DBG("rx_disabled");
		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
		} else {
			LOG_WRN("Not able to allocate UART receive buffer (%d)", 99);
			k_work_reschedule(&uart_work, UART_WAIT_FOR_BUF_DELAY);
			return;
		}

		uart_rx_enable(uart, buf->data, sizeof(buf->data),
			       UART_WAIT_FOR_RX);

		break;

	case UART_RX_BUF_REQUEST:
		LOG_DBG("rx_buf_request");
		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
			uart_rx_buf_rsp(uart, buf->data, sizeof(buf->data));
		} else {
			LOG_WRN("Not able to allocate UART receive buffer (%d)", 99);
		}

		break;

	case UART_RX_BUF_RELEASED:
		LOG_DBG("rx_buf_released");
		buf = CONTAINER_OF(evt->data.rx_buf.buf, struct uart_data_t,
				   data);
		if (buf_release && (current_buf != evt->data.rx_buf.buf)) {
			k_free(buf);
			buf_release = false;
			current_buf = NULL;
		}

		break;

	case UART_TX_ABORTED:
			LOG_DBG("tx_aborted");
			if (!aborted_buf) {
				aborted_buf = (uint8_t *)evt->data.tx.buf;
			}

			aborted_len += evt->data.tx.len;
			buf = CONTAINER_OF(aborted_buf, struct uart_data_t,
					   data);

			uart_tx(uart, &buf->data[aborted_len],
				buf->len - aborted_len, SYS_FOREVER_MS);

		break;

	default:
		break;
	}
}

static void uart_work_handler(struct k_work *item)
{
	struct uart_data_t *buf;

	buf = k_malloc(sizeof(*buf));
	if (buf) {
		buf->len = 0;
	} else {
		LOG_WRN("Not able to allocate UART receive buffer (%d)", 99);
		k_work_reschedule(&uart_work, UART_WAIT_FOR_BUF_DELAY);
		return;
	}

	uart_rx_enable(uart, buf->data, sizeof(buf->data), UART_WAIT_FOR_RX);
}

static bool uart_test_async_api(const struct device *dev)
{
	const struct uart_driver_api *api =
			(const struct uart_driver_api *)dev->api;

	return (api->callback_set != NULL);
}

static int uart_init(void)
{
	int err;
	int pos;
	struct uart_data_t *rx;
	struct uart_data_t *tx;

	uart = device_get_binding(CONFIG_BT_NUS_UART_DEV);
	if (!uart) {
		return -ENXIO;
	}

	if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
		err = usb_enable(NULL);
		if (err) {
			LOG_ERR("Failed to enable USB (%d)", err);
			return err;
		}
	}

	rx = k_malloc(sizeof(*rx));
	if (rx) {
		rx->len = 0;
	} else {
		return -ENOMEM;
	}

	k_work_init_delayable(&uart_work, uart_work_handler);


	if (IS_ENABLED(CONFIG_BT_NUS_UART_ASYNC_ADAPTER) && !uart_test_async_api(uart)) {
		/* Implement API adapter */
		uart_async_adapter_init(async_adapter, uart);
		uart = async_adapter;
	}

	err = uart_callback_set(uart, uart_cb, NULL);
	if (err) {
		LOG_ERR("Cannot initialize UART callback (%d)", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_UART_LINE_CTRL)) {
		LOG_INF("Wait for DTR%s","");
		while (true) {
			uint32_t dtr = 0;

			uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr);
			if (dtr) {
				break;
			}
			/* Give CPU resources to low priority threads. */
			k_sleep(K_MSEC(100));
		}
		LOG_INF("DTR set%s","");
		err = uart_line_ctrl_set(uart, UART_LINE_CTRL_DCD, 1);
		if (err) {
			LOG_WRN("Failed to set DCD, ret code %d", err);
		}
		err = uart_line_ctrl_set(uart, UART_LINE_CTRL_DSR, 1);
		if (err) {
			LOG_WRN("Failed to set DSR, ret code %d", err);
		}
	}

	tx = k_malloc(sizeof(*tx));

	if (tx) {
		pos = snprintf(tx->data, sizeof(tx->data),
			       "Starting BH Password Manager\r\n");

		if ((pos < 0) || (pos >= sizeof(tx->data))) {
			k_free(tx);
			LOG_ERR("snprintf returned %d", pos);
			return -ENOMEM;
		}

		tx->len = pos;
	} else {
		return -ENOMEM;
	}

	err = uart_tx(uart, tx->data, tx->len, SYS_FOREVER_MS);
	if (err) {
		LOG_ERR("Cannot display welcome message (err: %d)", err);
		return err;
	}

	return uart_rx_enable(uart, rx->data, sizeof(rx->data), 50);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected %s", log_strdup(addr));

	current_conn = bt_conn_ref(conn);

	dk_set_led_on(CON_STATUS_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason %u)", log_strdup(addr), reason);

	if (auth_conn) {
		bt_conn_unref(auth_conn);
		auth_conn = NULL;
	}

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
		dk_set_led_off(CON_STATUS_LED);
	}
}

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security changed: %s level %u", log_strdup(addr),
			level);
	} else {
		LOG_WRN("Security failed: %s level %u err %d", log_strdup(addr),
			level, err);
	}
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
	.security_changed = security_changed,
#endif
};

#if defined(CONFIG_BT_NUS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Passkey for %s: %06u", log_strdup(addr), passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	auth_conn = bt_conn_ref(conn);

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Passkey for %s: %06u", log_strdup(addr), passkey);
	LOG_INF("Press Button 1 to confirm, Button 2 to reject.%s","");
}


static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s", log_strdup(addr));
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing completed: %s, bonded: %d", log_strdup(addr),
		bonded);
}


static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing failed conn: %s, reason %d", log_strdup(addr),
		reason);
}


static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.cancel = auth_cancel,
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};
#else
static struct bt_conn_auth_cb conn_auth_callbacks;
#endif

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data,
			  uint16_t len)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN] = {0};

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, ARRAY_SIZE(addr));

	LOG_INF("Received data from: %s", log_strdup(addr));

	for (uint16_t pos = 0; pos != len;) {
		struct uart_data_t *tx = k_malloc(sizeof(*tx));

		if (!tx) {
			LOG_WRN("Not able to allocate UART send data buffer (%d)", 99);
			return;
		}

		/* Keep the last byte of TX buffer for potential LF char. */
		size_t tx_data_size = sizeof(tx->data) - 1;

		if ((len - pos) > tx_data_size) {
			tx->len = tx_data_size;
		} else {
			tx->len = (len - pos);
		}

		memcpy(tx->data, &data[pos], tx->len);

		pos += tx->len;

		/* Append the LF character when the CR character triggered
		 * transmission from the peer.
		 */
		if ((pos == len) && (data[len - 1] == '\r')) {
			tx->data[tx->len] = '\n';
			tx->len++;
		}

		/* Save the message received */
		char ble_msg[tx->len+1]; 
		if(tx->len < UART_BUF_SIZE) tx->data[tx->len] = '\0';
		strcpy(ble_msg, tx->data);

		/* The message received may contain confidential information. It is better to delete it */
		for(int i = 0; i < tx->len; i++){
			tx->data[i] = ' ';
		}

		err = uart_tx(uart, tx->data, tx->len, SYS_FOREVER_MS);
		if (err) {
			k_fifo_put(&fifo_uart_tx_data, tx);
		}

		printk("\n");

		/* A JSON message can arrive in different packets */
		if (ble_msg[tx->len - 1] == '}'){
			/* Last message */
			if(ble_msg[0] == '{'){
				/* It's a single message */
				strcpy(msg_rcv_buff, ble_msg);
			}else{
				strcat(msg_rcv_buff, ble_msg);
			}			

			/* Parse JSON
			Messages must be composed of 2 or 3 string values (depending on the type of message). Keys must be "username", "url" and (optionally) "pwd"
			Example: {"url": "myurl.something", "username": "myusername"} or {"username": "myusername", "url": "myurl.something", "pwd": "mypassword"}*/
			const cJSON *monitor_json = cJSON_Parse(msg_rcv_buff);
			if(monitor_json == NULL){
				printk("Error parsing JSON message\n");
			}else{
				const cJSON *json_url = cJSON_GetObjectItemCaseSensitive(monitor_json, "url");
				const cJSON *json_username = cJSON_GetObjectItemCaseSensitive(monitor_json, "user");
				const cJSON *json_pwd = cJSON_GetObjectItemCaseSensitive(monitor_json, "pwd");

				if( cJSON_IsString(json_url) && (json_url->valuestring != NULL) && cJSON_IsString(json_username) && (json_username->valuestring != NULL) ){
					/* It's a correct message */
					if(cJSON_IsString(json_pwd) && (json_pwd->valuestring != NULL)){
						/* It's a password register request */
						if( (strlen(json_url->valuestring) <= URL_SIZE) && (strlen(json_username->valuestring) <= USERNAME_SIZE) && (strlen(json_pwd->valuestring) <= PWD_SIZE) ){
							strcpy(pwdStruct.pwd, json_pwd->valuestring);
							strcpy(pwdStruct.url, json_url->valuestring);
							strcpy(pwdStruct.username, json_username->valuestring);

							k_mutex_lock(&state_mutex, K_FOREVER);
							state = WAITING_STORE_PWD_CONF;
							k_mutex_unlock(&state_mutex);

							printk("Do you want to store the password for user \"%s\"?\nTo confirm/reject, type Y/n\n", pwdStruct.username);
						}else{
							k_mutex_lock(&state_mutex, K_FOREVER);
							state = WAITING_REQUEST_ERROR;
							k_mutex_unlock(&state_mutex);

							printk("Message error. Make sure the fields do not exceed the maximum allowed length");

							k_sem_give(&sem);
						}
						
					}else{
						/* It's a password get request */
						if( (strlen(json_url->valuestring) <= URL_SIZE) && (strlen(json_username->valuestring) <= USERNAME_SIZE) ){
							strcpy(pwdStruct.url, json_url->valuestring);				
							strcpy(pwdStruct.username, json_username->valuestring);
							strcpy(pwdStruct.pwd, "");
							k_sem_give(&sem);
						}else{
							k_mutex_lock(&state_mutex, K_FOREVER);
							state = WAITING_REQUEST_ERROR;
							k_mutex_unlock(&state_mutex);

							printk("Message error. Make sure the fields do not exceed the maximum allowed length");

							k_sem_give(&sem);
						}
						
					}
				}else{
					printk("Wrong message format\n");
				}
			}
		} else if(ble_msg[0] == '{'){
			/* First message */
			strcpy(msg_rcv_buff, ble_msg);
			
		} else {
			/* Other message*/
			strcat(msg_rcv_buff, ble_msg);
		}
	}
}

static struct bt_nus_cb nus_cb = {
	.received = bt_receive_cb,
};

void error(void)
{
	dk_set_leds_state(DK_ALL_LEDS_MSK, DK_NO_LEDS_MSK);

	while (true) {
		/* Spin for ever */
		k_sleep(K_MSEC(1000));
	}
}

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
static void num_comp_reply(bool accept)
{
	if (accept) {
		bt_conn_auth_passkey_confirm(auth_conn);
		LOG_INF("Numeric Match, conn %p", (void *)auth_conn);
	} else {
		bt_conn_auth_cancel(auth_conn);
		LOG_INF("Numeric Reject, conn %p", (void *)auth_conn);
	}

	bt_conn_unref(auth_conn);
	auth_conn = NULL;
}

void button_changed(uint32_t button_state, uint32_t has_changed)
{
	uint32_t buttons = button_state & has_changed;

	if (auth_conn) {
		if (buttons & KEY_PASSKEY_ACCEPT) {
			num_comp_reply(true);
		}

		if (buttons & KEY_PASSKEY_REJECT) {
			num_comp_reply(false);
		}
	}
}
#endif /* CONFIG_BT_NUS_SECURITY_ENABLED */

static void configure_gpio(void)
{
	int err;

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
	err = dk_buttons_init(button_changed);
	if (err) {
		LOG_ERR("Cannot init buttons (err: %d)", err);
	}
#endif /* CONFIG_BT_NUS_SECURITY_ENABLED */

	err = dk_leds_init();
	if (err) {
		LOG_ERR("Cannot init LEDs (err: %d)", err);
	}
}

void main(void)
{
	int err = 0;

	configure_gpio();

	err = uart_init();
	if (err) {
		error();
	}

	if (IS_ENABLED(CONFIG_BT_NUS_SECURITY_ENABLED)) {
		bt_conn_auth_cb_register(&conn_auth_callbacks);
	}

	err = bt_enable(NULL);
	if (err) {
		error();
	}

	LOG_INF("Bluetooth initialized%s", "");

	k_sem_give(&ble_init_ok);

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("Failed to initialize UART service (err: %d)", err);
		return;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd,
			      ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	err = store_manager_init();
	if (err < 0){
		return;
	}

	k_sem_init(&sem, 0, 1);
	k_mutex_init(&state_mutex);

	for(;;){
		k_sem_take(&sem, K_FOREVER);

		k_mutex_lock(&state_mutex, K_FOREVER);
		int current_state = state;
		k_mutex_unlock(&state_mutex);

		if(current_state == DELETE_ALL_CONFIRMED){
			deleteAllPwd();
			printk("All stored passwords have been deleted\n");
			k_mutex_lock(&state_mutex, K_FOREVER);
			state = IDLE;
			k_mutex_unlock(&state_mutex);

		}else if(current_state == WAITING_SHOW_LIST){
			k_mutex_lock(&state_mutex, K_FOREVER);
			state = IDLE;
			k_mutex_unlock(&state_mutex);
			struct TPassword tmpPwdList[MAX_STORABLE_PWD];
			err = getAllPwd(tmpPwdList);
			if(err > 0){
				printk("List of stored password (%d):\n", err);
				for(int i = 0; i < err; i++){
					printk("\t%d. URL: %s, username: %s\n", i, tmpPwdList[i].url, tmpPwdList[i].username);
				}
			}else if(err == 0){
				printk("No password stored\n");
			}else{
				printk("err = %d\n", err);
			}

		}else if(current_state == WAITING_REQUEST_ERROR){
			if (bt_nus_send(NULL, ERR_WRONG_FORMAT, strlen(ERR_WRONG_FORMAT))) {
				LOG_WRN("Failed to send data over BLE connection (%d)", 99);
			}

			k_mutex_lock(&state_mutex, K_FOREVER);
			state = IDLE;
			k_mutex_unlock(&state_mutex);

		}else if(strcmp(pwdStruct.pwd, "") == 0){
			/* Get password */
			err = getPwd(&pwdStruct);
			if(err == 0){
				/* Password obtained */
				printk("New message:\n");
				printk("\t- URL: %s\n", pwdStruct.url);
				printk("\t- Username: %s\n", pwdStruct.username);
				/*printk("There is a password stored for this user: %s\n", pwdStruct.pwd);*/

				/* Password obtained. Ask user for confirmation */
				printk("There is a password stored for user '%s'.\nTo confirm/reject, type Y/n\n", pwdStruct.username);
				k_mutex_lock(&state_mutex, K_FOREVER);
				state = WAITING_GET_PWD_CONF;
				k_mutex_unlock(&state_mutex);
			}else{
				printk("Password is not stored (err = %d)\n", err);

				if (bt_nus_send(NULL, ERR_OPERATION_REJECTED, strlen(ERR_OPERATION_REJECTED))) {
					LOG_WRN("Failed to send data over BLE connection (%d)", 99);
				}
			}
		}else{
			printk("New message:\n");
			printk("\t- URL: %s\n", pwdStruct.url);
			printk("\t- Username: %s\n", pwdStruct.username);
			/*printk("\t- Password: %s\n", pwdStruct.pwd);*/
			printk("\t- Password: ********\n");

			/* Storage password*/
			err = storePwd(&pwdStruct);
			strcpy(pwdStruct.pwd, ""); // Must be empty for future uses
			if(err == 0){
				printk("Password stored\n");
				if (bt_nus_send(NULL, ERR_OK, strlen(ERR_OK))) {
					LOG_WRN("Failed to send data over BLE connection (%d)", 99);
				}
			}else if(err == -1){
				printk("Storage is full. No new password can be stored\n");
				if (bt_nus_send(NULL, ERR_COMPLETE_STORAGE, strlen(ERR_COMPLETE_STORAGE))) {
					LOG_WRN("Failed to send data over BLE connection (%d)", 99);
				}
			}			
		}
	}

	/*for (;;) {
		dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
		k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL));
	}*/
}

void ble_write_thread(void)
{
	/* Don't go any further until BLE is initialized */
	k_sem_take(&ble_init_ok, K_FOREVER);

	for (;;) {
		/* Wait indefinitely for data to be sent over bluetooth */
		struct uart_data_t *buf = k_fifo_get(&fifo_uart_rx_data,
						     K_FOREVER);

		char pwd_msg[12+strlen(pwdStruct.pwd)];

		k_mutex_lock(&state_mutex, K_FOREVER);
		switch(state){
			case IDLE:
				k_mutex_unlock(&state_mutex);
				if( buf->len < UART_BUF_SIZE) buf->data[buf->len] = '\0';
				if( buf->data[buf->len - 1] == '\r' || buf->data[buf->len - 1] == '\n'){
					buf->data[buf->len - 1] = '\0';
					if( buf->len > 1 && (buf->data[buf->len - 2] == '\r' || buf->data[buf->len - 2] == '\n') ){
						buf->data[buf->len - 2] = '\0';
					}
				}

				if(strcmp((char *) buf->data, "clear storage") == 0){
					k_mutex_lock(&state_mutex, K_FOREVER);
					state = WAITING_DELETE_ALL;
					k_mutex_unlock(&state_mutex);
					printk("Are you sure you want to delete ALL passwords?\nTo confirm/reject, type Y/n\n");
				}else if(strcmp((char *) buf->data, "list") == 0){
					k_mutex_lock(&state_mutex, K_FOREVER);
					state = WAITING_SHOW_LIST;
					k_mutex_unlock(&state_mutex);
					k_sem_give(&sem);
				}
				break;

			case WAITING_DELETE_ALL:
				k_mutex_unlock(&state_mutex);
				if( buf->len < UART_BUF_SIZE) buf->data[buf->len] = '\0';
				if(buf->data[0]=='Y' || buf->data[0]=='y'){
					k_mutex_lock(&state_mutex, K_FOREVER);
					state = DELETE_ALL_CONFIRMED;
					k_mutex_unlock(&state_mutex);

					k_sem_give(&sem);
				}
				break;

			case WAITING_GET_PWD_CONF:
				state = IDLE;
				k_mutex_unlock(&state_mutex);

				if( buf->len < UART_BUF_SIZE) buf->data[buf->len] = '\0';
				if(buf->data[0]=='Y' || buf->data[0]=='y'){
					strcpy(pwd_msg, "{\"pwd\": \"" );
					strcpy(pwd_msg + 9, pwdStruct.pwd);
					strcpy(pwd_msg + 9 + strlen(pwdStruct.pwd), "\"}");
					strcpy(pwdStruct.pwd, ""); /* Must be empty for future uses */
					
					if (bt_nus_send(NULL, pwd_msg, strlen(pwd_msg))) {
						LOG_WRN("Failed to send data over BLE connection (%d)", 99);
					}else{
						printk("Password sent to client\n");
					}
				}else{
					if (bt_nus_send(NULL, ERR_OPERATION_REJECTED, strlen(ERR_OPERATION_REJECTED))) {
						LOG_WRN("Failed to send data over BLE connection (%d)", 99);
					}
				}
				break;

			case WAITING_STORE_PWD_CONF:
				state = IDLE;
				k_mutex_unlock(&state_mutex);

				if( buf->len < UART_BUF_SIZE) buf->data[buf->len] = '\0';
				if(buf->data[0]=='Y' || buf->data[0]=='y'){
					k_sem_give(&sem);
				}else{
					printk("Password storage cancelled\n");
					if (bt_nus_send(NULL, ERR_OPERATION_REJECTED, strlen(ERR_OPERATION_REJECTED))) {
						LOG_WRN("Failed to send data over BLE connection (%d)", 99);
					}else{
						printk("Sent: %s\n", ERR_OPERATION_REJECTED);
					}
				}
				break;

			default:
				k_mutex_unlock(&state_mutex);
				break;
		}

		k_free(buf);
	}
}

K_THREAD_DEFINE(ble_write_thread_id, STACKSIZE, ble_write_thread, NULL, NULL,
		NULL, PRIORITY, 0, 0);
