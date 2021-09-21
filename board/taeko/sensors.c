/* Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "accelgyro.h"
#include "adc_chip.h"
#include "driver/accel_bma422.h"
#include "driver/accel_bma4xx.h"
#include "driver/accel_lis2dw12.h"
#include "driver/accelgyro_lsm6dso.h"
#include "hooks.h"
#include "i2c.h"
#include "motion_sense.h"
#include "temp_sensor.h"
#include "thermal.h"
#include "temp_sensor/thermistor.h"

#if 0
#define CPRINTS(format, args...) ccprints(format, ## args)
#define CPRINTF(format, args...) ccprintf(format, ## args)
#else
#define CPRINTS(format, args...)
#define CPRINTF(format, args...)
#endif

/* ADC configuration */
const struct adc_t adc_channels[] = {
	[ADC_TEMP_SENSOR_1_DDR_SOC] = {
		.name = "TEMP_DDR_SOC",
		.input_ch = NPCX_ADC_CH0,
		.factor_mul = ADC_MAX_VOLT,
		.factor_div = ADC_READ_MAX + 1,
		.shift = 0,
	},
	[ADC_TEMP_SENSOR_2_FAN] = {
		.name = "TEMP_FAN",
		.input_ch = NPCX_ADC_CH1,
		.factor_mul = ADC_MAX_VOLT,
		.factor_div = ADC_READ_MAX + 1,
		.shift = 0,
	},
	[ADC_TEMP_SENSOR_3_CHARGER] = {
		.name = "TEMP_CHARGER",
		.input_ch = NPCX_ADC_CH6,
		.factor_mul = ADC_MAX_VOLT,
		.factor_div = ADC_READ_MAX + 1,
		.shift = 0,
	},
	[ADC_TEMP_SENSOR_4_WWAN] = {
		.name = "TEMP_WWAN",
		.input_ch = NPCX_ADC_CH7,
		.factor_mul = ADC_MAX_VOLT,
		.factor_div = ADC_READ_MAX + 1,
		.shift = 0,
	},
};
BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CH_COUNT);

K_MUTEX_DEFINE(g_lid_accel_mutex);
K_MUTEX_DEFINE(g_base_accel_mutex);
static struct stprivate_data g_lis2dw12_data;
static struct lsm6dso_data lsm6dso_data;
static struct accelgyro_saved_data_t g_bma422_data;

/* TODO(b/184779333): calibrate the orientation matrix on later board stage */
static const mat33_fp_t lid_standard_ref = {
	{ 0, FLOAT_TO_FP(1), 0},
	{ FLOAT_TO_FP(1), 0, 0},
	{ 0, 0, FLOAT_TO_FP(-1)}
};

/* TODO(b/184779743): verify orientation matrix */
static const mat33_fp_t base_standard_ref = {
	{ FLOAT_TO_FP(1), 0, 0},
	{ 0, FLOAT_TO_FP(-1), 0},
	{ 0, 0, FLOAT_TO_FP(-1)}
};


struct motion_sensor_t bma422_lid_accel = {
	.name = "Lid Accel - BMA",
	.active_mask = SENSOR_ACTIVE_S0_S3,
	.chip = MOTIONSENSE_CHIP_BMA422,
	.type = MOTIONSENSE_TYPE_ACCEL,
	.location = MOTIONSENSE_LOC_LID,
	.drv = &bma4_accel_drv,
	.mutex = &g_lid_accel_mutex,
	.drv_data = &g_bma422_data,
	.port = I2C_PORT_SENSOR,
	.i2c_spi_addr_flags = BMA4_I2C_ADDR_PRIMARY, /* 0x18 */
	.rot_standard_ref = &lid_standard_ref, /* identity matrix */
	.default_range = 2, /* g, enough for laptop. */
	.min_frequency = BMA4_ACCEL_MIN_FREQ,
	.max_frequency = BMA4_ACCEL_MAX_FREQ,
	.config = {
		/* EC use accel for angle detection */
		[SENSOR_CONFIG_EC_S0] = {
			.odr = 12500 | ROUND_UP_FLAG,
			.ec_rate = 100 * MSEC,
		},
		/* Sensor on in S3 */
		[SENSOR_CONFIG_EC_S3] = {
			.odr = 12500 | ROUND_UP_FLAG,
			.ec_rate = 0,
		},
	},
};

