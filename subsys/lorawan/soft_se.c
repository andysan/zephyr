/*
 * Copyright (c) 2020 Andreas Sandberg
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <crypto/cipher.h>
#include <sys/byteorder.h>
#include <zephyr.h>

#include <secure-element.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(lorawan_soft_se, CONFIG_LORAWAN_LOG_LEVEL);

static struct device *crypto_dev = NULL;

#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE 16
#define DEV_EUI_LEN SE_EUI_SIZE
#define JOIN_EUI_LEN SE_EUI_SIZE

#define NUM_KEYS 24
#define NUM_MC_KEYS 24

static const u8_t key_zero[AES_KEY_SIZE] = {};

static struct {
	u8_t dev_eui[DEV_EUI_LEN];
	u8_t join_eui[JOIN_EUI_LEN];
	u8_t keys[NUM_KEYS][AES_KEY_SIZE];
	u8_t mc_keys[NUM_MC_KEYS][AES_KEY_SIZE];
} state;

static void *get_key_raw(KeyIdentifier_t kid)
{
	if (kid == SLOT_RAND_ZERO_KEY) {
		return (void *)key_zero;
	} else if (kid < NUM_KEYS) {
		return state.keys[kid];
	} else if (kid >= LORAMAC_CRYPTO_MULTICAST_KEYS &&
		   kid < LORAMAC_CRYPTO_MULTICAST_KEYS + NUM_MC_KEYS) {
		return state.mc_keys[kid - LORAMAC_CRYPTO_MULTICAST_KEYS];
	} else {
		return NULL;
	}
}

static int get_key(struct cipher_ctx *ctx, KeyIdentifier_t kid)
{
	void *key;

	key = get_key_raw(kid);
	if (!key) {
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));

	ctx->keylen = AES_KEY_SIZE;
	ctx->key.bit_stream = key;
	ctx->flags = CAP_RAW_KEY | CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS;

	return 0;
}

static int set_key(KeyIdentifier_t kid, void *key)
{
	if (kid < NUM_KEYS) {
		memcpy(state.keys[kid], key, AES_KEY_SIZE);
		return 0;
	} else if (kid >= LORAMAC_CRYPTO_MULTICAST_KEYS &&
		   kid < LORAMAC_CRYPTO_MULTICAST_KEYS + NUM_MC_KEYS) {
		memcpy(state.mc_keys[kid - LORAMAC_CRYPTO_MULTICAST_KEYS],
			key, AES_KEY_SIZE);
		return 0;
	} else {
		return -EINVAL;
	}
}

SecureElementStatus_t SecureElementInit(SecureElementNvmEvent nvm_ctx_cb)
{
	int caps;

	ARG_UNUSED(nvm_ctx_cb);

	LOG_DBG("Initializing secure element");
	if (crypto_dev) {
		LOG_ERR("SecureElementInit called twice!");
		return SECURE_ELEMENT_ERROR;
	}

	crypto_dev = device_get_binding(CONFIG_LORAMAC_SE_CRYPTO_DEV_NAME);
	if (!crypto_dev) {
		LOG_ERR("Failed to get crypto device");
		return SECURE_ELEMENT_ERROR;
	}

	caps = cipher_query_hwcaps(crypto_dev);
	if (!(caps & CAP_RAW_KEY) || !(caps & CAP_SYNC_OPS) ||
	    !(CAP_SEPARATE_IO_BUFS)) {
		LOG_ERR("Unsupported crypto device");
		return SECURE_ELEMENT_ERROR;
	}

	return SECURE_ELEMENT_SUCCESS;
}

SecureElementStatus_t SecureElementRestoreNvmCtx(void *nvm_ctx)
{
	ARG_UNUSED(nvm_ctx);

	return SECURE_ELEMENT_SUCCESS;
}

void *SecureElementGetNvmCtx(size_t *nvm_ctx_size)
{
	if (nvm_ctx_size)
		nvm_ctx_size = 0;

	return NULL;
}

SecureElementStatus_t SecureElementSetKey(KeyIdentifier_t kid, u8_t *key)
{
	LOG_DBG("Setting key %i", (int)kid);

	// TODO: MCC_KEY_x may need to be decrypted

	if (set_key(kid, key) < 0) {
		return SECURE_ELEMENT_ERROR_INVALID_KEY_ID;
	} else {
		return SECURE_ELEMENT_SUCCESS;
	}
}

SecureElementStatus_t SecureElementComputeAesCmac(
	u8_t *mic_bx_buf, u8_t *buffer, u16_t size, KeyIdentifier_t kid,
	u32_t *cmac)
{
	SecureElementStatus_t ret = SECURE_ELEMENT_SUCCESS;
	u8_t mac[AES_BLOCK_SIZE];
	struct cipher_ctx ctx = {};
	struct cipher_mac_pkt update_mic_bx = {
		.data.update.buf = mic_bx_buf,
		.data.update.len = 16,
	};
	struct cipher_mac_pkt update_buf = {
		.data.update.buf = buffer,
		.data.update.len = size,
	};
	struct cipher_mac_pkt finalize = {
		.data.finalize.tag = mac,
		.data.finalize.len = sizeof(mac),
		.finalize = true,
	};

	LOG_DBG("Compute AES CMAC using key %i", (int)kid);
	if (!buffer || !cmac) {
		return SECURE_ELEMENT_ERROR_NPE;
	}

	if (get_key(&ctx, kid) < 0) {
		return SECURE_ELEMENT_ERROR_INVALID_KEY_ID;
	}

	if (cipher_begin_session(crypto_dev, &ctx, CRYPTO_CIPHER_ALGO_AES,
				 CRYPTO_CIPHER_MODE_CMAC,
				 CRYPTO_CIPHER_OP_ENCRYPT) < 0) {
		LOG_ERR("Failed to start crypto session: %i", ret);
		return SECURE_ELEMENT_ERROR;
	}

	if ((mic_bx_buf && cipher_cmac_op(&ctx, &update_mic_bx) < 0) ||
	    (cipher_cmac_op(&ctx, &update_buf) < 0)) {
		LOG_ERR("CMAC update failed");
		ret = SECURE_ELEMENT_ERROR;
		goto out_free_session;
	}

	if (cipher_cmac_op(&ctx, &finalize) < 0) {
		LOG_ERR("CMAC finalize failed");
		ret = SECURE_ELEMENT_ERROR;
		goto out_free_session;
	}

	/* We need to provide the first 4 bytes of the MAC in LE
	 * format */
	*cmac = sys_get_le32(mac);

