/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Based on samples/bluetooth/direct_test_mode/src/main.c from nRF Connect SDK v3.4.0,
 * with vendor-specific constant carrier (CARRIER_TEST / CARRIER_TEST_STUDIO) support
 * added on top of the SoftDevice Controller vendor-specific HCI command
 * SDC_HCI_OPCODE_CMD_VS_TRANSMITTER_CARRIER_TEST (0xFD23).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/bluetooth/hci_types.h>

#include <bluetooth/dtm_twowire/dtm_twowire_to_hci.h>
#include <bluetooth/dtm_twowire/dtm_twowire_types.h>

#include "dtm_twowire_transport.h"

static K_FIFO_DEFINE(hci_c2h_queue);

/* ---------------------------------------------------------------------------
 * Vendor-specific constant carrier
 *
 * The DTM 2-wire to HCI conversion library rejects vendor-specific packet
 * payloads (PKT = 0b11 on a non-Coded PHY), so the vendor-specific
 * CARRIER_TEST / CARRIER_TEST_STUDIO commands are handled here instead and
 * mapped onto the SoftDevice Controller vendor-specific HCI command
 * "Transmitter carrier frequency test".
 * ---------------------------------------------------------------------------
 */

/* SDC_HCI_OPCODE_CMD_VS_TRANSMITTER_CARRIER_TEST, see nrfxlib sdc_hci_vs.h.
 * Requires CONFIG_BT_HCI_VS=y and CONFIG_BT_CTLR_DTM_HCI=y.
 *
 * Note: deprecated after v3.4.x in favour of SDC_HCI_OPCODE_CMD_VS_DTM_COMMAND
 * (0xFC1F) with sub-opcode SDC_HCI_VS_DTM_COMMAND_OPCODE_TRANSMITTER_CARRIER_TEST.
 */
#define SDC_HCI_OP_VS_TX_CARRIER_TEST 0xFD23

/* TX_Power_Level default, matching the conversion library's reset value
 * (Bluetooth Core v6.2, Vol 6, Part F, 3.3.2). The library resets its buffered
 * parameters with a memset, so an unset transmit power means 0 dBm.
 *
 * The values 0x7E and 0x7F select the controller's minimum and maximum
 * transmit power and are passed through to the controller unmodified.
 */
#define TX_POWER_LEVEL_DEFAULT 0

/* 2-wire command fields, see Bluetooth Core specification Vol 6, Part F, 3.3. */
#define TW_CMD_CODE(c)      (((c) >> 14) & 0x03)
#define TW_CMD_CHANNEL(c)   (((c) >> 8) & 0x3F)
#define TW_CMD_LENGTH(c)    (((c) >> 2) & 0x3F)
#define TW_CMD_PKT_TYPE(c)  ((c) & 0x03)
#define TW_CMD_CONTROL(c)   (((c) >> 8) & 0x3F)
#define TW_CMD_PARAMETER(c) ((c) & 0xFF)

/* Length field values of the vendor-specific transmitter test command. */
#define TW_VS_CARRIER_TEST        0x00
#define TW_VS_CARRIER_TEST_STUDIO 0x01

/* TX power the carrier is transmitted at. Shadowed from the 2-wire
 * LE_Test_Setup commands so that a tester-selected power level is honored and
 * a tester reset restores the default, mirroring the conversion library.
 */
static int8_t tx_power_level = TX_POWER_LEVEL_DEFAULT;

static bool tw_cmd_is_carrier_test(uint16_t tw_cmd)
{
	if (TW_CMD_CODE(tw_cmd) != DTM_TW_CMD_TRANSMITTER_TEST ||
	    TW_CMD_PKT_TYPE(tw_cmd) != DTM_TW_PKT_0XFF_OR_VS) {
		return false;
	}

	const uint8_t length = TW_CMD_LENGTH(tw_cmd);

	return (length == TW_VS_CARRIER_TEST) || (length == TW_VS_CARRIER_TEST_STUDIO);
}

static void tw_cmd_shadow_tx_power(uint16_t tw_cmd)
{
	if (TW_CMD_CODE(tw_cmd) != DTM_TW_CMD_TEST_SETUP) {
		return;
	}

	switch (TW_CMD_CONTROL(tw_cmd)) {
	case DTM_TW_TEST_SETUP_RESET:
		tx_power_level = TX_POWER_LEVEL_DEFAULT;
		break;

	case DTM_TW_TEST_SETUP_TRANSMIT_POWER:
		tx_power_level = (int8_t)TW_CMD_PARAMETER(tw_cmd);
		break;

	default:
		break;
	}
}