struct motion_sensor_t motion_sensors[] = {
	[LID_ACCEL] = {
		.name = "Lid Accel - ST",
		.active_mask = SENSOR_ACTIVE_S0_S3,
		.chip = MOTIONSENSE_CHIP_LIS2DW12,
		.type = MOTIONSENSE_TYPE_ACCEL,
		.location = MOTIONSENSE_LOC_LID,
		.drv = &lis2dw12_drv,
		.mutex = &g_lid_accel_mutex,
		.drv_data = &g_lis2dw12_data,
		.int_signal = GPIO_EC_ACCEL_INT_R_L,
		.port = I2C_PORT_SENSOR,
		.i2c_spi_addr_flags = LIS2DW12_ADDR1, /* 0x19 */
		.flags = MOTIONSENSE_FLAG_INT_SIGNAL,
		.rot_standard_ref = &lid_standard_ref, /* identity matrix */
		.default_range = 2, /* g */
		.min_frequency = LIS2DW12_ODR_MIN_VAL,
		.max_frequency = LIS2DW12_ODR_MAX_VAL,
		.config = {
			/* EC use accel for angle detection */
			[SENSOR_CONFIG_EC_S0] = {
				.odr = 12500 | ROUND_UP_FLAG,
			},
			/* Sensor on for lid angle detection */
			[SENSOR_CONFIG_EC_S3] = {
				.odr = 10000 | ROUND_UP_FLAG,
			},
		},
	},

	[BASE_ACCEL] = {
		.name = "Base Accel",
		.active_mask = SENSOR_ACTIVE_S0_S3,
		.chip = MOTIONSENSE_CHIP_LSM6DSO,
		.type = MOTIONSENSE_TYPE_ACCEL,
		.location = MOTIONSENSE_LOC_BASE,
		.drv = &lsm6dso_drv,
		.mutex = &g_base_accel_mutex,
		.drv_data = &lsm6dso_data,
		.int_signal = GPIO_EC_IMU_INT_R_L,
		.flags = MOTIONSENSE_FLAG_INT_SIGNAL,
		.port = I2C_PORT_SENSOR,
		.i2c_spi_addr_flags = LSM6DSO_ADDR0_FLAGS,
		.rot_standard_ref = &base_standard_ref,
		.default_range = 4,  /* g */
		.min_frequency = LSM6DSO_ODR_MIN_VAL,
		.max_frequency = LSM6DSO_ODR_MAX_VAL,
		.config = {
			[SENSOR_CONFIG_EC_S0] = {
				.odr = 13000 | ROUND_UP_FLAG,
				.ec_rate = 100 * MSEC,
			},
			[SENSOR_CONFIG_EC_S3] = {
				.odr = 10000 | ROUND_UP_FLAG,
				.ec_rate = 100 * MSEC,
			},
		},
	},

	[BASE_GYRO] = {
		.name = "Base Gyro",
		.active_mask = SENSOR_ACTIVE_S0_S3,
		.chip = MOTIONSENSE_CHIP_LSM6DSO,
		.type = MOTIONSENSE_TYPE_GYRO,
		.location = MOTIONSENSE_LOC_BASE,
		.drv = &lsm6dso_drv,
		.mutex = &g_base_accel_mutex,
		.drv_data = &lsm6dso_data,
		.int_signal = GPIO_EC_IMU_INT_R_L,
		.flags = MOTIONSENSE_FLAG_INT_SIGNAL,
		.port = I2C_PORT_SENSOR,
		.i2c_spi_addr_flags = LSM6DSO_ADDR0_FLAGS,
		.default_range = 1000 | ROUND_UP_FLAG, /* dps */
		.rot_standard_ref = &base_standard_ref,
		.min_frequency = LSM6DSO_ODR_MIN_VAL,
		.max_frequency = LSM6DSO_ODR_MAX_VAL,
	},

};
const unsigned int motion_sensor_count = ARRAY_SIZE(motion_sensors);

static void board_detect_motionsensor(void)
{
	int ret;
	int val;

	if (chipset_in_state(CHIPSET_STATE_ANY_OFF))
		return;

	/*
	 * TODO:b/194765820 - Dynamic motion sensor count
	 * Clamshell un-support motion sensor.
	 * We should ignore to detect motion sensor for clamshell.
	 * List this TODO item until we know how to identify DUT type.
	 */

	/* Check lid accel chip */
	ret = i2c_read8(I2C_PORT_SENSOR, LIS2DW12_ADDR1,
			LIS2DW12_WHO_AM_I_REG, &val);
	if (ret == 0 && val == LIS2DW12_WHO_AM_I) {
		CPRINTS("LID_ACCEL is IS2DW12");
		/* Enable gpio interrupt for lid accel sensor */
		gpio_enable_interrupt(GPIO_EC_ACCEL_INT_R_L);
		return;
	}

	ret = i2c_read8(I2C_PORT_SENSOR, BMA4_I2C_ADDR_PRIMARY,
			BMA4_CHIP_ID_ADDR, &val);
	if (ret == 0 && val == BMA422_CHIP_ID) {
		CPRINTS("LID_ACCEL is BMA422");
		motion_sensors[LID_ACCEL] = bma422_lid_accel;
		/*
		 * The driver for BMA422 doesn't have code to support
		 * INT1. So, it doesn't need to enable interrupt.
		 * Vendor recommend to configure EC gpio as high-z if
		 * we don't use INT1. Keep this pin as input w/o enable
		 * interrupt.
		 */
		return;
	}

	/* Lid accel is not stuffed, don't allow line to float */
	gpio_disable_interrupt(GPIO_EC_ACCEL_INT_R_L);
	gpio_set_flags(GPIO_EC_ACCEL_INT_R_L, GPIO_INPUT | GPIO_PULL_DOWN);
	CPRINTS("No LID_ACCEL");
}
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, board_detect_motionsensor,
		HOOK_PRIO_DEFAULT);


