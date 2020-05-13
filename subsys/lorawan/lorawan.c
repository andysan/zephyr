/*
 * Copyright (c) 2019 Manivannan Sadhasivam <mani@kernel.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <init.h>
#include <errno.h>
#include <net/lorawan.h>
#include <zephyr.h>

#include <LoRaMac.h>

#ifdef CONFIG_LORAMAC_REGION_AS923
	#define LORAWAN_REGION LORAMAC_REGION_AS923
#elif CONFIG_LORAMAC_REGION_AU915
	#define LORAWAN_REGION LORAMAC_REGION_AU915
#elif CONFIG_LORAMAC_REGION_CN470
	#define LORAWAN_REGION LORAMAC_REGION_CN470
#elif CONFIG_LORAMAC_REGION_CN779
	#define LORAWAN_REGION LORAMAC_REGION_CN779
#elif CONFIG_LORAMAC_REGION_EU433
	#define LORAWAN_REGION LORAMAC_REGION_EU433
#elif CONFIG_LORAMAC_REGION_EU868
	#define LORAWAN_REGION LORAMAC_REGION_EU868
#elif CONFIG_LORAMAC_REGION_KR920
	#define LORAWAN_REGION LORAMAC_REGION_KR920
#elif CONFIG_LORAMAC_REGION_IN865
	#define LORAWAN_REGION LORAMAC_REGION_IN865
#elif CONFIG_LORAMAC_REGION_US915
	#define LORAWAN_REGION LORAMAC_REGION_US915
#elif CONFIG_LORAMAC_REGION_RU864
	#define LORAWAN_REGION LORAMAC_REGION_RU864
#elif
	#error "Atleast one LoRaWAN region should be selected"
#endif

#define LORAWAN_PKT_MAX_LEN 0xff

/* Use version 1.0.3.0 for ABP */
#define LORAWAN_ABP10_VERSION 0x01000300

#define MIB_SET_OR_RETURN(req)					\
	do {							\
		LoRaMacStatus_t status;				\
		status = LoRaMacMibSetRequestConfirm((req));	\
		if (status != LORAMAC_STATUS_OK) {		\
			return status;				\
		}						\
	} while(0)

#include <logging/log.h>
LOG_MODULE_REGISTER(lorawan, CONFIG_LORAWAN_LOG_LEVEL);

K_SEM_DEFINE(mlme_confirm_sem, 0, 1);
K_SEM_DEFINE(mcps_confirm_sem, 0, 1);

K_MUTEX_DEFINE(lorawan_join_mutex);
K_MUTEX_DEFINE(lorawan_send_mutex);

static enum lorawan_datarate lorawan_datarate = LORAWAN_DR_0;
static unsigned int lorawan_send_tries = 4;

const char *status2str(int status)
{
	switch (status) {
	case LORAMAC_STATUS_OK:
		return "OK";
	case LORAMAC_STATUS_BUSY:
		return "Busy";
	case LORAMAC_STATUS_SERVICE_UNKNOWN:
		return "Service unknown";
	case LORAMAC_STATUS_PARAMETER_INVALID:
		return "Parameter invalid";
	case LORAMAC_STATUS_FREQUENCY_INVALID:
		return "Frequency invalid";
	case LORAMAC_STATUS_DATARATE_INVALID:
		return "Datarate invalid";
	case LORAMAC_STATUS_FREQ_AND_DR_INVALID:
		return "Frequency or datarate invalid";
	case LORAMAC_STATUS_NO_NETWORK_JOINED:
		return "No network joined";
	case LORAMAC_STATUS_LENGTH_ERROR:
		return "Length error";
	case LORAMAC_STATUS_REGION_NOT_SUPPORTED:
		return "Region not supported";
	case LORAMAC_STATUS_SKIPPED_APP_DATA:
		return "Skipped APP data";
	case LORAMAC_STATUS_DUTYCYCLE_RESTRICTED:
		return "Duty-cycle restricted";
	case LORAMAC_STATUS_NO_CHANNEL_FOUND:
		return "No channel found";
	case LORAMAC_STATUS_NO_FREE_CHANNEL_FOUND:
		return "No free channel found";
	case LORAMAC_STATUS_BUSY_BEACON_RESERVED_TIME:
		return "Busy beacon reserved time";
	case LORAMAC_STATUS_BUSY_PING_SLOT_WINDOW_TIME:
		return "Busy ping-slot window time";
	case LORAMAC_STATUS_BUSY_UPLINK_COLLISION:
		return "Busy uplink collision";
	case LORAMAC_STATUS_CRYPTO_ERROR:
		return "Crypto error";
	case LORAMAC_STATUS_FCNT_HANDLER_ERROR:
		return "FCnt handler error";
	case LORAMAC_STATUS_MAC_COMMAD_ERROR:
		return "MAC command error";
	case LORAMAC_STATUS_CLASS_B_ERROR:
		return "ClassB error";
	case LORAMAC_STATUS_CONFIRM_QUEUE_ERROR:
		return "Confirm queue error";
	case LORAMAC_STATUS_MC_GROUP_UNDEFINED:
		return "Multicast group undefined";
	case LORAMAC_STATUS_ERROR:
		return "Unknown error";
	default:
		return NULL;
	}
}