out_free_session:
	cipher_free_session(crypto_dev, &ctx);

	return ret;
}

SecureElementStatus_t SecureElementVerifyAesCmac(
	u8_t *buffer, u16_t size, u32_t expected_mac, KeyIdentifier_t kid)
{
	u32_t mac;
	SecureElementStatus_t ret;

	LOG_DBG("Verify AES CMAC using key %i", (int)kid);
	if (!buffer) {
		return SECURE_ELEMENT_ERROR_NPE;
	}

	ret = SecureElementComputeAesCmac(NULL, buffer, size, kid,
					  &mac);
	if (ret != SECURE_ELEMENT_SUCCESS) {
		return ret;
	}

	if (expected_mac != mac) {
		return SECURE_ELEMENT_FAIL_CMAC;
	}

	return SECURE_ELEMENT_SUCCESS;
}

SecureElementStatus_t SecureElementAesEncrypt(u8_t *input, u16_t size,
					      KeyIdentifier_t kid, u8_t *out)
{
	int ret, i;
	struct cipher_ctx ctx = {};

	LOG_DBG("AES Encrypt using key %i", (int)kid);

	if (!input) {
		return SECURE_ELEMENT_ERROR_NPE;
	}

	if (get_key(&ctx, kid) < 0) {
		return SECURE_ELEMENT_ERROR_INVALID_KEY_ID;
	}

	if (size & (AES_BLOCK_SIZE - 1)) {
		return SECURE_ELEMENT_ERROR_BUF_SIZE;
	}

	ret = cipher_begin_session(crypto_dev, &ctx, CRYPTO_CIPHER_ALGO_AES,
				   CRYPTO_CIPHER_MODE_ECB,
				   CRYPTO_CIPHER_OP_ENCRYPT);
	if (ret < 0) {
		LOG_ERR("Failed to start crypto session: %i", ret);
		return SECURE_ELEMENT_ERROR;
	}

	for (i = 0; i < size; i += AES_BLOCK_SIZE) {
		struct cipher_pkt encrypt = {
			.in_buf = input + i,
			.in_len = AES_BLOCK_SIZE,
			.out_buf_max = AES_BLOCK_SIZE,
			.out_buf = out + i,
		};

		ret = cipher_block_op(&ctx, &encrypt);
		if (ret < 0) {
			LOG_ERR("AES ECB OP failed: %i", ret);
			cipher_free_session(crypto_dev, &ctx);
			return SECURE_ELEMENT_ERROR;
		}
	}

	cipher_free_session(crypto_dev, &ctx);
	return SECURE_ELEMENT_SUCCESS;
}