static uint16_t carrier_test_start(uint8_t channel)
{
	struct net_buf *cmd = bt_buf_get_tx(BT_BUF_CMD, K_FOREVER, NULL, 0);
	struct bt_hci_cmd_hdr *cmd_hdr = net_buf_add(cmd, sizeof(*cmd_hdr));

	cmd_hdr->opcode = sys_cpu_to_le16(SDC_HCI_OP_VS_TX_CARRIER_TEST);
	cmd_hdr->param_len = 2U;

	net_buf_add_u8(cmd, channel);                  /* TX_Channel, 0 - 39 */
	net_buf_add_u8(cmd, (uint8_t)tx_power_level);  /* TX_Power_Level, dBm */

	printk("Starting constant carrier on channel %u at %d dBm\n", channel, tx_power_level);

	int err = bt_send(cmd);

	/* Note: bt_send unrefs the buffer on success */
	if (err) {
		net_buf_unref(cmd);
		return DTM_TW_EVENT_TEST_STATUS_ERROR;
	}

	struct net_buf *evt = k_fifo_get(&hci_c2h_queue, K_FOREVER);

	/* H4 packet indicator, event header, Command Complete header, status byte. */
	const size_t status_offset = 1 + sizeof(struct bt_hci_evt_hdr) +
				     sizeof(struct bt_hci_evt_cmd_complete);
	uint8_t status = (evt->len > status_offset) ? evt->data[status_offset]
						   : BT_HCI_ERR_UNSPECIFIED;

	net_buf_unref(evt);

	if (status != BT_HCI_ERR_SUCCESS) {
		printk("Carrier test command failed with status 0x%02X\n", status);
		return DTM_TW_EVENT_TEST_STATUS_ERROR;
	}

	return DTM_TW_EVENT_TEST_STATUS_SUCCESS;
}

int main(void)
{
	printk("Starting DTM sample\n");

	int err;

	err = dtm_tw_transport_init();
	if (err) {
		return err;
	}

	err = bt_enable_raw(&hci_c2h_queue);
	if (err) {
		return err;
	}

	for (;;) {
		const uint16_t tw_cmd = dtm_tw_transport_read();

		printk("Received 2-wire command 0x%04X\n", tw_cmd);

		/* Keep track of the tester-selected TX power, then let the
		 * conversion library handle the command as usual.
		 */
		tw_cmd_shadow_tx_power(tw_cmd);

		if (tw_cmd_is_carrier_test(tw_cmd)) {
			const uint16_t tw_event = carrier_test_start(TW_CMD_CHANNEL(tw_cmd));

			printk("Sending 2-wire event 0x%04X\n", tw_event);
			dtm_tw_transport_write(tw_event);
			continue;
		}

		uint16_t tw_event;
		struct net_buf *p_hci_pkt = bt_buf_get_tx(BT_BUF_CMD, K_FOREVER, NULL, 0);

		dtm_tw_to_hci_status_t status = dtm_tw_to_hci_process_tw_cmd(tw_cmd,
									     p_hci_pkt,
									     &tw_event);
		switch (status) {
		case DTM_TW_TO_HCI_STATUS_ERROR:
			net_buf_unref(p_hci_pkt);
			printk("Error processing 2-wire command 0x%04X\n", tw_cmd);
			break;

		case DTM_TW_TO_HCI_STATUS_TW_EVENT:
			net_buf_unref(p_hci_pkt);
			printk("Sending 2-wire event 0x%04X\n", tw_event);
			dtm_tw_transport_write(tw_event);
			break;

		case DTM_TW_TO_HCI_STATUS_HCI_CMD:
			printk("Sending HCI command to SDC\n");
			err = bt_send(p_hci_pkt);
			/* Note: bt_send unrefs the buffer on success */
			if (err) {
				net_buf_unref(p_hci_pkt);
				return err;
			}

			p_hci_pkt = k_fifo_get(&hci_c2h_queue, K_FOREVER);
			status = dtm_tw_to_hci_process_hci_event(tw_cmd,
								 p_hci_pkt,
								 &tw_event);
			net_buf_unref(p_hci_pkt);
			if (status == DTM_TW_TO_HCI_STATUS_TW_EVENT) {
				dtm_tw_transport_write(tw_event);
			}
			break;

		default:
			net_buf_unref(p_hci_pkt);
			printk("Unexpected status %d processing 2-wire command 0x%04X\n",
			       status, tw_cmd);
			break;
		}
	}
}