const char *eventinfo2str(int status)
{
	switch (status) {
	case LORAMAC_EVENT_INFO_STATUS_OK:
		return "OK";
	case LORAMAC_EVENT_INFO_STATUS_ERROR:
		return "Error";
	case LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT:
		return "Tx timeout";
	case LORAMAC_EVENT_INFO_STATUS_RX1_TIMEOUT:
		return "Rx 1 timeout";
	case LORAMAC_EVENT_INFO_STATUS_RX2_TIMEOUT:
		return "Rx 2 timeout";
	case LORAMAC_EVENT_INFO_STATUS_RX1_ERROR:
		return "Rx1 error";
	case LORAMAC_EVENT_INFO_STATUS_RX2_ERROR:
		return "Rx2 error";
	case LORAMAC_EVENT_INFO_STATUS_JOIN_FAIL:
		return "Join failed";
	case LORAMAC_EVENT_INFO_STATUS_DOWNLINK_REPEATED:
		return "Downlink repeated";
	case LORAMAC_EVENT_INFO_STATUS_TX_DR_PAYLOAD_SIZE_ERROR:
		return "Tx DR payload size error";
	case LORAMAC_EVENT_INFO_STATUS_DOWNLINK_TOO_MANY_FRAMES_LOSS:
		return "Downlink too many frames loss";
	case LORAMAC_EVENT_INFO_STATUS_ADDRESS_FAIL:
		return "Address fail";
	case LORAMAC_EVENT_INFO_STATUS_MIC_FAIL:
		return "MIC fail";
	case LORAMAC_EVENT_INFO_STATUS_MULTICAST_FAIL:
		return "Multicast fail";
	case LORAMAC_EVENT_INFO_STATUS_BEACON_LOCKED:
		return "Beacon locked";
	case LORAMAC_EVENT_INFO_STATUS_BEACON_LOST:
		return "Beacon lost";
	case LORAMAC_EVENT_INFO_STATUS_BEACON_NOT_FOUND:
		return "Beacon not found";
	default:
		return NULL;
	}
}

/*
 * MAC status and Event status to Zephyr error code conversion.
 * Direct mapping is not possible as statuses often indicate the domain from
 * which the error originated rather than its cause or meaning. -EINVAL has been
 * used as a general error code because those usually result from incorrect
 * configuration.
 */
const int mac_status_to_errno[] = {
	0,			/* LORAMAC_STATUS_OK */
	-EBUSY,			/* LORAMAC_STATUS_BUSY */
	-ENOPROTOOPT,		/* LORAMAC_STATUS_SERVICE_UNKNOWN */
	-EINVAL,		/* LORAMAC_STATUS_PARAMETER_INVALID */
	-EINVAL,		/* LORAMAC_STATUS_FREQUENCY_INVALID */
	-EINVAL,		/* LORAMAC_STATUS_DATARATE_INVALID */
	-EINVAL,		/* LORAMAC_STATUS_FREQ_AND_DR_INVALID */
	-ENOTCONN,		/* LORAMAC_STATUS_NO_NETWORK_JOINED */
	-EMSGSIZE,		/* LORAMAC_STATUS_LENGTH_ERROR */
	-EPFNOSUPPORT,		/* LORAMAC_STATUS_REGION_NOT_SUPPORTED */
	-EMSGSIZE,		/* LORAMAC_STATUS_SKIPPED_APP_DATA */
	-ECONNREFUSED,		/* LORAMAC_STATUS_DUTYCYCLE_RESTRICTED */
	-ENOTCONN,		/* LORAMAC_STATUS_NO_CHANNEL_FOUND */
	-ENOTCONN,		/* LORAMAC_STATUS_NO_FREE_CHANNEL_FOUND */
	-EBUSY,			/* LORAMAC_STATUS_BUSY_BEACON_RESERVED_TIME */
	-EBUSY,			/* LORAMAC_STATUS_BUSY_PING_SLOT_WINDOW_TIME */
	-EBUSY,			/* LORAMAC_STATUS_BUSY_UPLINK_COLLISION */
	-EINVAL,		/* LORAMAC_STATUS_CRYPTO_ERROR */
	-EINVAL,		/* LORAMAC_STATUS_FCNT_HANDLER_ERROR */
	-EINVAL,		/* LORAMAC_STATUS_MAC_COMMAD_ERROR */
	-EINVAL,		/* LORAMAC_STATUS_CLASS_B_ERROR */
	-EINVAL,		/* LORAMAC_STATUS_CONFIRM_QUEUE_ERROR */
	-EINVAL			/* LORAMAC_STATUS_MC_GROUP_UNDEFINED */
};

