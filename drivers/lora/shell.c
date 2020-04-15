/*
 * Copyright (c) 2020 Andreas Sandberg
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <drivers/lora.h>
#include <inttypes.h>
#include <shell/shell.h>
#include <stdlib.h>
#include <string.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL

LOG_MODULE_REGISTER(lora_shell);

#define DEFAULT_RADIO_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_HAS_NODE(DEFAULT_RADIO_NODE),
	     "No default LoRa radio specified in DT");
#define DEFAULT_RADIO DT_LABEL(DEFAULT_RADIO_NODE)

static struct lora_modem_config modem_config = {
	.frequency = 0,
	.bandwidth = BW_125_KHZ,
	.datarate = SF_10,
	.coding_rate = CR_4_5,
	.preamble_len = 8,
	.tx_power = 4,
};

static const int bw_table[] = {
	[BW_125_KHZ] = 125,
	[BW_250_KHZ] = 250,
	[BW_500_KHZ] = 500,
};

static struct device *get_modem(const struct shell *shell,
				struct lora_modem_config *cfg)
{
	struct device *dev;
	int ret;

	dev = device_get_binding(DEFAULT_RADIO);
	if (!dev) {
		shell_error(shell, "%s Device not found", DEFAULT_RADIO);
		return NULL;
	}

	if (cfg) {
		if (cfg->frequency == 0) {
			shell_error(shell, "No frequency specified.");
			return NULL;
		}

		ret = lora_config(dev, cfg);
		if (ret < 0) {
			shell_error(shell, "LoRa config failed");
			return NULL;
		}
	}

	return dev;
}
static int cmd_lora_conf(const struct shell *shell, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(shell, DEFAULT_RADIO ":");
		shell_print(shell, "  Frequency: %" PRIu32 " Hz",
			    modem_config.frequency);
		shell_print(shell, "  TX power: %" PRIi8 " dBm",
			    modem_config.tx_power);
		shell_print(shell, "  Bandwidth: %i kHz",
			    bw_table[modem_config.bandwidth]);
		shell_print(shell, "  Spreading factor: SF%i",
			    (int)modem_config.datarate);
		shell_print(shell, "  Coding rate: 4/%i",
			    (int)modem_config.coding_rate + 4);
		shell_print(shell, "  Preamble length: %" PRIu16,
			    modem_config.preamble_len);

		return 0;
	}

	for (int i = 1; i < argc; i += 2) {
		char *param = argv[i];
		char *eptr;
		int value;

		if (i + 1 >= argc) {
			shell_error(shell, "'%s' expects an argument",
				    argv[i]);
			return -EINVAL;
		}

		value = strtol(argv[i + 1], &eptr, 0);
		if (*eptr != '\0') {
			shell_error(shell, "'%s' is not an integer",
				    argv[i + 1]);
			return -EINVAL;
		}

		if (!strcmp("freq", param)) {
			modem_config.frequency = value;
		} else if (!strcmp("tx-power", param)) {
			modem_config.tx_power = value;
		} else if (!strcmp("bw", param)) {
			switch (value) {
			case 125:
				modem_config.bandwidth = BW_125_KHZ;
				break;
			case 250:
				modem_config.bandwidth = BW_250_KHZ;
				break;
			case 500:
				modem_config.bandwidth = BW_500_KHZ;
				break;
			default:
				shell_error(shell, "Invalid bandwidth: %s",
					    value);
				return -EINVAL;
			}
		} else if (!strcmp("sf", param)) {
			if (value < SF_6 || value > SF_12) {
				shell_error(shell,
					    "Invalid spreading factor: SF%i",
					    value);
				return -EINVAL;
			}

			modem_config.datarate = value;
		} else if (!strcmp("cr", param)) {
			if (value < 5 || value > 8) {
				shell_error(shell, "Coding rate: 4/%i",
					    value);
				return -EINVAL;
			}

			modem_config.coding_rate = CR_4_5 + value - 5;
		} else if (!strcmp("pre-len", param)) {
			modem_config.preamble_len = value;
		} else {
			shell_error(shell, "Unknown parameter '%s'", param);
			return -EINVAL;
		}
	}

	return 0;
}

static int cmd_lora_send(const struct shell *shell,
			size_t argc, char **argv)
{
	int ret;
	struct device *dev;

	modem_config.tx = true;
	dev = get_modem(shell, &modem_config);
	if (!dev) {
		return -ENODEV;
	}

	ret = lora_send(dev, argv[1], strlen(argv[1]));
	if (ret < 0) {
		shell_error(shell, "LoRa send failed: %i", ret);
		return ret;
	}

	return 0;
}

static int cmd_lora_recv(const struct shell *shell,
			size_t argc, char **argv)
{
	static char buf[0xff];
	struct device *dev;
	int ret;
	s16_t rssi;
	s8_t snr;

	modem_config.tx = false;
	dev = get_modem(shell, &modem_config);
	if (!dev) {
		return -ENODEV;
	}

	ret = lora_recv(dev, buf, sizeof(buf), K_FOREVER, &rssi, &snr);
	if (ret < 0) {
		shell_error(shell, "LoRa recv failed: %i", ret);
		return ret;
	}

	shell_print(shell, "Data: %s", buf);
	shell_print(shell, "RSSI: %" PRIi16 " dBm, SNR:%" PRIi8 " dBm",
		    rssi, snr);

	return 0;
}

static int cmd_lora_test_cw(const struct shell *shell,
			    size_t argc, char **argv)
{
	struct device *dev;
	int ret;
	u32_t freq;
	s8_t power;
	u16_t duration;

	dev = get_modem(shell, NULL);
	if (!dev) {
		return -ENODEV;
	}

	freq = atoi(argv[1]);
	power = atoi(argv[2]);
	duration = atoi(argv[3]);

	ret = lora_test_cw(dev, freq, power, duration);
	if (ret < 0) {
		shell_error(shell, "LoRa test CW failed: %i", ret);
		return ret;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lora,
	SHELL_CMD_ARG(config, NULL,
		      "Configure the LoRa radio\n"
		      " Usage: config [freq <Hz>] [tx-power <dBm>] [bw <kHz>] "
		      "[sf <int>] [cr <int>] [pre-len <int>]\n",
		      cmd_lora_conf, 1, 6),
	SHELL_CMD_ARG(send, NULL,
		      "Send LoRa packet\n"
		      " Usage: send <data>",
		      cmd_lora_send, 2, 0),
	SHELL_CMD_ARG(recv, NULL,
		      "Receive LoRa packet\n"
		      " Usage: recv",
		      cmd_lora_recv, 1, 0),
	SHELL_CMD_ARG(test_cw, NULL,
		  "Send a continuous wave\n"
		  " Usage: test_cw <freq (Hz)> <power (dBm)> <duration (s)>",
		  cmd_lora_test_cw, 4, 0),
	SHELL_SUBCMD_SET_END /* Array terminated. */
);

SHELL_CMD_REGISTER(lora, &sub_lora, "LoRa commands", NULL);