SecureElementStatus_t SecureElementDeriveAndStoreKey(
	Version_t version, u8_t *input, KeyIdentifier_t root_id,
	KeyIdentifier_t target_id)
{
	SecureElementStatus_t ret;
	u8_t key[AES_KEY_SIZE];

	LOG_DBG("Derive and store key %i", (int)target_id);

	if (!input) {
		return SECURE_ELEMENT_ERROR_NPE;
	}

	/* TODO: Check target and root key IDs? Are there any constraints? */

	ret = SecureElementAesEncrypt(input, AES_KEY_SIZE, root_id, key);
	if(ret != SECURE_ELEMENT_SUCCESS) {
		return ret;
	}

	ret = SecureElementSetKey(target_id, key);
	if (ret != SECURE_ELEMENT_SUCCESS) {
		return ret;
	}

	return SECURE_ELEMENT_SUCCESS;
}

SecureElementStatus_t SecureElementRandomNumber(u32_t *num)
{
	int ret;

	if (!num) {
		return SECURE_ELEMENT_ERROR_NPE;
	}

	ret = sys_csrand_get(num, sizeof(*num));
	if (ret == 0) {
		return SECURE_ELEMENT_SUCCESS;
	} else {
		LOG_ERR("Failed to generate random number: %i", ret);
		return SECURE_ELEMENT_ERROR;
	}
}

SecureElementStatus_t SecureElementSetDevEui(u8_t *dev_eui)
{
	if (!dev_eui) {
		return SECURE_ELEMENT_ERROR_NPE;
	}

#if defined(CONFIG_LORAMAC_SE_DEV_EUI_RO)
	LOG_ERR("Trying to change read-only dev. EUI");
	return SECURE_ELEMENT_ERROR;
#else
	memcpy(state.dev_eui, dev_eui, DEV_EUI_LEN);
	return SECURE_ELEMENT_SUCCESS;
#endif
}

u8_t *SecureElementGetDevEui(void)
{
	return state.dev_eui;
}

SecureElementStatus_t SecureElementSetJoinEui(u8_t *join_eui)
{
	if (!join_eui) {
		return SECURE_ELEMENT_ERROR_NPE;
	}

#if defined(CONFIG_LORAMAC_SE_JOIN_EUI_RO)
	LOG_ERR("Trying to change read-only join EUI");
	return SECURE_ELEMENT_ERROR;
#else
	memcpy(state.join_eui, join_eui, JOIN_EUI_LEN);
	return SECURE_ELEMENT_SUCCESS;
#endif
}

u8_t *SecureElementGetJoinEui(void)
{
	return state.join_eui;
}

