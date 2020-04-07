/*
 * Copyright (c) 2019 Manivannan Sadhasivam
 * Copyright (c) 2020 Grinn
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT semtech_sx1276

#include <drivers/counter.h>
#include <drivers/gpio.h>
#include <drivers/lora.h>
#include <drivers/spi.h>
#include <zephyr.h>

#include <sx1276/sx1276.h>

#define LOG_LEVEL CONFIG_LORA_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(sx1276);

#define GPIO_RESET_PIN		DT_INST_GPIO_PIN(0, reset_gpios)
#define GPIO_RESET_FLAGS	DT_INST_GPIO_FLAGS(0, reset_gpios)
#define GPIO_CS_PIN		DT_INST_SPI_DEV_CS_GPIOS_PIN(0)

#define GPIO_ANTENNA_ENABLE_PIN				\
	DT_INST_GPIO_PIN(0, antenna_enable_gpios)
#define GPIO_ANTENNA_ENABLE_FLAGS			\
	DT_INST_GPIO_FLAGS(0, antenna_enable_gpios)

#define GPIO_RFI_ENABLE_PIN			\
	DT_INST_GPIO_PIN(0, rfi_enable_gpios)
#define GPIO_RFI_ENABLE_FLAGS			\
	DT_INST_GPIO_FLAGS(0, rfi_enable_gpios)

#define GPIO_RFO_ENABLE_PIN			\
	DT_INST_GPIO_PIN(0, rfo_enable_gpios)
#define GPIO_RFO_ENABLE_FLAGS			\
	DT_INST_GPIO_FLAGS(0, rfo_enable_gpios)

#define GPIO_PA_BOOST_ENABLE_PIN			\
	DT_INST_GPIO_PIN(0, pa_boost_enable_gpios)
#define GPIO_PA_BOOST_ENABLE_FLAGS			\
	DT_INST_GPIO_FLAGS(0, pa_boost_enable_gpios)

#define GPIO_TCXO_POWER_PIN	DT_INST_GPIO_PIN(0, tcxo_power_gpios)
#define GPIO_TCXO_POWER_FLAGS	DT_INST_GPIO_FLAGS(0, tcxo_power_gpios)

#if DT_INST_NODE_HAS_PROP(0, tcxo_power_startup_delay_ms)
#define TCXO_POWER_STARTUP_DELAY_MS			\
	DT_INST_PROP(0, tcxo_power_startup_delay_ms)
#else
#define TCXO_POWER_STARTUP_DELAY_MS		0
#endif

/*
 * Those macros must be in sync with 'power-amplifier-output' dts property.
 */
#define SX1276_PA_RFO		0
#define SX1276_PA_BOOST		1

#if DT_INST_NODE_HAS_PROP(0, rfo_enable_gpios) &&	\
	DT_INST_NODE_HAS_PROP(0, pa_boost_enable_gpios)
#define SX1276_PA_OUTPUT(power)				\
	((power) > 14 ? SX1276_PA_BOOST : SX1276_PA_RFO)
#elif DT_INST_NODE_HAS_PROP(0, rfo_enable_gpios)
#define SX1276_PA_OUTPUT(power)		SX1276_PA_RFO
#elif DT_INST_NODE_HAS_PROP(0, pa_boost_enable_gpios)
#define SX1276_PA_OUTPUT(power)		SX1276_PA_BOOST
#elif DT_INST_NODE_HAS_PROP(0, power_amplifier_output)
#define SX1276_PA_OUTPUT(power)				\
	DT_ENUM_IDX(DT_DRV_INST(0), power_amplifier_output)
#else
BUILD_ASSERT(0, "None of rfo-enable-gpios, pa-boost-enable-gpios and "
	     "power-amplifier-output has been specified. "
	     "Look at semtech,sx1276.yaml to fix that.");
#endif

#define SX1276_REG_PA_CONFIG			0x09
#define SX1276_REG_PA_DAC			0x4d
#define SX1276_REG_VERSION			0x42

#define SX1276_PA_CONFIG_MAX_POWER_SHIFT	4

extern DioIrqHandler *DioIrq[];

