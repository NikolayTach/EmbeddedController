/* Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Battery pack vendor provided charging profile
 */

#include "battery.h"
#include "battery_smart.h"
#include "bd9995x.h"
#include "charge_ramp.h"
#include "charge_state.h"
#include "charger_profile_override.h"
#include "console.h"
#include "ec_commands.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "i2c.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_CHARGER, format, ## args)

enum battery_type {
	BATTERY_SONY_CORP,
	BATTERY_SMP_COS4870,
	BATTERY_SMP_C22N1626,
	BATTERY_CPT_C22N1626,
	BATTERY_TYPE_COUNT,
};

enum temp_range {
	TEMP_RANGE_0,
	TEMP_RANGE_1,
	TEMP_RANGE_2,
	TEMP_RANGE_3,
	TEMP_RANGE_4,
};

struct ship_mode_info {
	const int ship_mode_reg;
	const int ship_mode_data;
	int (*batt_init)(void);
};

struct board_batt_params {
	const char *manuf_name;
	const struct ship_mode_info *ship_mode_inf;
	const struct battery_info *batt_info;
	const struct fast_charge_params *fast_chg_params;
};

#define DEFAULT_BATTERY_TYPE BATTERY_SONY_CORP
#define SONY_DISCHARGE_DISABLE_FET_BIT (0x01 << 13)
#define C22N1626_DISCHARGE_ENABLE_FET_BIT (0x01 << 0)

/* keep track of previous charge profile info */
static const struct fast_charge_profile *prev_chg_profile_info;

static enum battery_present batt_pres_prev = BP_NOT_SURE;

static enum battery_type board_battery_type = BATTERY_TYPE_COUNT;

static const struct fast_charge_profile fast_charge_smp_cos4870_info[] = {
	/* < 0C */
	[TEMP_RANGE_0] = {
		.temp_c = TEMPC_TENTHS_OF_DEG(-1),
		.current_mA = {
			[VOLTAGE_RANGE_LOW] = 0,
			[VOLTAGE_RANGE_HIGH] = 0,
		},
	},

	/* 0C >= && <=15C */
	[TEMP_RANGE_1] = {
		.temp_c = TEMPC_TENTHS_OF_DEG(15),
		.current_mA = {
			[VOLTAGE_RANGE_LOW] = 944,
			[VOLTAGE_RANGE_HIGH] = 472,
		},
	},

	/* 15C > && <=20C */
	[TEMP_RANGE_2] = {
		.temp_c = TEMPC_TENTHS_OF_DEG(20),
		.current_mA = {
			[VOLTAGE_RANGE_LOW] = 1416,
			[VOLTAGE_RANGE_HIGH] = 1416,
		},
	},

	/* 20C > && <=45C */
	[TEMP_RANGE_3] = {
		.temp_c = TEMPC_TENTHS_OF_DEG(45),
		.current_mA = {
			[VOLTAGE_RANGE_LOW] = 3300,
			[VOLTAGE_RANGE_HIGH] = 3300,
		},
	},

	/* > 45C */
	[TEMP_RANGE_4] = {
		.temp_c = TEMPC_TENTHS_OF_DEG(CHARGER_PROF_TEMP_C_LAST_RANGE),
		.current_mA = {
			[VOLTAGE_RANGE_LOW] = 0,
			[VOLTAGE_RANGE_HIGH] = 0,
		},
	},
};

static const struct fast_charge_params fast_chg_params_smp_cos4870 = {
	.total_temp_ranges = ARRAY_SIZE(fast_charge_smp_cos4870_info),
	.default_temp_range_profile = TEMP_RANGE_2,
	.vtg_low_limit_mV = 8000,
	.chg_profile_info = &fast_charge_smp_cos4870_info[0],
};

const struct battery_info batt_info_smp_cos4870 = {
	.voltage_max = 8700,	/* mV */
	.voltage_normal = 7600,
	/*
	 * Actual value 6000mV, added 100mV for charger accuracy so that
	 * unwanted low VSYS_Prochot# assertion can be avoided.
	 */
	.voltage_min = 6100,
	.precharge_current = 256,	/* mA */
	.start_charging_min_c = 0,
	.start_charging_max_c = 46,
	.charging_min_c = 0,
	.charging_max_c = 45,
	.discharging_min_c = 0,
	.discharging_max_c = 60,
};

