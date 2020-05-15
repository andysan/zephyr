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
	u32_t system_max_rx_error;

	/** Maximum number of re-transmissions for confirmed packets. */
	unsigned int send_retries;
};

typedef unsigned int lorawan_send_flags_t;

/** Request a an acknowledgement from the server */
#define LW_SEND_CONFIRMED BIT(0)

/** Listen for packets on any port */
#define LW_RECV_PORT_ANY 0

/**
 * @brief LoRaWAN join parameters for over-the-Air activation (OTAA)
 *
 * Note that all of the fields use LoRaWAN 1.1 terminology.
 *
 * All parameters are optional if a secure element is present in which
 * case the values stored in the secure element will be used instead.
 */
struct lorawan_join_otaa {
	u8_t *join_eui;
	u8_t *nwk_key;
	u8_t *app_key;
};

struct lorawan_join_abp10 {
	/** Device address on the network */
	u32_t dev_addr;
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

#define LORAWAN_BATTERY_UNKNOWN		0xff
#define LORAWAN_BATTERY_EXTERNAL	0x00
#define LORAWAN_BATTERY_MIN		0x01
#define LORAWAN_BATTERY_MAX		0xfe

struct lorawan_callbacks {
	/**
	 * @brief Return the current battery level of the node
	 *
	 * The MAC can inform the network of a node's battery
	 * status. To enable this feature. register this callback and
	 * return a value between LORAWAN_BATTER_MIN and
	 * LORAWAN_BATTERY_MAX or LORAWAN_BATTERY_EXTERNAL if on
	 * external power.
	 *
	 * This callback may be left as NULL in which case the battery
	 * level will be treated as unknown.
	 *
	 * @return Current battery level, LORAWAN_BATTERY_UNKNOWN if
	 * unknown.
	 */
	u8_t (*get_battery_level)();
};

/**
 * @brief Setup callback handlers
 *
 * @param cbs Callback structure
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_set_callbacks(const struct lorawan_callbacks *cbs);

/**
 * @brief Configure the LoRaWAN stack
 *
 * Configure the LoRaWAN stack using MIB (Mac Information Base) parameters.
 *
 * @param config MIB configuration
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_config(const struct lorawan_config *config);

/**
 * @brief Start the stack
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_start();

/**
 * @brief Restore connection from persistent storage
 *
 * If the stack has been built with support for the settings
 * subsystem, enough state to restore a connection without a full join
 * request will be stored to persistent storage. Call this function to
 * re-establish a connection, if the function fails a full join may be
 * required.
 *
 * If this function fails, the stack will have to be started using the
 * lorawan_start() call.
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_resume();

/**
 * @brief Stop the stack and store the persistent state
 *
 * If the stack has been built with support for the settings
 * subsystem, enough state to restore a connection without a full join
 * request will be stored to persistent storage. Call this function to
 * stop the stack and store the state.
 *
 * @return 0 if successful, negative errno code if failure
 */
int lorawan_suspend();

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
int lorawan_set_class(enum lorawan_class dev_class);

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
int lorawan_set_datarate(enum lorawan_datarate dr, bool adr);

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
int lorawan_send(u8_t port, void *data, size_t len, lorawan_send_flags_t flags);

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