struct sx1276_dio {
	const char *port;
	gpio_pin_t pin;
	gpio_dt_flags_t flags;
};

/* Helper macro that UTIL_LISTIFY can use and produces an element with comma */
#define SX1276_DIO_GPIO_ELEM(idx, inst) \
	{ \
		DT_INST_GPIO_LABEL_BY_IDX(inst, dio_gpios, idx), \
		DT_INST_GPIO_PIN_BY_IDX(inst, dio_gpios, idx), \
		DT_INST_GPIO_FLAGS_BY_IDX(inst, dio_gpios, idx), \
	},

#define SX1276_DIO_GPIO_INIT(n) \
	UTIL_LISTIFY(DT_INST_PROP_LEN(n, dio_gpios), SX1276_DIO_GPIO_ELEM, n)

static const struct sx1276_dio sx1276_dios[] = { SX1276_DIO_GPIO_INIT(0) };

#define SX1276_MAX_DIO ARRAY_SIZE(sx1276_dios)

struct sx1276_data {
	struct device *counter;
	struct device *reset;
#if DT_INST_NODE_HAS_PROP(0, antenna_enable_gpios)
	struct device *antenna_enable;
#endif
#if DT_INST_NODE_HAS_PROP(0, rfi_enable_gpios)
	struct device *rfi_enable;
#endif
#if DT_INST_NODE_HAS_PROP(0, rfo_enable_gpios)
	struct device *rfo_enable;
#endif
#if DT_INST_NODE_HAS_PROP(0, pa_boost_enable_gpios)
	struct device *pa_boost_enable;
#endif
#if DT_INST_NODE_HAS_PROP(0, rfo_enable_gpios) &&	\
	DT_INST_NODE_HAS_PROP(0, pa_boost_enable_gpios)
	u8_t tx_power;
#endif
#if DT_INST_NODE_HAS_PROP(0, tcxo_power_gpios)
	struct device *tcxo_power;
	bool tcxo_power_enabled;
#endif
	struct device *spi;
	struct spi_config spi_cfg;
	struct device *dio_dev[SX1276_MAX_DIO];
	struct k_work dio_work[SX1276_MAX_DIO];
	struct k_sem data_sem;
	RadioEvents_t sx1276_event;
	u8_t *rx_buf;
	u8_t rx_len;
	s8_t snr;
	s16_t rssi;
} dev_data;

static s8_t clamp_s8(s8_t x, s8_t min, s8_t max)
{
	if (x < min) {
		return min;
	} else if (x > max) {
		return max;
	} else {
		return x;
	}
}

bool SX1276CheckRfFrequency(uint32_t frequency)
{
	/* TODO */
	return true;
}

void RtcStopAlarm(void)
{
	counter_stop(dev_data.counter);
}

static inline void sx1276_antenna_enable(int val)
{
#if DT_INST_NODE_HAS_PROP(0, antenna_enable_gpios)
	gpio_pin_set(dev_data.antenna_enable, GPIO_ANTENNA_ENABLE_PIN, val);
#endif
}

static inline void sx1276_rfi_enable(int val)
{
#if DT_INST_NODE_HAS_PROP(0, rfi_enable_gpios)
	gpio_pin_set(dev_data.rfi_enable, GPIO_RFI_ENABLE_PIN, val);
#endif
}

static inline void sx1276_rfo_enable(int val)
{
#if DT_INST_NODE_HAS_PROP(0, rfo_enable_gpios)
	gpio_pin_set(dev_data.rfo_enable, GPIO_RFO_ENABLE_PIN, val);
#endif
}

static inline void sx1276_pa_boost_enable(int val)
{
#if DT_INST_NODE_HAS_PROP(0, pa_boost_enable_gpios)
	gpio_pin_set(dev_data.pa_boost_enable,
		     GPIO_PA_BOOST_ENABLE_PIN, val);
#endif
}

void SX1276SetAntSwLowPower(bool low_power)
{
	if (low_power) {
		/* force inactive (low power) state of all antenna paths */
		sx1276_rfi_enable(0);
		sx1276_rfo_enable(0);
		sx1276_pa_boost_enable(0);

		sx1276_antenna_enable(0);
	} else {
		sx1276_antenna_enable(1);

		/* rely on SX1276SetAntSw() to configure proper antenna path */
	}
}

