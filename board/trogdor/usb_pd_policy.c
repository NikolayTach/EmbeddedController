/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "charge_manager.h"
#include "console.h"
#include "gpio.h"
#include "pi3usb9281.h"
#include "system.h"
#include "usb_mux.h"
#include "usbc_ppc.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_USBCHARGE, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_USBCHARGE, format, ## args)

#define PDO_FIXED_FLAGS (PDO_FIXED_DUAL_ROLE | PDO_FIXED_DATA_SWAP |\
			 PDO_FIXED_COMM_CAP)

const uint32_t pd_src_pdo[] = {
	PDO_FIXED(5000, 1500, PDO_FIXED_FLAGS),
};
const int pd_src_pdo_cnt = ARRAY_SIZE(pd_src_pdo);
const uint32_t pd_src_pdo_max[] = {
		PDO_FIXED(5000, 3000, PDO_FIXED_FLAGS),
};
const int pd_src_pdo_max_cnt = ARRAY_SIZE(pd_src_pdo_max);

const uint32_t pd_snk_pdo[] = {
	PDO_FIXED(5000, 500, PDO_FIXED_FLAGS),
	PDO_BATT(4750, 21000, 15000),
	PDO_VAR(4750, 21000, 3000),
};
const int pd_snk_pdo_cnt = ARRAY_SIZE(pd_snk_pdo);

int pd_board_checks(void)
{
	return EC_SUCCESS;
}

int pd_check_data_swap(int port, int data_role)
{
	/*
	 * Allow data swap if we are a UFP, otherwise don't allow.
	 *
	 * When we are still in the Read-Only firmware, avoid swapping roles
	 * so we don't jump in RW as a SNK/DFP and potentially confuse the
	 * power supply by sending a soft-reset with wrong data role.
	 */

	return (data_role == PD_ROLE_UFP) &&
	       (system_get_image_copy() != SYSTEM_IMAGE_RO) ? 1 : 0;
}

void pd_check_dr_role(int port, int dr_role, int flags)
{
	/* If UFP, try to switch to DFP */
	if ((flags & PD_FLAGS_PARTNER_DR_DATA) &&
			dr_role == PD_ROLE_UFP &&
			system_get_image_copy() != SYSTEM_IMAGE_RO)
		pd_request_data_swap(port);
}

int pd_check_power_swap(int port)
{
	/*
	 * Allow power swap as long as we are acting as a dual role device,
	 * otherwise assume our role is fixed (not in S0 or console command
	 * to fix our role).
	 */
	return pd_get_dual_role(port) == PD_DRP_TOGGLE_ON ? 1 : 0;
}

void pd_check_pr_role(int port, int pr_role, int flags)
{
	/*
	 * If partner is dual-role power and dualrole toggling is on, consider
	 * if a power swap is necessary.
	 */
	if ((flags & PD_FLAGS_PARTNER_DR_POWER) &&
	    pd_get_dual_role(port) == PD_DRP_TOGGLE_ON) {
		/*
		 * If we are a sink and partner is not externally powered, then
		 * swap to become a source. If we are source and partner is
		 * externally powered, swap to become a sink.
		 */
		int partner_extpower = flags & PD_FLAGS_PARTNER_EXTPOWER;

		if ((!partner_extpower && pr_role == PD_ROLE_SINK) ||
		     (partner_extpower && pr_role == PD_ROLE_SOURCE))
			pd_request_power_swap(port);
	}
}

int pd_check_vconn_swap(int port)
{
	/* In G3, do not allow vconn swap since PP5000 rail is off */
	return gpio_get_level(GPIO_EN_PP5000);
}

void pd_execute_data_swap(int port, int data_role)
{
	/* Both ports don't support device mode. Should not reach here. */
}

int pd_is_valid_input_voltage(int mv)
{
	return 1;
}

static uint8_t vbus_en[CONFIG_USB_PD_PORT_MAX_COUNT];
static uint8_t vbus_rp[CONFIG_USB_PD_PORT_MAX_COUNT] = {TYPEC_RP_1A5,
							TYPEC_RP_1A5};

static void board_vbus_update_source_current(int port)
{
	/* Both port are controlled by PPC SN5S330. */
	ppc_set_vbus_source_current_limit(port, vbus_rp[port]);
	ppc_vbus_source_enable(port, vbus_en[port]);
}

void pd_power_supply_reset(int port)
{
	int prev_en;

	prev_en = vbus_en[port];

	/* Disable VBUS */
	vbus_en[port] = 0;
	board_vbus_update_source_current(port);

	/* Enable discharge if we were previously sourcing 5V */
	if (prev_en)
		pd_set_vbus_discharge(port, 1);

#ifdef CONFIG_USB_PD_MAX_SINGLE_SOURCE_CURRENT
	/* Give back the current quota we are no longer using */
	charge_manager_source_port(port, 0);
#endif /* defined(CONFIG_USB_PD_MAX_SINGLE_SOURCE_CURRENT) */

	/* notify host of power info change */
	pd_send_host_event(PD_EVENT_POWER_CHANGE);
}

