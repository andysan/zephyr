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

#define LOG_LEVEL CONFIG_LORAWAN_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(lorawan);

K_SEM_DEFINE(lorawan_config_sem, 0, 1);
K_SEM_DEFINE(lorawan_tx_sem, 0, 1);

K_MUTEX_DEFINE(lorawan_join_mutex);
K_MUTEX_DEFINE(lorawan_send_mutex);

static volatile bool join_status = false;
static volatile bool send_status = false;

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

static LoRaMacPrimitives_t macPrimitives;
static LoRaMacCallback_t macCallbacks;

void OnMacProcessNotify(void)
{
	LoRaMacProcess();
}

static void McpsConfirm(McpsConfirm_t *mcpsConfirm)
{
	if (mcpsConfirm->Status != LORAMAC_EVENT_INFO_STATUS_OK) {
		LOG_ERR("McpsRequest failed : %s",
			log_strdup(eventinfo2str(mcpsConfirm->Status)));
	} else {
		LOG_DBG("McpsRequest success!");
		send_status = true;
	}

	k_sem_give(&lorawan_tx_sem);
}

static void McpsIndication(McpsIndication_t *mcpsIndication)
{
	if (mcpsIndication->Status != LORAMAC_EVENT_INFO_STATUS_OK) {
		LOG_ERR("McpsIndication failed : %s",
			log_strdup(eventinfo2str(mcpsIndication->Status)));
		return;
	}

	/* TODO: Check MCPS Indication type */
	if (mcpsIndication->RxData == true) {
		if (mcpsIndication->BufferSize != 0) {
			LOG_DBG("Rx Data: %s",
				log_strdup(mcpsIndication->Buffer));
		}
	}

	/* TODO: Compliance test based on FPort value*/
}

static void MlmeConfirm(MlmeConfirm_t *mlmeConfirm)
{
	MibRequestConfirm_t mibGet;

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
		join_status = true;
		break;
	case MLME_LINK_CHECK:
		/* Not implemented */
		break;
	default:
		break;
	}

out_sem:
	k_sem_give(&lorawan_config_sem);
}

static void MlmeIndication(MlmeIndication_t *mlmeIndication)
{
	LOG_DBG("%s", __func__);
}

int lorawan_config(struct lorawan_mib_config *mib_config)
{
	MibRequestConfirm_t mibReq;

	mibReq.Type = MIB_NWK_KEY;
	mibReq.Param.NwkKey = mib_config->nwk_key;
	LoRaMacMibSetRequestConfirm(&mibReq);

	mibReq.Type = MIB_DEV_EUI;
	mibReq.Param.DevEui = mib_config->dev_eui;
	LoRaMacMibSetRequestConfirm(&mibReq);

	mibReq.Type = MIB_JOIN_EUI;
	mibReq.Param.JoinEui = mib_config->join_eui;
	LoRaMacMibSetRequestConfirm(&mibReq);

	mibReq.Type = MIB_DEVICE_CLASS;
	mibReq.Param.Class = mib_config->lw_class;
	LoRaMacMibSetRequestConfirm(&mibReq);

	mibReq.Type = MIB_ADR;
	mibReq.Param.AdrEnable = mib_config->adr_enable;
	LoRaMacMibSetRequestConfirm(&mibReq);

	mibReq.Type = MIB_PUBLIC_NETWORK;
	mibReq.Param.EnablePublicNetwork = mib_config->pub_nw;
	LoRaMacMibSetRequestConfirm(&mibReq);

	return 0;
}

static LoRaMacStatus_t lorawan_join_otaa(enum lorawan_datarate datarate)
{
	MlmeReq_t mlmeReq;

	mlmeReq.Type = MLME_JOIN;
	mlmeReq.Req.Join.Datarate = datarate;

	return LoRaMacMlmeRequest(&mlmeReq);
}

int lorawan_join_network(enum lorawan_datarate datarate,
			 enum lorawan_act_type mode)
{
	LoRaMacStatus_t status;
	int ret;

	k_mutex_lock(&lorawan_join_mutex, K_FOREVER);

	if (mode == LORAWAN_ACT_OTAA) {
		join_status = false;
		status = lorawan_join_otaa(datarate);
		if (status != LORAMAC_STATUS_OK) {
			LOG_ERR("OTAA join failed: %s",
				log_strdup(status2str(status)));
			ret = -EINVAL;
			goto out;
		}

		LOG_DBG("Network join request sent!");

		/*
		 * We can be sure that the semaphore will be released for
		 * both success and failure cases after a specific time period.
		 * So we can use K_FOREVER and no need to check the return val.
		 */
		k_sem_take(&lorawan_config_sem, K_FOREVER);
	} else {
		ret = -EINVAL;
		goto out;
	}

	if (join_status) {
		ret = 0;
	} else {
		/* TODO: Return the exact error code */
		ret = -EINVAL;
	}

out:
	k_mutex_unlock(&lorawan_join_mutex);
	return ret;
}

int lorawan_send(u8_t port, enum lorawan_datarate datarate, u8_t *data,
		 u8_t len, bool confirm, u8_t tries)
{
	LoRaMacStatus_t status;
	McpsReq_t mcpsReq;
	LoRaMacTxInfo_t txInfo;
	int ret = 0;
	bool empty_frame = false;

	if (data == NULL) {
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
		LOG_ERR("LoRaWAN Send failed: %s",
			log_strdup(status2str(status)));
		empty_frame = true;
		mcpsReq.Type = MCPS_UNCONFIRMED;
		mcpsReq.Req.Unconfirmed.fBuffer = NULL;
		mcpsReq.Req.Unconfirmed.fBufferSize = 0;
		mcpsReq.Req.Unconfirmed.Datarate = DR_0;
	} else {
		if (confirm == false) {
			mcpsReq.Type = MCPS_UNCONFIRMED;
			mcpsReq.Req.Unconfirmed.fPort = port;
			mcpsReq.Req.Unconfirmed.fBuffer = data;
			mcpsReq.Req.Unconfirmed.fBufferSize = len;
			mcpsReq.Req.Unconfirmed.Datarate = datarate;
		} else {
			mcpsReq.Type = MCPS_CONFIRMED;
			mcpsReq.Req.Confirmed.fPort = port;
			mcpsReq.Req.Confirmed.fBuffer = data;
			mcpsReq.Req.Confirmed.fBufferSize = len;
			mcpsReq.Req.Confirmed.NbTrials = tries;
			mcpsReq.Req.Confirmed.Datarate = datarate;
			send_status = false;
		}
	}

	status = LoRaMacMcpsRequest(&mcpsReq);
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("LoRaWAN Send failed: %s",
			log_strdup(status2str(status)));
		ret = -EINVAL;
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
	if (confirm) {
		/*
		 * We can be sure that the semaphore will be released for
		 * both success and failure cases after a specific time period.
		 * So we can use K_FOREVER and no need to check the return val.
		 */
		k_sem_take(&lorawan_tx_sem, K_FOREVER);

		if (send_status) {
			ret = 0;
		} else {
			/* TODO: Return the exact error code */
			ret = -EINVAL;
		}
	}

out:
	k_mutex_unlock(&lorawan_send_mutex);
	return ret;
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