const int mac_event_info_to_errno[] = {
	0,			/* LORAMAC_EVENT_INFO_STATUS_OK */
	-EINVAL,		/* LORAMAC_EVENT_INFO_STATUS_ERROR */
	-ETIMEDOUT,		/* LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT */
	-ETIMEDOUT,		/* LORAMAC_EVENT_INFO_STATUS_RX1_TIMEOUT */
	-ETIMEDOUT,		/* LORAMAC_EVENT_INFO_STATUS_RX2_TIMEOUT */
	-EINVAL,		/* LORAMAC_EVENT_INFO_STATUS_RX1_ERROR */
	-EINVAL,		/* LORAMAC_EVENT_INFO_STATUS_RX2_ERROR */
	-EINVAL,		/* LORAMAC_EVENT_INFO_STATUS_JOIN_FAIL */
	-ECONNRESET,		/* LORAMAC_EVENT_INFO_STATUS_DOWNLINK_REPEATED */
	-EMSGSIZE,		/* LORAMAC_EVENT_INFO_STATUS_TX_DR_PAYLOAD_SIZE_ERROR */
	-ECONNRESET,		/* LORAMAC_EVENT_INFO_STATUS_DOWNLINK_TOO_MANY_FRAMES_LOSS */
	-EACCES,		/* LORAMAC_EVENT_INFO_STATUS_ADDRESS_FAIL */
	-EACCES,		/* LORAMAC_EVENT_INFO_STATUS_MIC_FAIL */
	-EINVAL,		/* LORAMAC_EVENT_INFO_STATUS_MULTICAST_FAIL */
	-EINVAL,		/* LORAMAC_EVENT_INFO_STATUS_BEACON_LOCKED */
	-EINVAL,		/* LORAMAC_EVENT_INFO_STATUS_BEACON_LOST */
	-EINVAL			/* LORAMAC_EVENT_INFO_STATUS_BEACON_NOT_FOUND */
};

static LoRaMacPrimitives_t macPrimitives;
static LoRaMacCallback_t macCallbacks;

static LoRaMacEventInfoStatus_t last_mcps_confirm_status;
static LoRaMacEventInfoStatus_t last_mlme_confirm_status;
static LoRaMacEventInfoStatus_t last_mcps_indication_status;
static LoRaMacEventInfoStatus_t last_mlme_indication_status;

static lorawan_recv_callback_t default_listener = NULL;

#define LW_RECV_PORT_ILLEGAL LW_RECV_PORT_ANY

struct lorawan_port_listener {
	u8_t port;
	lorawan_recv_callback_t cb;
};

static struct lorawan_port_listener port_listener[
	CONFIG_LORAWAN_MAX_LISTENERS] = {};

static struct lorawan_port_listener *get_listener(u8_t port)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(port_listener); i++) {
		if (port_listener[i].port == port) {
			return &port_listener[i];
		}
	}

	return NULL;
}

static void notify_listeners(u8_t port, void *buf, size_t size)
{
	struct lorawan_port_listener *l = get_listener(port);

	if (l && l->cb) {
		l->cb(port, buf, size);
	} else if (default_listener) {
		LOG_DBG("No listener for port %i, using default.", (int)port);
		default_listener(port, buf, size);
	} else {
		LOG_WRN("No listener registered for port %i", (int)port);
	}
}

static void OnMacProcessNotify(void)
{
	LoRaMacProcess();
}

