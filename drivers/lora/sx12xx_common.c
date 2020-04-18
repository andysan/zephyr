/*
 * Copyright (c) 2019 Manivannan Sadhasivam
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <drivers/counter.h>
#include <zephyr.h>

#include <radio.h>

#include "sx12xx_common.h"

#define LOG_LEVEL CONFIG_LORA_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(sx12xx);

static struct sx12xx_data {
	struct device *counter;
	struct k_sem data_sem;
	u8_t *rx_buf;
	u8_t rx_len;
	s8_t snr;
	s16_t rssi;
} dev_data;


void RtcStopAlarm(void)
{
	counter_stop(dev_data.counter);
}

uint32_t RtcGetTimerElapsedTime(void)
{
	u32_t ticks;
	int err;

	err = counter_get_value(dev_data.counter, &ticks);
	if (err) {
		LOG_ERR("Failed to read counter value (err %d)", err);
		return 0;
	}

	return ticks;
}

uint32_t RtcGetTimerValue(void)
{
	u32_t ticks;
	int err;

	err = counter_get_value(dev_data.counter, &ticks);
	if (err) {
		LOG_ERR("Failed to read counter value (err %d)", err);
		return 0;
	}

	return ticks;
}


u32_t RtcGetMinimumTimeout(void)
{
	/* TODO: Get this value from counter driver */
	return 3;
}

void RtcSetAlarm(uint32_t timeout)
{
	struct counter_alarm_cfg alarm_cfg;

	alarm_cfg.flags = 0;
	alarm_cfg.ticks = timeout;

	counter_set_channel_alarm(dev_data.counter, 0, &alarm_cfg);
}

uint32_t RtcSetTimerContext(void)
{
	return 0;
}

uint32_t RtcMs2Tick(uint32_t milliseconds)
{
	return counter_us_to_ticks(dev_data.counter, (milliseconds / 1000));
}

uint32_t RtcTick2Ms(uint32_t tick)
{
	return counter_ticks_to_us(dev_data.counter, tick) / 1000;
}

void DelayMsMcu(uint32_t ms)
{
	k_sleep(ms);
}

void BoardCriticalSectionBegin(uint32_t *mask)
{
	*mask = irq_lock();
}

void BoardCriticalSectionEnd(uint32_t *mask)
{
	irq_unlock(*mask);
}

void sx12xx_ev_tx_done(void)
{
	Radio.Sleep();
}

void sx12xx_ev_rx_done(u8_t *payload, u16_t size, int16_t rssi, int8_t snr)
{
	Radio.Sleep();

	dev_data.rx_buf = payload;
	dev_data.rx_len = size;
	dev_data.rssi = rssi;
	dev_data.snr = snr;

	k_sem_give(&dev_data.data_sem);
}

int sx12xx_lora_send(struct device *dev, u8_t *data, u32_t data_len)
{
	Radio.SetMaxPayloadLength(MODEM_LORA, data_len);

	Radio.Send(data, data_len);

	return 0;
}

int sx12xx_lora_recv(struct device *dev, u8_t *data, u8_t size,
		     s32_t timeout, s16_t *rssi, s8_t *snr)
{
	int ret;

	Radio.SetMaxPayloadLength(MODEM_LORA, 255);
	Radio.Rx(0);

	/*
	 * As per the API requirement, timeout value can be in ms/K_FOREVER/
	 * K_NO_WAIT. So, let's handle all cases.
	 */
	ret = k_sem_take(&dev_data.data_sem, timeout == K_FOREVER ? K_FOREVER :
			 timeout == K_NO_WAIT ? K_NO_WAIT : K_MSEC(timeout));
	if (ret < 0) {
		LOG_ERR("Receive timeout!");
		return ret;
	}

	/* Only copy the bytes that can fit the buffer, drop the rest */
	if (dev_data.rx_len > size)
		dev_data.rx_len = size;

	/*
	 * FIXME: We are copying the global buffer here, so it might get
	 * overwritten inbetween when a new packet comes in. Use some
	 * wise method to fix this!
	 */
	memcpy(data, dev_data.rx_buf, dev_data.rx_len);

	if (rssi != NULL) {
		*rssi = dev_data.rssi;
	}

	if (snr != NULL) {
		*snr = dev_data.snr;
	}

	return dev_data.rx_len;
}

int sx12xx_lora_config(struct device *dev, struct lora_modem_config *config)
{
	Radio.SetChannel(config->frequency);

	if (config->tx) {
		Radio.SetTxConfig(MODEM_LORA, config->tx_power, 0,
				  config->bandwidth, config->datarate,
				  config->coding_rate, config->preamble_len,
				  false, true, 0, 0, false, 4000);
	} else {
		/* TODO: Get symbol timeout value from config parameters */
		Radio.SetRxConfig(MODEM_LORA, config->bandwidth,
				  config->datarate, config->coding_rate,
				  0, config->preamble_len, 10, false, 0,
				  false, 0, 0, false, true);
	}

	return 0;
}

int sx12xx_lora_test_cw(struct device *dev, u32_t frequency,
			s8_t tx_power, u16_t duration)
{
	Radio.SetTxContinuousWave(frequency, tx_power, duration);
	return 0;
}

int sx12xx_lora_init(struct device *dev)
{
	dev_data.counter = device_get_binding(DT_RTC_0_NAME);
	if (!dev_data.counter) {
		LOG_ERR("Cannot get pointer to %s device", DT_RTC_0_NAME);
		return -EIO;
	}

	k_sem_init(&dev_data.data_sem, 0, UINT_MAX);

	return 0;
}