static void baseboard_sensors_init(void)
{
	CPRINTS("baseboard_sensors_init");
	/*
	 * GPIO_EC_ACCEL_INT_R_L
	 * The interrupt of lid accel is disabled by default.
	 * We'll enable it later if lid accel is LIS2DW12.
	 */

	/* Enable gpio interrupt for base accelgyro sensor */
	gpio_enable_interrupt(GPIO_EC_IMU_INT_R_L);
}
DECLARE_HOOK(HOOK_INIT, baseboard_sensors_init, HOOK_PRIO_INIT_I2C + 1);

void motion_interrupt(enum gpio_signal signal)
{
	if (motion_sensors[LID_ACCEL].chip == MOTIONSENSE_CHIP_LIS2DW12) {
		lis2dw12_interrupt(signal);
		CPRINTS("IS2DW12 interrupt");
		return;
	}

	/*
	 * From other project, ex. guybrush, it seem BMA422 doesn't have
	 * interrupt handler when EC_ACCEL_INT_R_L is asserted.
	 * However, I don't see BMA422 assert EC_ACCEL_INT_R_L when it has
	 * power. That could be the reason EC code doesn't register any
	 * interrupt handler.
	 */
	CPRINTS("BMA422 interrupt");

}

/* Temperature sensor configuration */
const struct temp_sensor_t temp_sensors[] = {
	[TEMP_SENSOR_1_DDR_SOC] = {
		.name = "DDR and SOC",
		.type = TEMP_SENSOR_TYPE_BOARD,
		.read = get_temp_3v3_30k9_47k_4050b,
		.idx = ADC_TEMP_SENSOR_1_DDR_SOC
	},
	[TEMP_SENSOR_2_FAN] = {
		.name = "FAN",
		.type = TEMP_SENSOR_TYPE_BOARD,
		.read = get_temp_3v3_30k9_47k_4050b,
		.idx = ADC_TEMP_SENSOR_2_FAN
	},
	[TEMP_SENSOR_3_CHARGER] = {
		.name = "CHARGER",
		.type = TEMP_SENSOR_TYPE_BOARD,
		.read = get_temp_3v3_30k9_47k_4050b,
		.idx = ADC_TEMP_SENSOR_3_CHARGER
	},
	[TEMP_SENSOR_4_WWAN] = {
		.name = "WWAN",
		.type = TEMP_SENSOR_TYPE_BOARD,
		.read = get_temp_3v3_30k9_47k_4050b,
		.idx = ADC_TEMP_SENSOR_4_WWAN
	},
};


BUILD_ASSERT(ARRAY_SIZE(temp_sensors) == TEMP_SENSOR_COUNT);

/*
 * TODO(b/180681346): update for Alder Lake/brya
 *
 * Tiger Lake specifies 100 C as maximum TDP temperature.  THRMTRIP# occurs at
 * 130 C.  However, sensor is located next to DDR, so we need to use the lower
 * DDR temperature limit (85 C)
 */
static const struct ec_thermal_config thermal_cpu = {
	.temp_host = {
		[EC_TEMP_THRESH_HIGH] = C_TO_K(70),
		[EC_TEMP_THRESH_HALT] = C_TO_K(80),
	},
	.temp_host_release = {
		[EC_TEMP_THRESH_HIGH] = C_TO_K(65),
	},
	.temp_fan_off = C_TO_K(35),
	.temp_fan_max = C_TO_K(50),
};

/*
 * TODO(b/180681346): update for Alder Lake/brya
 *
 * Inductor limits - used for both charger and PP3300 regulator
 *
 * Need to use the lower of the charger IC, PP3300 regulator, and the inductors
 *
 * Charger max recommended temperature 100C, max absolute temperature 125C
 * PP3300 regulator: operating range -40 C to 145 C
 *
 * Inductors: limit of 125c
 * PCB: limit is 80c
 */
static const struct ec_thermal_config thermal_fan = {
	.temp_host = {
		[EC_TEMP_THRESH_HIGH] = C_TO_K(75),
		[EC_TEMP_THRESH_HALT] = C_TO_K(80),
	},
	.temp_host_release = {
		[EC_TEMP_THRESH_HIGH] = C_TO_K(65),
	},
	.temp_fan_off = C_TO_K(40),
	.temp_fan_max = C_TO_K(55),
};

/* this should really be "const" */
struct ec_thermal_config thermal_params[] = {
	[TEMP_SENSOR_1_DDR_SOC] = thermal_cpu,
	[TEMP_SENSOR_2_FAN]	= thermal_fan,
	[TEMP_SENSOR_3_CHARGER]	= thermal_fan,
	[TEMP_SENSOR_4_WWAN]	= thermal_fan,
};

BUILD_ASSERT(ARRAY_SIZE(thermal_params) == TEMP_SENSOR_COUNT);