void SX1276SetBoardTcxo(u8_t state)
{
#if DT_INST_NODE_HAS_PROP(0, tcxo_power_gpios)
	bool enable = state;

	if (enable == dev_data.tcxo_power_enabled) {
		return;
	}

	if (enable) {
		gpio_pin_set(dev_data.tcxo_power, GPIO_TCXO_POWER_PIN, 1);

		if (TCXO_POWER_STARTUP_DELAY_MS > 0) {
			k_sleep(K_MSEC(TCXO_POWER_STARTUP_DELAY_MS));
		}
	} else {
		gpio_pin_set(dev_data.tcxo_power, GPIO_TCXO_POWER_PIN, 0);
	}

	dev_data.tcxo_power_enabled = enable;
#endif
}

void SX1276SetAntSw(u8_t opMode)
{
	switch (opMode) {
	case RFLR_OPMODE_TRANSMITTER:
		sx1276_rfi_enable(0);

		if (SX1276_PA_OUTPUT(dev_data.tx_power) == SX1276_PA_BOOST) {
			sx1276_rfo_enable(0);
			sx1276_pa_boost_enable(1);
		} else {
			sx1276_pa_boost_enable(0);
			sx1276_rfo_enable(1);
		}
		break;
	default:
		sx1276_rfo_enable(0);
		sx1276_pa_boost_enable(0);
		sx1276_rfi_enable(1);
		break;
	}
}

void SX1276Reset(void)
{
	SX1276SetBoardTcxo(true);

	gpio_pin_configure(dev_data.reset, GPIO_RESET_PIN,
			   GPIO_OUTPUT_ACTIVE | GPIO_RESET_FLAGS);

	k_sleep(K_MSEC(1));

	gpio_pin_set(dev_data.reset, GPIO_RESET_PIN, 0);

	k_sleep(K_MSEC(6));
}

void BoardCriticalSectionBegin(uint32_t *mask)
{
	*mask = irq_lock();
}