static void McpsConfirm(McpsConfirm_t *mcpsConfirm)
{
	LOG_DBG("Received McpsConfirm (for McpsRequest %d)",
		mcpsConfirm->McpsRequest);

	if (mcpsConfirm->Status != LORAMAC_EVENT_INFO_STATUS_OK) {
		LOG_ERR("McpsRequest failed : %s",
			log_strdup(eventinfo2str(mcpsConfirm->Status)));
	} else {
		LOG_DBG("McpsRequest success!");
	}

	last_mcps_confirm_status = mcpsConfirm->Status;
	k_sem_give(&mcps_confirm_sem);
}

static void McpsIndication(McpsIndication_t *mcpsIndication)
{
	LOG_DBG("Received McpsIndication %d", mcpsIndication->McpsIndication);

	if (mcpsIndication->Status != LORAMAC_EVENT_INFO_STATUS_OK) {
		LOG_ERR("McpsIndication failed : %s",
			log_strdup(eventinfo2str(mcpsIndication->Status)));
		return;
	}

	/* TODO: Check MCPS Indication type */
	if (mcpsIndication->RxData == true) {
		LOG_DBG("Rx Data on port %i", (int)mcpsIndication->Port);
		LOG_HEXDUMP_DBG(mcpsIndication->Buffer,
				mcpsIndication->BufferSize, "Data");
		notify_listeners(mcpsIndication->Port, mcpsIndication->Buffer,
				 mcpsIndication->BufferSize);
	}

	last_mcps_indication_status = mcpsIndication->Status;

	/* TODO: Compliance test based on FPort value*/
}

static void MlmeConfirm(MlmeConfirm_t *mlmeConfirm)
{
	MibRequestConfirm_t mibGet;

	LOG_DBG("Received MlmeConfirm (for MlmeRequest %d)",
		mlmeConfirm->MlmeRequest);

	if (mlmeConfirm->Status != LORAMAC_EVENT_INFO_STATUS_OK) {
		LOG_ERR("MlmeConfirm failed : %s",
			log_strdup(eventinfo2str(mlmeConfirm->Status)));
		goto out_sem;
	}

	switch (mlmeConfirm->MlmeRequest) {
	case MLME_JOIN:
		mibGet.Type = MIB_DEV_ADDR;
		LoRaMacMibGetRequestConfirm(&mibGet);
		LOG_INF("Joined network! DevAddr: %08x", mibGet.Param.DevAddr);
		break;
	case MLME_LINK_CHECK:
		/* Not implemented */
		break;
	default:
		break;
	}

out_sem:
	last_mlme_confirm_status = mlmeConfirm->Status;
	k_sem_give(&mlme_confirm_sem);
}

static void MlmeIndication(MlmeIndication_t *mlmeIndication)
{
	LOG_DBG("Received MlmeIndication %d", mlmeIndication->MlmeIndication);
	last_mlme_indication_status = mlmeIndication->Status;
}

int lorawan_config(const struct lorawan_config *config)
{
	MibRequestConfirm_t mibReq;

	mibReq.Type = MIB_SYSTEM_MAX_RX_ERROR;
	mibReq.Param.SystemMaxRxError = config->system_max_rs_error;
	LoRaMacMibSetRequestConfirm(&mibReq);

	lorawan_send_tries = config->send_retries;

	return 0;
}

int lorawan_restore_connection()
{
	/* TODO: Unimplemented */
	return -ENOENT;
}


static LoRaMacStatus_t lorawan_join_otaa(
	const struct lorawan_join_config *join)
{
	MlmeReq_t mlme_join = {
		.Type = MLME_JOIN,
		.Req.Join.Datarate = lorawan_datarate,
	};

	if (join->dev_eui) {
		MibRequestConfirm_t req = {
			.Type = MIB_DEV_EUI,
			.Param.DevEui = join->dev_eui,
		};
		MIB_SET_OR_RETURN(&req);
	}

	if (join->otaa.join_eui) {
		MibRequestConfirm_t req = {
			.Type = MIB_JOIN_EUI,
			.Param.JoinEui = join->otaa.join_eui,
		};
		MIB_SET_OR_RETURN(&req);
	}

	if (join->otaa.nwk_key) {
		MibRequestConfirm_t req = {
			.Type = MIB_NWK_KEY,
			.Param.NwkKey = join->otaa.nwk_key,
		};
		MIB_SET_OR_RETURN(&req);
	}

	if (join->otaa.app_key) {
		MibRequestConfirm_t req = {
			.Type = MIB_APP_KEY,
			.Param.NwkKey = join->otaa.app_key,
		};
		MIB_SET_OR_RETURN(&req);
	}

	return LoRaMacMlmeRequest(&mlme_join);
}

