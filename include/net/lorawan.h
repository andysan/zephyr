/*
 * Copyright (c) 2019 Manivannan Sadhasivam <mani@kernel.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_NET_LORAWAN_H_
#define ZEPHYR_INCLUDE_NET_LORAWAN_H_

/**
 * @file
 * @brief Public LoRaWAN APIs
 */

#include <zephyr/types.h>
#include <device.h>

/**
 * @brief LoRaWAN class types.
 */
enum lorawan_class {
	LORAWAN_CLASS_A = 0x00,
	LORAWAN_CLASS_B = 0x01,
	LORAWAN_CLASS_C = 0x02,
};

/**
 * @brief LoRaWAN activation types.
 */
enum lorawan_act_type {
	LORAWAN_ACT_OTAA = 0,
	LORAWAN_ACT_ABP10,
};

/**
 * @brief LoRaWAN datarate types.
 */
enum lorawan_datarate {
	LORAWAN_DR_0 = 0,
	LORAWAN_DR_1,
	LORAWAN_DR_2,
	LORAWAN_DR_3,
	LORAWAN_DR_4,
	LORAWAN_DR_5,
	LORAWAN_DR_6,
	LORAWAN_DR_7,
	LORAWAN_DR_8,
	LORAWAN_DR_9,
	LORAWAN_DR_10,
	LORAWAN_DR_11,
	LORAWAN_DR_12,
	LORAWAN_DR_13,
	LORAWAN_DR_14,
	LORAWAN_DR_15,
};

struct lorawan_config {
	u32_t system_max_rs_error;

	/** Maximum number of re-transmissions for confirmed packets. */
	unsigned int send_retries;
};

/** Request a an acknowledgement from the server */
#define LW_SEND_CONFIRMED BIT(0)

/** Listen for packets on any port */
#define LW_RECV_PORT_ANY 0

/**
 * @brief LoRaWAN join parameters for over-the-Air activation (OTAA)
 *
 * All parameters are optional if a secure element is present in which
 * case the values stored in the secure element will be used instead.
 */
struct lorawan_join_otaa {
	u8_t *app_key;
	u8_t *nwk_key;
	u8_t *join_eui;
};

struct lorawan_join_abp10 {
	/** Device address on the network */
	u8_t dev_addr[3];
	/** Application session key */
	u8_t *app_skey;
	/** Network session key */
	u8_t *nwk_skey;
	/** Application EUI */
	u8_t *app_eui;
};

struct lorawan_join_config {
	union {
		struct lorawan_join_otaa otaa;
		struct lorawan_join_abp10 abp10;
	};

	/** Device EUI. Optional if a secure element is present. */
	u8_t *dev_eui;

	enum lorawan_act_type mode;
};

/**
 * Callback to handle received packets.
 */
typedef void (*lorawan_recv_callback_t)(u8_t port, const void *data,
					size_t len);

/**
 * @brief Configure the LoRaWAN stack
 *
 * Configure the LoRaWAN stack using MIB (Mac Information Base) parameters.
 *
 * @param mib_config MIB configuration
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_config(struct lorawan_config *config);

/**
 * @brief Restore connection from persistent storage
 *
 * If the stack has been built with support for the settings
 * subsystem, enough state to restore a connection without a full join
 * request will be stored to persistent storage. Call this function to
 * re-establish a connection, if the function fails a full join may be
 * required.
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_restore_connection();

/**
 * @brief Join the LoRaWAN network
 *
 * Join the LoRaWAN network using either OTAA or AWB.
 *
 * @param datarate   Datarate to be used for network join
 * @param mode       Activation mode
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_join_network(const struct lorawan_join_config *join_req);

/**
 * @brief Set the current device class
 *
 * Change the current device class. This function may be called before
 * or after a network connection has been established.
 *
 * @param dev_class New device class
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_set_class(lorawan_class dev_class);

/**
 * @brief Set the default data rate
 *
 * Change the default data rate and control whether adaptive data rate
 * (ADR) is enabled. When ADR is enabled, the data rate is treated as
 * a default data rate that wil be used if the ADR algorithm has not
 * established a data rate. ADR should normally only be enabled for
 * devices with stable RF conditions (i.e., devices in a mostly static
 * location).
 *
 * @param dr Data rate for transmiattions, or default data rate if ADR
 *           is anabled.
 * @param adr Enable adaptive data rate if true.
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_set_data_rate(lorawan_datarate dr, bool adr);

/**
 * @brief Send data to the LoRaWAN network
 *
 * Send data to the connected LoRaWAN network.
 *
 * @param port       Port to be used for sending data. Must be set if the
 *                   payload is not empty.
 * @param data       Data buffer to be sent
 * @param len        Length of the buffer to be sent. Maximum length of this
 *                   buffer is 255 bytes but the actual payload size varies with
 *                   region and datarate.
 * @param flags      Flags controlling transmission options.
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_send(u8_t port, u8_t *data, u8_t len, unsigned int flags);

/**
 * @brief Register a data handler for a specific port
 *
 * Register a callback for packets received on a specific port. Only
 * one callback can be attached per port. Calling this function twice
 * for the same port replaces the previous listener.
 *
 * The port may be specified as LW_RECV_PORT_ANY to receive packets on
 * any port that does not have a port-specific listener.
 *
 * @param port       Port or LW_RECV_PORT_ANY to listen to any port.
 * @param cb         Function to call when a packet has been received, NULL
 *                   to disable an existing listener.
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_listen(u8_t port, lorawan_recv_callback_t cb);

#endif	/* ZEPHYR_INCLUDE_NET_LORAWAN_H_ */
