# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(loramac-node)

zephyr_library_sources_ifdef(CONFIG_LORA_SX12XX sx12xx_common.c)
zephyr_library_sources_ifdef(CONFIG_LORA_SX1276 sx1276.c)
zephyr_library_sources_ifdef(CONFIG_LORA_SX126X sx126x.c)