static const struct fast_charge_profile fast_charge_sonycorp_info[] = {
	/* < 10C */
	[TEMP_RANGE_0] = {
		.temp_c = TEMPC_TENTHS_OF_DEG(9),
		.current_mA = {
			[VOLTAGE_RANGE_LOW] = 1200,
			[VOLTAGE_RANGE_HIGH] = 1200,
		},
	},

	/* >= 10C */
	[TEMP_RANGE_1] = {
		.temp_c = TEMPC_TENTHS_OF_DEG(CHARGER_PROF_TEMP_C_LAST_RANGE),
		.current_mA = {
			[VOLTAGE_RANGE_LOW] = 3300,
			[VOLTAGE_RANGE_HIGH] = 3300,
		},
	},
};

static const struct fast_charge_params fast_chg_params_sonycorp = {
	.total_temp_ranges = ARRAY_SIZE(fast_charge_sonycorp_info),
	.default_temp_range_profile = TEMP_RANGE_1,
	.vtg_low_limit_mV = 8000,
	.chg_profile_info = &fast_charge_sonycorp_info[0],
};

const struct battery_info batt_info_sonycorp = {
	.voltage_max = 8700,	/* mV */
	.voltage_normal = 7600,

	/*
	 * Actual value 6000mV, added 100mV for charger accuracy so that
	 * unwanted low VSYS_Prochot# assertion can be avoided.
	 */
	.voltage_min = 6100,
	.precharge_current = 256,	/* mA */
	.start_charging_min_c = 0,
	.start_charging_max_c = 50,
	.charging_min_c = 0,
	.charging_max_c = 60,
	.discharging_min_c = -20,
	.discharging_max_c = 75,
};

const struct battery_info batt_info_c22n1626 = {
	.voltage_max = 8800,	/* mV */
	.voltage_normal = 7700,

	/*
	 * Actual value 6000mV, added 100mV for charger accuracy so that
	 * unwanted low VSYS_Prochot# assertion can be avoided.
	 */
	.voltage_min = 6100,
	.precharge_current = 256,	/* mA */
	.start_charging_min_c = 0,
	.start_charging_max_c = 45,
	.charging_min_c = 0,
	.charging_max_c = 60,
	.discharging_min_c = 0,
	.discharging_max_c = 60,
};

static int batt_smp_cos4870_init(void)
{
	int batt_status;

	return battery_status(&batt_status) ? 0 :
		batt_status & STATUS_INITIALIZED;
}

static int batt_sony_corp_init(void)
{
	int batt_status;

	/*
	 * SB_MANUFACTURER_ACCESS:
	 * [13] : Discharging Disabled
	 *      : 0b - Allowed to Discharge
	 *      : 1b - Not Allowed to Discharge
	 */
	return sb_read(SB_MANUFACTURER_ACCESS, &batt_status) ? 0 :
		!(batt_status & SONY_DISCHARGE_DISABLE_FET_BIT);
}

static int batt_c22n1626_init(void)
{
	int batt_status;

	/*
	 * SB_PACK_STATUS:
	 * [0] : Discharging Enabled
	 *      : 0b - Not Allowed to Discharge
	 *      : 1b - Allowed to Discharge
	 */
	return sb_read(SB_PACK_STATUS, &batt_status) ? 0 :
		!!(batt_status & C22N1626_DISCHARGE_ENABLE_FET_BIT);
}

static const struct ship_mode_info ship_mode_info_smp_cos4870 = {
	.ship_mode_reg = 0x00,
	.ship_mode_data = 0x0010,
	.batt_init = batt_smp_cos4870_init,
};

static const struct ship_mode_info ship_mode_info_sonycorp = {
	.ship_mode_reg = 0x3A,
	.ship_mode_data = 0xC574,
	.batt_init = batt_sony_corp_init,
};

static const struct ship_mode_info ship_mode_info_c22n1626= {
	.ship_mode_reg = 0x00,
	.ship_mode_data = 0x0010,
	.batt_init = batt_c22n1626_init,
};

