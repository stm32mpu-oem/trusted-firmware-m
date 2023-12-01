/*
 * Copyright (c) 2023, STMicroelectronics.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * This driver allows to read/write/increment counters
 * stored in memory mapped device like ddr/backupram/bkpregister/sram.
 * This avoid to manage backup copy
 */
#include <stdint.h>
#include <cmsis_compiler.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#include <tfm_plat_nv_counters.h>
#include <Driver_Flash.h>
#include <flash_layout.h>
#include <debug.h>

/* Compilation time checks to be sure the defines are well defined */
#ifndef NV_COUNTERS_AREA_SIZE
#error "NV_COUNTERS_AREA_SIZE must be defined"
#endif

#ifndef NV_COUNTERS_AREA_ADDR
#error "NV_COUNTERS_AREA_ADDR must be defined"
#endif

#ifndef NV_COUNTERS_FLASH_DRIVER
#error "NV_COUNTERS_FLASH_DRIVER must be defined"
#endif

/* Import the CMSIS flash device driver */
extern ARM_DRIVER_FLASH NV_COUNTERS_FLASH_DRIVER;

#define NV_COUNTER_SIZE  sizeof(uint32_t)
#define INIT_VALUE_SIZE  sizeof(uint32_t)
#define NUM_NV_COUNTERS  ((NV_COUNTERS_AREA_SIZE - INIT_VALUE_SIZE) \
			  / NV_COUNTER_SIZE)

#define NV_MAGIC_ADDR		(NV_COUNTERS_AREA_ADDR)
#define NV_MAGIC_SZ		(sizeof(uint32_t))
#define NV_COUNTER_ADDR(x)	(NV_MAGIC_ADDR + NV_MAGIC_SZ + (NV_COUNTER_SZ * x))
#define NV_COUNTER_SZ		(sizeof(uint32_t))

#define NV_COUNTERS_INITIALIZED 0xC0DE0042

/* Valid entries for data item width */
static const uint32_t data_width_byte[] = {
    sizeof(uint8_t),
    sizeof(uint16_t),
    sizeof(uint32_t),
};

enum tfm_plat_err_t tfm_plat_init_nv_counter(void)
{
	enum tfm_plat_err_t err = TFM_PLAT_ERR_SUCCESS;
	ARM_FLASH_CAPABILITIES DriverCapabilities;
	ARM_FLASH_INFO *dev_info;
	uint8_t data_width;
	uint32_t magic;
	int32_t cnt;

	err = (enum tfm_plat_err_t)NV_COUNTERS_FLASH_DRIVER.Initialize(NULL);
	if (err != ARM_DRIVER_OK)
		return TFM_PLAT_ERR_SYSTEM_ERR;

	DriverCapabilities = NV_COUNTERS_FLASH_DRIVER.GetCapabilities();
	data_width = data_width_byte[DriverCapabilities.data_width];

	/*
	 * to avoid a backup copy on write, some properties are required:
	 * - nv_couter size = program_unit
	 * - memory mapped device => erase not needed
	 */
	dev_info = NV_COUNTERS_FLASH_DRIVER.GetInfo();

	if (NV_COUNTER_SIZE % dev_info->program_unit)
		return TFM_PLAT_ERR_UNSUPPORTED;

	if (dev_info->erased_value != 0)
		return TFM_PLAT_ERR_UNSUPPORTED;

	/* read init value & checksum */
	cnt = NV_COUNTERS_FLASH_DRIVER.ReadData(NV_MAGIC_ADDR, &magic,
						NV_MAGIC_SZ / data_width);
	if (cnt < 0)
		return TFM_PLAT_ERR_SYSTEM_ERR;

	if (magic == NV_COUNTERS_INITIALIZED)
		return TFM_PLAT_ERR_SUCCESS;

#if defined(STM32_PROV_FAKE)
	uint32_t nv_couter[NUM_NV_COUNTERS];

        WMSG("\033[1;31mNV_MM_COUNTER_INIT is not suitable for production! "
	     "This device is \033[1;1mNOT SECURE \033[0m");

        memset(nv_couter, 0, sizeof(nv_couter));
	cnt = NV_COUNTERS_FLASH_DRIVER.ProgramData(NV_COUNTER_ADDR(0), nv_couter,
						   sizeof(nv_couter) / data_width);
	if (cnt < 0)
		return TFM_PLAT_ERR_SYSTEM_ERR;

	magic = NV_COUNTERS_INITIALIZED;
	cnt = NV_COUNTERS_FLASH_DRIVER.ProgramData(NV_MAGIC_ADDR, &magic,
						   NV_MAGIC_SZ / data_width);
	if (cnt == (NV_MAGIC_SZ / data_width))
		return TFM_PLAT_ERR_SUCCESS;
#endif

	return TFM_PLAT_ERR_SYSTEM_ERR;
}

enum tfm_plat_err_t tfm_plat_read_nv_counter(enum tfm_nv_counter_t counter_id,
                                             uint32_t size, uint8_t *val)
{
	ARM_FLASH_CAPABILITIES DriverCapabilities;
	uint8_t data_width;
	int32_t cnt;

	DriverCapabilities = NV_COUNTERS_FLASH_DRIVER.GetCapabilities();
	data_width = data_width_byte[DriverCapabilities.data_width];

	if (size != NV_COUNTER_SIZE)
		return TFM_PLAT_ERR_SYSTEM_ERR;

	if (counter_id >= NUM_NV_COUNTERS)
		return TFM_PLAT_ERR_SYSTEM_ERR;

	cnt = NV_COUNTERS_FLASH_DRIVER.ReadData(NV_COUNTER_ADDR(counter_id), val,
						NV_COUNTER_SIZE / data_width);
	if (cnt < 0)
		return TFM_PLAT_ERR_SYSTEM_ERR;

	return TFM_PLAT_ERR_SUCCESS;
}

enum tfm_plat_err_t tfm_plat_set_nv_counter(enum tfm_nv_counter_t counter_id,
					    uint32_t value)
{
	ARM_FLASH_CAPABILITIES DriverCapabilities;
	uint8_t data_width;
	int32_t cnt;

	DriverCapabilities = NV_COUNTERS_FLASH_DRIVER.GetCapabilities();
	data_width = data_width_byte[DriverCapabilities.data_width];

	cnt = NV_COUNTERS_FLASH_DRIVER.ProgramData(NV_COUNTER_ADDR(counter_id),
						   &value,
						   NV_COUNTER_SIZE / data_width);
	if (cnt < 0)
		return TFM_PLAT_ERR_SYSTEM_ERR;

	return TFM_PLAT_ERR_SUCCESS;
}

enum tfm_plat_err_t
tfm_plat_increment_nv_counter(enum tfm_nv_counter_t counter_id)
{
	uint32_t security_cnt;
	enum tfm_plat_err_t err;

	err = tfm_plat_read_nv_counter(counter_id,
				       sizeof(security_cnt),
				       (uint8_t *)&security_cnt);
	if (err != TFM_PLAT_ERR_SUCCESS) {
		return err;
	}

	if (security_cnt == UINT32_MAX) {
		return TFM_PLAT_ERR_MAX_VALUE;
	}

	return tfm_plat_set_nv_counter(counter_id, security_cnt + 1u);
}