int pd_set_power_supply_ready(int port)
{
	/* Disable charging */
	board_vbus_sink_enable(port, 0);

	pd_set_vbus_discharge(port, 0);

	/* Provide VBUS */
	vbus_en[port] = 1;
	board_vbus_update_source_current(port);

	/* Ensure we advertise the proper available current quota */
	charge_manager_source_port(port, 1);

	/* notify host of power info change */
	pd_send_host_event(PD_EVENT_POWER_CHANGE);

	return EC_SUCCESS; /* we are ready */
}

void pd_transition_voltage(int idx)
{
	/* No-operation: we are always 5V */
}

int board_vbus_source_enabled(int port)
{
	return vbus_en[port];
}

void typec_set_source_current_limit(int port, enum tcpc_rp_value rp)
{
	vbus_rp[port] = rp;
	board_vbus_update_source_current(port);
}

int pd_snk_is_vbus_provided(int port)
{
	return !gpio_get_level(port ? GPIO_USB_C1_VBUS_DET_L :
				      GPIO_USB_C0_VBUS_DET_L);
}

/* ----------------- Vendor Defined Messages ------------------ */
const struct svdm_response svdm_rsp = {
	.identity = NULL,
	.svids = NULL,
	.modes = NULL,
};

int pd_custom_vdm(int port, int cnt, uint32_t *payload,
		  uint32_t **rpayload)
{
	int cmd = PD_VDO_CMD(payload[0]);
	uint16_t dev_id = 0;
	int is_rw, is_latest;

	/* make sure we have some payload */
	if (cnt == 0)
		return 0;

	switch (cmd) {
	case VDO_CMD_VERSION:
		/* guarantee last byte of payload is null character */
		*(payload + cnt - 1) = 0;
		CPRINTF("version: %s\n", (char *)(payload+1));
		break;
	case VDO_CMD_READ_INFO:
	case VDO_CMD_SEND_INFO:
		/* copy hash */
		if (cnt == 7) {
			dev_id = VDO_INFO_HW_DEV_ID(payload[6]);
			is_rw = VDO_INFO_IS_RW(payload[6]);

			is_latest = pd_dev_store_rw_hash(port,
							 dev_id,
							 payload + 1,
							  is_rw ?
							 SYSTEM_IMAGE_RW :
							 SYSTEM_IMAGE_RO);
			/*
			 * Send update host event unless our RW hash is
			 * already known to be the latest update RW.
			 */
			if (!is_rw || !is_latest)
				pd_send_host_event(PD_EVENT_UPDATE_DEVICE);

			CPRINTF("DevId:%d.%d SW:%d RW:%d\n",
				HW_DEV_ID_MAJ(dev_id),
				HW_DEV_ID_MIN(dev_id),
				VDO_INFO_SW_DBG_VER(payload[6]),
				is_rw);
		} else if (cnt == 6) {
			/* really old devices don't have last byte */
			pd_dev_store_rw_hash(port, dev_id, payload + 1,
					     SYSTEM_IMAGE_UNKNOWN);
		}
		break;
	case VDO_CMD_CURRENT:
		CPRINTF("Current: %dmA\n", payload[1]);
		break;
	case VDO_CMD_FLIP:
		usb_mux_flip(port);
		break;
#ifdef CONFIG_USB_PD_LOGGING
	case VDO_CMD_GET_LOG:
		pd_log_recv_vdm(port, cnt, payload);
		break;
#endif /* CONFIG_USB_PD_LOGGING */
	}

	return 0;
}

#ifdef CONFIG_USB_PD_ALT_MODE_DFP
static int dp_flags[CONFIG_USB_PD_PORT_MAX_COUNT];
static uint32_t dp_status[CONFIG_USB_PD_PORT_MAX_COUNT];

static void svdm_safe_dp_mode(int port)
{
	/* make DP interface safe until configure */
	dp_flags[port] = 0;
	dp_status[port] = 0;
	usb_mux_set(port, TYPEC_MUX_NONE,
		    USB_SWITCH_CONNECT, pd_get_polarity(port));
}

static int svdm_enter_dp_mode(int port, uint32_t mode_caps)
{
	/* Only enter mode if device is DFP_D capable */
	if (mode_caps & MODE_DP_SNK) {
		svdm_safe_dp_mode(port);
		return 0;
	}

	return -1;
}

static int svdm_dp_status(int port, uint32_t *payload)
{
	int opos = pd_alt_mode(port, USB_SID_DISPLAYPORT);

	payload[0] = VDO(USB_SID_DISPLAYPORT, 1,
			 CMD_DP_STATUS | VDO_OPOS(opos));
	payload[1] = VDO_DP_STATUS(0, /* HPD IRQ  ... not applicable */
				   0, /* HPD level ... not applicable */
				   0, /* exit DP? ... no */
				   0, /* usb mode? ... no */
				   0, /* multi-function ... no */
				   (!!(dp_flags[port] & DP_FLAGS_DP_ON)),
				   0, /* power low? ... no */
				   (!!(dp_flags[port] & DP_FLAGS_DP_ON)));
	return 2;
};