static const struct board_batt_params info[] = {
	/* BQ40Z555 SONY CORP BATTERY battery specific configurations */
	[BATTERY_SONY_CORP] = {
		.manuf_name = "SONYCorp",
		.ship_mode_inf = &ship_mode_info_sonycorp,
		.fast_chg_params = &fast_chg_params_sonycorp,
		.batt_info = &batt_info_sonycorp,
	},

	/* BQ40Z55 SMP COS4870 BATTERY battery specific configurations */
	[BATTERY_SMP_COS4870] = {
		.manuf_name = "SMP-COS4870",
		.ship_mode_inf = &ship_mode_info_smp_cos4870,
		.fast_chg_params = &fast_chg_params_smp_cos4870,
		.batt_info = &batt_info_smp_cos4870,
	},

	/* BQ40Z55 SMP C22N1626 BATTERY battery specific configurations */
	[BATTERY_SMP_C22N1626] = {
		.manuf_name = "AS1FNZD3KD",
		.ship_mode_inf = &ship_mode_info_c22n1626,
		.fast_chg_params = &fast_chg_params_smp_cos4870,
		.batt_info = &batt_info_c22n1626,
	},

	/* BQ40Z55 CPT C22N1626 BATTERY battery specific configurations */
	[BATTERY_CPT_C22N1626] = {
		.manuf_name = "AS1FOAD3KD",
		.ship_mode_inf = &ship_mode_info_c22n1626,
		.fast_chg_params = &fast_chg_params_smp_cos4870,
		.batt_info = &batt_info_c22n1626,
	},
};
BUILD_ASSERT(ARRAY_SIZE(info) == BATTERY_TYPE_COUNT);

static inline const struct board_batt_params *board_get_batt_params(void)
{
	return &info[board_battery_type == BATTERY_TYPE_COUNT ?
			DEFAULT_BATTERY_TYPE : board_battery_type];
}

static inline enum battery_present battery_hw_present(void)
{
	/* The GPIO is low when the battery is physically present */
	return gpio_get_level(GPIO_EC_BATT_PRES_L) ? BP_NO : BP_YES;
}

/* Get type of the battery connected on the board */
static int board_get_battery_type(void)
{
	const struct fast_charge_params *chg_params;
	char name[32];
	int i;

	if (!battery_manufacturer_name(name, sizeof(name))) {
		for (i = 0; i < BATTERY_TYPE_COUNT; i++) {
			if (!strcasecmp(name, info[i].manuf_name)) {
				board_battery_type = i;
				break;
			}
		}
	}

	/* Initialize fast charging parameters */
	chg_params = board_get_batt_params()->fast_chg_params;
	prev_chg_profile_info = &chg_params->chg_profile_info[
					chg_params->default_temp_range_profile];

	return board_battery_type;
}

/*
 * Initialize the battery type for the board.
 *
 * Very first battery info is called by the charger driver to initialize
 * the charger parameters hence initialize the battery type for the board
 * as soon as the I2C is initialized.
 */
static void board_init_battery_type(void)
{
	if (board_get_battery_type() != BATTERY_TYPE_COUNT)
		CPRINTS("found batt:%s", info[board_battery_type].manuf_name);
	else
		CPRINTS("battery not found");
}
DECLARE_HOOK(HOOK_INIT, board_init_battery_type, HOOK_PRIO_INIT_I2C + 1);

const struct battery_info *battery_get_info(void)
{
	return board_get_batt_params()->batt_info;
}

int board_cut_off_battery(void)
{
	int rv;
	const struct ship_mode_info *ship_mode_inf =
				board_get_batt_params()->ship_mode_inf;

	/* Ship mode command must be sent twice to take effect */
	rv = sb_write(ship_mode_inf->ship_mode_reg,
			ship_mode_inf->ship_mode_data);
	if (rv != EC_SUCCESS)
		return rv;

	return sb_write(ship_mode_inf->ship_mode_reg,
			ship_mode_inf->ship_mode_data);
}

enum battery_disconnect_state battery_get_disconnect_state(void)
{
	uint8_t data[6];
	int rv;

	/*
	 * Take note if we find that the battery isn't in disconnect state,
	 * and always return NOT_DISCONNECTED without probing the battery.
	 * This assumes the battery will not go to disconnect state during
	 * runtime.
	 */
	static int not_disconnected;

	if (not_disconnected)
		return BATTERY_NOT_DISCONNECTED;