static LoRaMacStatus_t lorawan_join_abp10(
	const struct lorawan_join_config *join)
{
	MibRequestConfirm_t req;

	req.Type = MIB_ABP_LORAWAN_VERSION;
	req.Param.AbpLrWanVersion.Value = LORAWAN_ABP10_VERSION;
	MIB_SET_OR_RETURN(&req);

	req.Type = MIB_NET_ID;
	req.Param.NetID = 0;
	MIB_SET_OR_RETURN(&req);

	req.Type = MIB_DEV_ADDR;
	req.Param.DevAddr = join->abp10.dev_addr;
	MIB_SET_OR_RETURN(&req);

	req.Type = MIB_F_NWK_S_INT_KEY;
	req.Param.FNwkSIntKey = join->abp10.nwk_skey;
	MIB_SET_OR_RETURN(&req);

	req.Type = MIB_S_NWK_S_INT_KEY;
	req.Param.SNwkSIntKey = join->abp10.nwk_skey;
	MIB_SET_OR_RETURN(&req);

	req.Type = MIB_NWK_S_ENC_KEY;
	req.Param.NwkSEncKey = join->abp10.nwk_skey;
	MIB_SET_OR_RETURN(&req);

	req.Type = MIB_APP_S_KEY;
	req.Param.AppSKey = join->abp10.app_skey;
	MIB_SET_OR_RETURN(&req);

	req.Type = MIB_NETWORK_ACTIVATION;
	req.Param.NetworkActivation = ACTIVATION_TYPE_ABP;
	MIB_SET_OR_RETURN(&req);

	return LORAMAC_STATUS_OK;
}

int lorawan_join_network(const struct lorawan_join_config *join_req)
{
	LoRaMacStatus_t status;
	int ret = 0;

	k_mutex_lock(&lorawan_join_mutex, K_FOREVER);

	if (join_req->mode == LORAWAN_ACT_OTAA) {
		status = lorawan_join_otaa(join_req);
		if (status != LORAMAC_STATUS_OK) {
			LOG_ERR("OTAA join failed: %s",
				log_strdup(status2str(status)));
			ret = mac_status_to_errno[status];
			goto out;
		}

		LOG_DBG("Network join request sent!");

		/*
		 * We can be sure that the semaphore will be released for
		 * both success and failure cases after a specific time period.
		 * So we can use K_FOREVER and no need to check the return val.
		 */
		k_sem_take(&mlme_confirm_sem, K_FOREVER);
		if (last_mlme_confirm_status != LORAMAC_EVENT_INFO_STATUS_OK) {
			ret = mac_event_info_to_errno[last_mlme_confirm_status];
			goto out;
		}
	} else if (join_req->mode == LORAWAN_ACT_ABP10) {
		status = lorawan_join_abp10(join_req);
		if (status != LORAMAC_STATUS_OK) {
			LOG_ERR("ABP join failed: %s",
				log_strdup(status2str(status)));
			ret = mac_status_to_errno[status];
			goto out;
		}
	} else {
		ret = -EINVAL;
	}

out:
	k_mutex_unlock(&lorawan_join_mutex);
	return ret;
}

int lorawan_set_class(enum lorawan_class dev_class)
{
	LoRaMacStatus_t status;
	MibRequestConfirm_t req = {
		.Type = MIB_DEVICE_CLASS,
	};

	switch (dev_class) {
	case LORAWAN_CLASS_A:
		req.Param.Class = CLASS_A;
		break;
	case LORAWAN_CLASS_C:
		req.Param.Class = CLASS_C;
		break;
	default:
		return -EINVAL;
	};

	status = LoRaMacMibSetRequestConfirm(&req);
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("Failed to set device class: %s",
			status2str(status));
		return mac_status_to_errno[status];
	}

	return 0;
}

int lorawan_set_datarate(enum lorawan_datarate dr, bool adr)
{
	MibRequestConfirm_t req = {
		.Type = MIB_ADR,
		.Param.AdrEnable = adr,
	};

	if (LoRaMacMibSetRequestConfirm(&req) != LORAMAC_STATUS_OK) {
		return -EFAULT;
	}

	lorawan_datarate = dr;

	return 0;
}

