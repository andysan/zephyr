/*
 * Copyright (c) 2020 Andreas Sandberg
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <net/lorawan.h>
#include <settings/settings.h>
#include <zephyr.h>

#include <LoRaMac.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(lorawan_settings, CONFIG_LORAWAN_LOG_LEVEL);

#define CFG_BASE "lorawan/state"

struct lorawan_load_context {
	unsigned int valid;
	LoRaMacCtxs_t *mac;
};

enum {
	VALID_CryptoNvmCtx		= BIT(0),
	VALID_SecureElementNvmCtx	= BIT(1),
	VALID_MacNvmCtx			= BIT(2),
	VALID_RegionNvmCtx		= BIT(3),
	VALID_CommandsNvmCtx		= BIT(4),
	VALID_ClassBNvmCtx		= BIT(5),
	VALID_ConfirmQueueNvmCtx	= BIT(6),
};

#define VALID_REQUIRED (BIT(7) - 1)

#define FOR_EACH_CTX()				\
	do {					\
		CTX(CryptoNvmCtx);		\
		CTX(SecureElementNvmCtx);	\
		CTX(MacNvmCtx);			\
		CTX(RegionNvmCtx);		\
		CTX(CommandsNvmCtx);		\
		CTX(ClassBNvmCtx);		\
		CTX(ConfirmQueueNvmCtx);	\
	} while(0)


static int load_setting(void *tgt, size_t tgt_size,
			const char *key, size_t len,
			settings_read_cb read_cb, void *cb_arg)
{
	if (len != tgt_size) {
		LOG_ERR("Can't load '%s' state, size mismatch.",
			log_strdup(key));
		return -EINVAL;
	}

	if (!tgt) {
		LOG_ERR("Can't load '%s' state, no target.",
			log_strdup(key));
		return -EINVAL;
	}

	if (read_cb(cb_arg, tgt, len) != len) {
		LOG_ERR("Can't load '%s' state, short read.",
			log_strdup(key));
		return -EINVAL;
	}

	return 0;
}
static int setting_load_cb(const char *key, size_t len,
			   settings_read_cb read_cb,
			   void *cb_arg, void *param)
{
	struct lorawan_load_context *ctx =
		(struct lorawan_load_context *)param;
	LOG_DBG("Loading '%s'...", log_strdup(key));

#define CTX(PFX)						\
	do {								\
		if (!strcmp(#PFX, key)) {				\
			int ret;					\
			ret = load_setting(ctx->mac->PFX,		\
					   ctx->mac->PFX ## Size,	\
					   key, len, read_cb, cb_arg);	\
			ctx->valid |= ret >= 0 ? VALID_ ## PFX : 0;	\
			return ret;					\
		}							\
	} while(0)

	FOR_EACH_CTX();

#undef CTX

	LOG_WRN("Unknown setting: %s", log_strdup(key));
	return 0;
}

int lorawan_resume()
{
	MibRequestConfirm_t req;
	LoRaMacStatus_t status;
	int ret;
	struct lorawan_load_context ctx = {};

	req.Type = MIB_NVM_CTXS;
	status = LoRaMacMibGetRequestConfirm(&req);
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("Failed to get LoRaMAC state: %d", status);
		return -EINVAL;
	}

	ctx.mac = req.Param.Contexts;
	ret = settings_load_subtree_direct(CFG_BASE, setting_load_cb,
					   &ctx);
	if (ret < 0) {
		LOG_ERR("Failed to load LoRaWAN state");
		return ret;
	}

	if ((ctx.valid & VALID_REQUIRED) != VALID_REQUIRED) {
		if (ctx.valid == 0) {
			LOG_INF("No context stored");
			return -EINVAL;
		}
		LOG_ERR("Failed to restore all required contexts");
		return -EINVAL;
	}

	req.Type = MIB_NVM_CTXS;
	status = LoRaMacMibSetRequestConfirm(&req);
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("Failed to set LoRaMAC state: %d", status);
		return -EINVAL;
	}

	status = LoRaMacStart( );
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("Failed to start the stack: %d", status);
		return -EINVAL;
	}

	return 0;
}

int lorawan_suspend()
{
	LoRaMacStatus_t status;
	MibRequestConfirm_t req;
	LoRaMacCtxs_t *contexts;

	status = LoRaMacStop();
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("Failed to stop the LoRaMAC stack: %d", status);
		return -EINVAL;
	}

	req.Type = MIB_NVM_CTXS;
	status = LoRaMacMibGetRequestConfirm(&req);
	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("Failed to get LoRaMAC state: %d", status);
		return -EINVAL;
	}

	contexts = req.Param.Contexts;

	LOG_DBG("Storing contexts...");

#define CTX(PFX)							\
	do {								\
		int ret;						\
		if (contexts->PFX) {					\
			LOG_DBG("Saving " #PFX);			\
			ret = settings_save_one(			\
				CFG_BASE "/" #PFX,			\
				contexts->PFX,				\
				contexts->PFX ## Size);			\
			if (ret != 0) {					\
				return ret;				\
			}						\
		}							\
	} while(0)

	FOR_EACH_CTX();

#undef CTX

	return 0;
}