	if (extpower_is_present()) {
		/* Check if battery charging + discharging is disabled. */
		rv = sb_read_mfgacc(PARAM_OPERATION_STATUS,
				SB_ALT_MANUFACTURER_ACCESS, data, sizeof(data));
		if (rv)
			return BATTERY_DISCONNECT_ERROR;
		if (~data[3] & (BATTERY_DISCHARGING_DISABLED |
				BATTERY_CHARGING_DISABLED)) {
			not_disconnected = 1;
			return BATTERY_NOT_DISCONNECTED;
		}

		/*
		 * Battery is neither charging nor discharging. Verify that
		 * we didn't enter this state due to a safety fault.
		 */
		rv = sb_read_mfgacc(PARAM_SAFETY_STATUS,
				SB_ALT_MANUFACTURER_ACCESS, data, sizeof(data));
		if (rv || data[2] || data[3] || data[4] || data[5])
			return BATTERY_DISCONNECT_ERROR;

		/*
		 * Battery is present and also the status is initialized and
		 * no safety fault, battery is disconnected.
		 */
		if (battery_is_present() == BP_YES)
			return BATTERY_DISCONNECTED;
	}
	not_disconnected = 1;
	return BATTERY_NOT_DISCONNECTED;
}

static int charger_should_discharge_on_ac(struct charge_state_data *curr)
{
	/* can not discharge on AC without battery */
	if (curr->batt.is_present != BP_YES)
		return 0;

	/* Do not discharge on AC if the battery is still waking up */
	if (!(curr->batt.flags & BATT_FLAG_WANT_CHARGE) &&
		!(curr->batt.status & STATUS_FULLY_CHARGED))
		return 0;

	/*
	 * In light load (<450mA being withdrawn from VSYS) the DCDC of the
	 * charger operates intermittently i.e. DCDC switches continuously
	 * and then stops to regulate the output voltage and current, and
	 * sometimes to prevent reverse current from flowing to the input.
	 * This causes a slight voltage ripple on VSYS that falls in the
	 * audible noise frequency (single digit kHz range). This small
	 * ripple generates audible noise in the output ceramic capacitors
	 * (caps on VSYS and any input of DCDC under VSYS).
	 *
	 * To overcome this issue enable the battery learning operation
	 * and suspend USB charging and DC/DC converter.
	 */
	if (!battery_is_cut_off() &&
		!(curr->batt.flags & BATT_FLAG_WANT_CHARGE) &&
		(curr->batt.status & STATUS_FULLY_CHARGED))
		return 1;

	/*
	 * To avoid inrush current from the external charger, enable
	 * discharge on AC till the new charger is detected and charge
	 * detect delay has passed.
	 */
	if (!chg_ramp_is_detected() && curr->batt.state_of_charge > 2)
		return 1;

	return 0;
}

/*
 * This can override the smart battery's charging profile. To make a change,
 * modify one or more of requested_voltage, requested_current, or state.
 * Leave everything else unchanged.
 *
 * Return the next poll period in usec, or zero to use the default (which is
 * state dependent).
 */
int charger_profile_override(struct charge_state_data *curr)
{
	int disch_on_ac = charger_should_discharge_on_ac(curr);

	charger_discharge_on_ac(disch_on_ac);

	if (disch_on_ac) {
		curr->state = ST_DISCHARGE;
		return 0;
	}

	return charger_profile_override_common(curr,
			board_get_batt_params()->fast_chg_params,
			&prev_chg_profile_info,
			board_get_batt_params()->batt_info->voltage_max);
}

/*
 * Physical detection of battery.
 */
enum battery_present battery_is_present(void)
{
	enum battery_present batt_pres;

	/* Get the physical hardware status */
	batt_pres = battery_hw_present();

	/*
	 * Make sure battery status is implemented, I2C transactions are
	 * success & the battery status is Initialized to find out if it
	 * is a working battery and it is not in the cut-off mode.
	 *
	 * If battery I2C fails but VBATT is high, battery is booting from
	 * cut-off mode.
	 *
	 * FETs are turned off after Power Shutdown time.
	 * The device will wake up when a voltage is applied to PACK.
	 * Battery status will be inactive until it is initialized.
	 */
	if (batt_pres == BP_YES && batt_pres_prev != batt_pres &&
		!battery_is_cut_off()) {
		/* Re-init board battery if battery presence status changes */
		if (board_get_battery_type() == BATTERY_TYPE_COUNT) {
			if (bd9995x_get_battery_voltage() >=
			    board_get_batt_params()->batt_info->voltage_min)
				batt_pres = BP_NO;
		} else if (!board_get_batt_params()->ship_mode_inf->batt_init())
			batt_pres = BP_NO;
	}

	batt_pres_prev = batt_pres;

	return batt_pres;
}

int board_battery_initialized(void)
{
	return battery_hw_present() == batt_pres_prev;
}