int lorawan_send(u8_t port, void *data, size_t len, lorawan_send_flags_t flags)
{
	LoRaMacStatus_t status;
	McpsReq_t mcpsReq;
	LoRaMacTxInfo_t txInfo;
	int ret = 0;
	bool empty_frame = false;

	if (data == NULL || len > LORAWAN_PKT_MAX_LEN) {
		return -EINVAL;
	}

	k_mutex_lock(&lorawan_send_mutex, K_FOREVER);

	status = LoRaMacQueryTxPossible(len, &txInfo);
	if (status != LORAMAC_STATUS_OK) {
		/*
		 * If this returns false, then most likely the payload has
		 * exceeded the maximum possible length for the current region
		 * and datarate. We can't do much other than sending empty
		 * frame in order to flush MAC commands in stack and hoping the
		 * application to lower the payload size for next try.
		 */
		LOG_ERR("LoRaWAN Query Tx Possible Failed: %s",
			log_strdup(status2str(status)));
		empty_frame = true;
		mcpsReq.Type = MCPS_UNCONFIRMED;
		mcpsReq.Req.Unconfirmed.fBuffer = NULL;
		mcpsReq.Req.Unconfirmed.fBufferSize = 0;
		mcpsReq.Req.Unconfirmed.Datarate = lorawan_datarate;
	} else {
		if (!(flags & LW_SEND_CONFIRMED)) {
			mcpsReq.Type = MCPS_UNCONFIRMED;
			mcpsReq.Req.Unconfirmed.fPort = port;
			mcpsReq.Req.Unconfirmed.fBuffer = data;
			mcpsReq.Req.Unconfirmed.fBufferSize = len;
			mcpsReq.Req.Unconfirmed.Datarate = lorawan_datarate;
		} else {
			mcpsReq.Type = MCPS_CONFIRMED;
			mcpsReq.Req.Confirmed.fPort = port;
			mcpsReq.Req.Confirmed.fBuffer = data;
			mcpsReq.Req.Confirmed.fBufferSize = len;
			mcpsReq.Req.Confirmed.NbTrials = lorawan_send_tries;
			mcpsReq.Req.Confirmed.Datarate = lorawan_datarate;
		}
	}

	status = LoRaMacMcpsRequest(&mcpsReq);
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("LoRaWAN Send failed: %s",
			log_strdup(status2str(status)));
		ret = mac_status_to_errno[status];
		goto out;
	}

	/*
	 * Indicate the application that the current packet is not sent and
	 * it has to resend the packet
	 */
	if (empty_frame) {
		ret = -EAGAIN;
		goto out;
	}

	/* Wait for send confirmation */
	if (flags & LW_SEND_CONFIRMED) {
		/*
		 * We can be sure that the semaphore will be released for
		 * both success and failure cases after a specific time period.
		 * So we can use K_FOREVER and no need to check the return val.
		 */
		k_sem_take(&mcps_confirm_sem, K_FOREVER);

		if (last_mcps_confirm_status != LORAMAC_EVENT_INFO_STATUS_OK) {
			ret = mac_event_info_to_errno[last_mcps_confirm_status];
		}
	}

out:
	k_mutex_unlock(&lorawan_send_mutex);
	return ret;
}

int lorawan_listen(u8_t port, lorawan_recv_callback_t cb)
{
	struct lorawan_port_listener *l;

	if (port == LW_RECV_PORT_ANY) {
		default_listener = cb;
		return 0;
	}

	/* Is there a listener or do we need to allocate one? */
	l = get_listener(port);
	if (!l) {
		l = get_listener(LW_RECV_PORT_ILLEGAL);
	}

	if (!l) {
		return -ENOMEM;
	}

	if (cb) {
		l->port = port;
		l->cb = cb;
	} else {
		l->port = LW_RECV_PORT_ILLEGAL;
		l->cb = NULL;
	}

	return 0;
}

static int lorawan_init(struct device *dev)
{
	LoRaMacStatus_t status;

	macPrimitives.MacMcpsConfirm = McpsConfirm;
	macPrimitives.MacMcpsIndication = McpsIndication;
	macPrimitives.MacMlmeConfirm = MlmeConfirm;
	macPrimitives.MacMlmeIndication = MlmeIndication;
	macCallbacks.GetBatteryLevel = NULL;
	macCallbacks.GetTemperatureLevel = NULL;
	macCallbacks.NvmContextChange = NULL;
	macCallbacks.MacProcessNotify = OnMacProcessNotify;

	status = LoRaMacInitialization(&macPrimitives, &macCallbacks,
				       LORAWAN_REGION);
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("LoRaMacInitialization failed: %s",
			log_strdup(status2str(status)));
		return -EINVAL;
	}

	LoRaMacStart();

	LOG_DBG("LoRaMAC Initialized");

	return 0;
}

SYS_INIT(lorawan_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