static int svdm_dp_config(int port, uint32_t *payload)
{
	int opos = pd_alt_mode(port, USB_SID_DISPLAYPORT);
	int pin_mode = pd_dfp_dp_get_pin_mode(port, dp_status[port]);

	if (!pin_mode)
		return 0;

	payload[0] = VDO(USB_SID_DISPLAYPORT, 1,
			 CMD_DP_CONFIG | VDO_OPOS(opos));
	payload[1] = VDO_DP_CFG(pin_mode,      /* pin mode */
				1,             /* DPv1.3 signaling */
				2);            /* UFP connected */
	return 2;
};

static void svdm_dp_post_config(int port)
{
	dp_flags[port] |= DP_FLAGS_DP_ON;
}

/**
 * Is the port fine to be muxed its DisplayPort lines?
 *
 * Only one port can be muxed to DisplayPort at a time.
 *
 * @param port	Port number of TCPC.
 * @return	1 is fine; 0 is bad as other port is already muxed;
 */
static int is_dp_muxable(int port)
{
	int i;

	for (i = 0; i < CONFIG_USB_PD_PORT_MAX_COUNT; i++)
		if (i != port) {
			if (usb_mux_get(i) & USB_PD_MUX_DP_ENABLED)
				return 0;
		}

	return 1;
}

static int svdm_dp_attention(int port, uint32_t *payload)
{
	int lvl = PD_VDO_DPSTS_HPD_LVL(payload[1]);
	int irq = PD_VDO_DPSTS_HPD_IRQ(payload[1]);
	const struct usb_mux *mux = &usb_muxes[port];

	dp_status[port] = payload[1];

	/*
	 * Initial implementation to handle HPD. Only the first-plugged port
	 * works, i.e. sending HPD signal to AP. The second-plugged port
	 * will be ignored.
	 *
	 * TODO(waihong): Continue the above case, if the first-plugged port
	 * is then unplugged, switch to the second-plugged port and signal AP?
	 */
	if (lvl) {
		if (is_dp_muxable(port)) {
			/*
			 * TODO(waihong): Better to move switching DP mux to
			 * the usb_mux abstraction.
			 */
			gpio_set_level(GPIO_DP_MUX_SEL, port == 1);
			gpio_set_level(GPIO_DP_MUX_OE_L, 0);

			/*
			 * When mf_pref not true, still use the dock muxing
			 * because of the board USB-C topology.
			 */
			usb_mux_set(port, TYPEC_MUX_DOCK,
				    USB_SWITCH_CONNECT, pd_get_polarity(port));
		} else {
			/* TODO(waihong): Info user? */
			CPRINTS("p%d: The other port is already muxed.", port);
			return 0;  /* Nack */
		}
	} else {
		gpio_set_level(GPIO_DP_MUX_OE_L, 1);
		usb_mux_set(port, TYPEC_MUX_USB,
			    USB_SWITCH_CONNECT, pd_get_polarity(port));
	}

	/* Signal AP for the HPD event */
	mux->hpd_update(port, lvl, irq);

	return 1;  /* Ack */
}

static void svdm_exit_dp_mode(int port)
{
	const struct usb_mux *mux = &usb_muxes[port];

	svdm_safe_dp_mode(port);
	mux->hpd_update(port, 0, 0);
}

static int svdm_enter_gfu_mode(int port, uint32_t mode_caps)
{
	/* Always enter GFU mode */
	return 0;
}

static void svdm_exit_gfu_mode(int port)
{
}

static int svdm_gfu_status(int port, uint32_t *payload)
{
	/*
	 * This is called after enter mode is successful, send unstructured
	 * VDM to read info.
	 */
	pd_send_vdm(port, USB_VID_GOOGLE, VDO_CMD_READ_INFO, NULL, 0);
	return 0;
}

static int svdm_gfu_config(int port, uint32_t *payload)
{
	return 0;
}

static int svdm_gfu_attention(int port, uint32_t *payload)
{
	return 0;
}

const struct svdm_amode_fx supported_modes[] = {
	{
		.svid = USB_SID_DISPLAYPORT,
		.enter = &svdm_enter_dp_mode,
		.status = &svdm_dp_status,
		.config = &svdm_dp_config,
		.post_config = &svdm_dp_post_config,
		.attention = &svdm_dp_attention,
		.exit = &svdm_exit_dp_mode,
	},
	{
		.svid = USB_VID_GOOGLE,
		.enter = &svdm_enter_gfu_mode,
		.status = &svdm_gfu_status,
		.config = &svdm_gfu_config,
		.attention = &svdm_gfu_attention,
		.exit = &svdm_exit_gfu_mode,
	}
};
const int supported_modes_cnt = ARRAY_SIZE(supported_modes);
#endif /* CONFIG_USB_PD_ALT_MODE_DFP */
