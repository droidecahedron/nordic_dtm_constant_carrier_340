# Bluetooth: Direct Test Mode

> [!IMPORTANT]
> This repo is an unofficial modification to the dtm sample in ncs 3.4.0 to get constant carrier in the GUI to work as intended.
>
> The **main** branch of this repository is just a 1:1 clone of direct_test_mode, with this readme for more information.
>
> The [**constant_carrier**](https://github.com/droidecahedron/nordic_dtm_constant_carrier_340/tree/constantcarrier) branch contains the workaround.

## Explanation
When you select constant carrier, you'll see an `EINVAL` if you enable RTT logs around the constant carrier command.

```
 [00:04:57.293,773] <inf> dtm_tw_transport: Received 0x9303 command
Received 2-wire command 0x9303
Sending 2-wire event 0x0001 <-- 
```

But the nRF Connect 3.4.0 SDK supports constant carrier. ([src](https://github.com/nrfconnect/sdk-nrfxlib/blob/dbeb418f40ea6f06b5789d02758db3d0375ba555/softdevice_controller/include/sdc_hci_vs.h#L1942))

Culprit looks to happen here:

<img width="736" height="437" alt="image" src="https://github.com/user-attachments/assets/e6d80443-e7a9-461d-a8cf-df3344d87306" />

So it never falls into the `DTM_TW_TO_HCI_STATUS_HCI_CMD` case in `main.c` if you follow the `lib/dtm_twowire/dtm_twowire_to_hci.c` > `on_test_tx_cmd()` > `get_hci_param_pkt_payload()` chain of events.

The [**constant_carrier**](https://github.com/droidecahedron/nordic_dtm_constant_carrier_340/tree/constantcarrier) adds a workaround that
intercepts the frame in main.c, hand-builds `0xFD23` with the TXPOWER, `bt_send()`s it, and maps the command complete status. 
Succinctly it re-adds the 2-wire->VScmd translation that was removed, in the application code instead of the lib.

## Results
### Original sample, constant carrier TX. (Since it gets rejected, this can be considered the control. Starting/stopping TX results in no ch22 spikes)

<img width="1271" height="359" alt="image" src="https://github.com/user-attachments/assets/dd94a76a-dd05-4da4-bba6-97857ddad10d" />

### Original sample, PRBS9 TX. (You will see expected spike in channel spike)

<img width="1271" height="359" alt="image" src="https://github.com/user-attachments/assets/c2170b0b-e210-434f-9a7f-4380004b09ab" />

### Modified sample in [constantcarrier](https://github.com/droidecahedron/nordic_dtm_constant_carrier_340/tree/constantcarrier) branch, seeing CH22 spikes as expected.

<img width="1271" height="359" alt="image" src="https://github.com/user-attachments/assets/86ac6d2a-6b85-4b7b-85bf-fe644619a18f" />


## Extra

> [!NOTE]
> The API is being deprecated, but the feature should  not be.
> 
> In NCS v3.5.x and onward, 0xFD23 will lead to BT_HCI_ERR_UNKNOWN_CMD, looks to be fenced off in the 3.5+ branches by CONFIG_BT_HCI_SUPPORT_DEPRECATED_COMMANDS.
>
> It states "Use the Transmitter Carrier Test subcommand of the VS DTM command instead."
> I.e. 0xFD23 -> 0xFC1F with a 1-byte sub opcode prefix, 0x1 for carrier test.

```c
typedef struct { uint8_t sub_opcode; } sdc_hci_vs_dtm_command_header_t;   /* 0x01 = carrier test */

typedef struct {
	sdc_hci_vs_dtm_command_header_t header;
	uint8_t tx_channel;
	int8_t  tx_power_level;
} sdc_hci_cmd_vs_dtm_transmitter_carrier_test_t;
```

So the swap for this would probably something like this in `main.c`.
```c
cmd_hdr->opcode = sys_cpu_to_le16(0xFC1F);
cmd_hdr->param_len = 3U;
net_buf_add_u8(cmd, 0x01);          /* SDC_HCI_VS_DTM_COMMAND_OPCODE_TRANSMITTER_CARRIER_TEST */
net_buf_add_u8(cmd, channel);
net_buf_add_u8(cmd, (uint8_t)tx_power_level);
```