void BoardCriticalSectionEnd(uint32_t *mask)
{
	irq_unlock(*mask);
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

void DelayMsMcu(uint32_t ms)
{
	k_sleep(ms);
}

static void sx1276_dio_work_handle(struct k_work *work)
{
	int dio = work - dev_data.dio_work;

	(*DioIrq[dio])(NULL);
}

static void sx1276_irq_callback(struct device *dev,
				struct gpio_callback *cb, u32_t pins)
{
	unsigned int i, pin;

	pin = find_lsb_set(pins) - 1;

	for (i = 0; i < SX1276_MAX_DIO; i++) {
		if (dev == dev_data.dio_dev[i] &&
		    pin == sx1276_dios[i].pin) {
			k_work_submit(&dev_data.dio_work[i]);
		}
	}
}

void SX1276IoIrqInit(DioIrqHandler **irqHandlers)
{
	unsigned int i;
	static struct gpio_callback callbacks[SX1276_MAX_DIO];

	/* Setup DIO gpios */
	for (i = 0; i < SX1276_MAX_DIO; i++) {
		if (!irqHandlers[i]) {
			continue;
		}

		dev_data.dio_dev[i] = device_get_binding(sx1276_dios[i].port);
		if (dev_data.dio_dev[i] == NULL) {
			LOG_ERR("Cannot get pointer to %s device",
				sx1276_dios[i].port);
			return;
		}

		k_work_init(&dev_data.dio_work[i], sx1276_dio_work_handle);

		gpio_pin_configure(dev_data.dio_dev[i], sx1276_dios[i].pin,
				   GPIO_INPUT | GPIO_INT_DEBOUNCE
				   | sx1276_dios[i].flags);

		gpio_init_callback(&callbacks[i],
				   sx1276_irq_callback,
				   BIT(sx1276_dios[i].pin));

		if (gpio_add_callback(dev_data.dio_dev[i], &callbacks[i]) < 0) {
			LOG_ERR("Could not set gpio callback.");
			return;
		}
		gpio_pin_interrupt_configure(dev_data.dio_dev[i],
					     sx1276_dios[i].pin,
					     GPIO_INT_EDGE_TO_ACTIVE);
	}

}

static int sx1276_transceive(u8_t reg, bool write, void *data, size_t length)
{
	const struct spi_buf buf[2] = {
		{
			.buf = &reg,
			.len = sizeof(reg)
		},
		{
			.buf = data,
			.len = length
		}
	};
	struct spi_buf_set tx = {
		.buffers = buf,
		.count = ARRAY_SIZE(buf),
	};

	if (!write) {
		const struct spi_buf_set rx = {
			.buffers = buf,
			.count = ARRAY_SIZE(buf)
		};

		return spi_transceive(dev_data.spi, &dev_data.spi_cfg,
				&tx, &rx);
	}

	return spi_write(dev_data.spi, &dev_data.spi_cfg, &tx);
}

int sx1276_read(u8_t reg_addr, u8_t *data, u8_t len)
{
	return sx1276_transceive(reg_addr, false, data, len);
}

int sx1276_write(u8_t reg_addr, u8_t *data, u8_t len)
{
	return sx1276_transceive(reg_addr | BIT(7), true, data, len);
}

void SX1276WriteBuffer(u16_t addr, u8_t *buffer, u8_t size)
{
	int ret;

	ret = sx1276_write(addr, buffer, size);
	if (ret < 0) {
		LOG_ERR("Unable to write address: 0x%x", addr);
	}
}

void SX1276ReadBuffer(u16_t addr, u8_t *buffer, u8_t size)
{
	int ret;

	ret = sx1276_read(addr, buffer, size);
	if (ret < 0) {
		LOG_ERR("Unable to read address: 0x%x", addr);
	}
}

void SX1276SetRfTxPower(int8_t power)
{
	int ret;
	u8_t pa_config = 0;
	u8_t pa_dac = 0;

	ret = sx1276_read(SX1276_REG_PA_DAC, &pa_dac, 1);
	if (ret < 0) {
		LOG_ERR("Unable to read PA dac");
		return;
	}

	pa_dac &= RF_PADAC_20DBM_MASK;

	if (SX1276_PA_OUTPUT(power) == SX1276_PA_BOOST) {
		power = clamp_s8(power, 2, 20);

		pa_config |= RF_PACONFIG_PASELECT_PABOOST;
		if (power > 17) {
			pa_dac |= RF_PADAC_20DBM_ON;
			pa_config |= (power - 5) & 0x0F;
		} else {
			pa_dac |= RF_PADAC_20DBM_OFF;
			pa_config |= (power - 2) & 0x0F;
		}
	} else {
		power = clamp_s8(power, -4, 15);

		pa_dac |= RF_PADAC_20DBM_OFF;
		if (power > 0) {
			/* Set the power range to 0 -- 10.8+0.6*7 dBm. */
			pa_config |= 7 << SX1276_PA_CONFIG_MAX_POWER_SHIFT;
			pa_config |= (power & 0x0F);
		} else {
			/* Set the power range to -4.2 -- 10.8+0.6*0 dBm */
			pa_config |= 0 << SX1276_PA_CONFIG_MAX_POWER_SHIFT;
			pa_config |= ((power + 4) & 0x0F);
		}
	}

#if DT_INST_NODE_HAS_PROP(0, rfo_enable_gpios) &&	\
	DT_INST_NODE_HAS_PROP(0, pa_boost_enable_gpios)
	dev_data.tx_power = power;
#endif

	ret = sx1276_write(SX1276_REG_PA_CONFIG, &pa_config, 1);
	if (ret < 0) {
		LOG_ERR("Unable to write PA config");
		return;
	}

	ret = sx1276_write(SX1276_REG_PA_DAC, &pa_dac, 1);
	if (ret < 0) {
		LOG_ERR("Unable to write PA dac");
		return;
	}
}

static int sx1276_lora_send(struct device *dev, u8_t *data, u32_t data_len)
{
	Radio.SetMaxPayloadLength(MODEM_LORA, data_len);

	Radio.Send(data, data_len);

	return 0;
}

static void sx1276_tx_done(void)
{
	Radio.Sleep();
}

static void sx1276_rx_done(u8_t *payload, u16_t size, int16_t rssi, int8_t snr)
{
	Radio.Sleep();

	dev_data.rx_buf = payload;
	dev_data.rx_len = size;
	dev_data.rssi = rssi;
	dev_data.snr = snr;

	k_sem_give(&dev_data.data_sem);
}

static int sx1276_lora_recv(struct device *dev, u8_t *data, u8_t size,
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

static int sx1276_lora_config(struct device *dev,
			      struct lora_modem_config *config)
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

/* Initialize Radio driver callbacks */
const struct Radio_s Radio = {
	.Init = SX1276Init,
	.GetStatus = SX1276GetStatus,
	.SetModem = SX1276SetModem,
	.SetChannel = SX1276SetChannel,
	.IsChannelFree = SX1276IsChannelFree,
	.Random = SX1276Random,
	.SetRxConfig = SX1276SetRxConfig,
	.SetTxConfig = SX1276SetTxConfig,
	.Send = SX1276Send,
	.Sleep = SX1276SetSleep,
	.Standby = SX1276SetStby,
	.Rx = SX1276SetRx,
	.Write = SX1276Write,
	.Read = SX1276Read,
	.WriteBuffer = SX1276WriteBuffer,
	.ReadBuffer = SX1276ReadBuffer,
	.SetMaxPayloadLength = SX1276SetMaxPayloadLength,
	.IrqProcess = NULL,
	.RxBoosted = NULL,
	.SetRxDutyCycle = NULL,
};

static int sx1276_antenna_configure(void)
{
	int ret = 0;

#if DT_INST_NODE_HAS_PROP(0, antenna_enable_gpios)
	dev_data.antenna_enable = device_get_binding(
			DT_INST_GPIO_LABEL(0, antenna_enable_gpios));
	if (!dev_data.antenna_enable) {
		LOG_ERR("Cannot get pointer to %s device",
		       DT_INST_GPIO_LABEL(0, antenna_enable_gpios));
		return -EIO;
	}

	ret = gpio_pin_configure(dev_data.antenna_enable,
				 GPIO_ANTENNA_ENABLE_PIN,
				 GPIO_OUTPUT_INACTIVE |
					GPIO_ANTENNA_ENABLE_FLAGS);
#endif

#if DT_INST_NODE_HAS_PROP(0, rfi_enable_gpios)
	dev_data.rfi_enable = device_get_binding(
			DT_INST_GPIO_LABEL(0, rfi_enable_gpios));
	if (!dev_data.rfi_enable) {
		LOG_ERR("Cannot get pointer to %s device",
		       DT_INST_GPIO_LABEL(0, rfi_enable_gpios));
		return -EIO;
	}

	ret = gpio_pin_configure(dev_data.rfi_enable, GPIO_RFI_ENABLE_PIN,
				 GPIO_OUTPUT_INACTIVE | GPIO_RFI_ENABLE_FLAGS);
#endif

#if DT_INST_NODE_HAS_PROP(0, rfo_enable_gpios)
	dev_data.rfo_enable = device_get_binding(
			DT_INST_GPIO_LABEL(0, rfo_enable_gpios));
	if (!dev_data.rfo_enable) {
		LOG_ERR("Cannot get pointer to %s device",
		       DT_INST_GPIO_LABEL(0, rfo_enable_gpios));
		return -EIO;
	}

	ret = gpio_pin_configure(dev_data.rfo_enable, GPIO_RFO_ENABLE_PIN,
				 GPIO_OUTPUT_INACTIVE | GPIO_RFO_ENABLE_FLAGS);
#endif

#if DT_INST_NODE_HAS_PROP(0, pa_boost_enable_gpios)
	dev_data.pa_boost_enable = device_get_binding(
			DT_INST_GPIO_LABEL(0, pa_boost_enable_gpios));
	if (!dev_data.pa_boost_enable) {
		LOG_ERR("Cannot get pointer to %s device",
		       DT_INST_GPIO_LABEL(0, pa_boost_enable_gpios));
		return -EIO;
	}

	ret = gpio_pin_configure(dev_data.pa_boost_enable,
				 GPIO_PA_BOOST_ENABLE_PIN,
				 GPIO_OUTPUT_INACTIVE |
					GPIO_PA_BOOST_ENABLE_FLAGS);
#endif

	return ret;
}

static int sx1276_lora_init(struct device *dev)
{
	static struct spi_cs_control spi_cs;
	int ret;
	u8_t regval;

	dev_data.spi = device_get_binding(DT_INST_BUS_LABEL(0));
	if (!dev_data.spi) {
		LOG_ERR("Cannot get pointer to %s device",
			    DT_INST_BUS_LABEL(0));
		return -EINVAL;
	}

	dev_data.spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB;
	dev_data.spi_cfg.frequency = DT_INST_PROP(0, spi_max_frequency);
	dev_data.spi_cfg.slave = DT_INST_REG_ADDR(0);

	spi_cs.gpio_pin = GPIO_CS_PIN,
	spi_cs.gpio_dev = device_get_binding(
			DT_INST_SPI_DEV_CS_GPIOS_LABEL(0));
	if (!spi_cs.gpio_dev) {
		LOG_ERR("Cannot get pointer to %s device",
		       DT_INST_SPI_DEV_CS_GPIOS_LABEL(0));
		return -EIO;
	}

	dev_data.spi_cfg.cs = &spi_cs;

#if DT_INST_NODE_HAS_PROP(0, tcxo_power_gpios)
	dev_data.tcxo_power = device_get_binding(
			DT_INST_GPIO_LABEL(0, tcxo_power_gpios));
	if (!dev_data.tcxo_power) {
		LOG_ERR("Cannot get pointer to %s device",
		       DT_INST_GPIO_LABEL(0, tcxo_power_gpios));
		return -EIO;
	}

	gpio_pin_configure(dev_data.tcxo_power, GPIO_TCXO_POWER_PIN,
			   GPIO_OUTPUT_INACTIVE | GPIO_TCXO_POWER_FLAGS);
#endif

	/* Setup Reset gpio */
	dev_data.reset = device_get_binding(
			DT_INST_GPIO_LABEL(0, reset_gpios));
	if (!dev_data.reset) {
		LOG_ERR("Cannot get pointer to %s device",
		       DT_INST_GPIO_LABEL(0, reset_gpios));
		return -EIO;
	}

	/* Perform soft reset */
	ret = gpio_pin_configure(dev_data.reset, GPIO_RESET_PIN,
				 GPIO_OUTPUT_ACTIVE | GPIO_RESET_FLAGS);

	k_sleep(K_MSEC(100));
	gpio_pin_set(dev_data.reset, GPIO_RESET_PIN, 0);
	k_sleep(K_MSEC(100));

	ret = sx1276_read(SX1276_REG_VERSION, &regval, 1);
	if (ret < 0) {
		LOG_ERR("Unable to read version info");
		return -EIO;
	}

	ret = sx1276_antenna_configure();
	if (ret < 0) {
		LOG_ERR("Unable to configure antenna");
		return -EIO;
	}

	dev_data.counter = device_get_binding(DT_RTC_0_NAME);
	if (!dev_data.counter) {
		LOG_ERR("Cannot get pointer to %s device", DT_RTC_0_NAME);
		return -EIO;
	}

	k_sem_init(&dev_data.data_sem, 0, UINT_MAX);

	dev_data.sx1276_event.TxDone = sx1276_tx_done;
	dev_data.sx1276_event.RxDone = sx1276_rx_done;
	Radio.Init(&dev_data.sx1276_event);

	LOG_INF("SX1276 Version:%02x found", regval);

	return 0;
}

static const struct lora_driver_api sx1276_lora_api = {
	.config = sx1276_lora_config,
	.send = sx1276_lora_send,
	.recv = sx1276_lora_recv,
};

DEVICE_AND_API_INIT(sx1276_lora, DT_INST_LABEL(0),
		    &sx1276_lora_init, NULL,
		    NULL, POST_KERNEL, CONFIG_LORA_INIT_PRIORITY,
		    &sx1276_lora_api);
