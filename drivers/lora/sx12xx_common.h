/*
 * Copyright (c) 2020 Andreas Sandberg
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_SX12XX_COMMON_H_
#define ZEPHYR_DRIVERS_SX12XX_COMMON_H_

#include <zephyr/types.h>
#include <drivers/lora.h>
#include <device.h>

void sx12xx_ev_rx_done(u8_t *payload, u16_t size, int16_t rssi, int8_t snr);
void sx12xx_ev_tx_done(void);

int sx12xx_lora_send(struct device *dev, u8_t *data, u32_t data_len);

int sx12xx_lora_recv(struct device *dev, u8_t *data, u8_t size,
		     s32_t timeout, s16_t *rssi, s8_t *snr);

int sx12xx_lora_config(struct device *dev, struct lora_modem_config *config);

int sx12xx_lora_test_cw(struct device *dev, u32_t frequency,
			s8_t tx_power, u16_t duration);

int sx12xx_lora_init(struct device *dev);

#endif	/* ZEPHYR_DRIVERS_SX12XX_COMMON_H_ */
