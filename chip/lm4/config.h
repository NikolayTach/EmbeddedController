/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __CROS_EC_CHIP_CONFIG_H
#define __CROS_EC_CHIP_CONFIG_H

/* 16.000 Mhz internal oscillator frequency (PIOSC) */
#define INTERNAL_CLOCK 16000000

/* Number of IRQ vectors on the NVIC */
#define CONFIG_IRQ_COUNT 132

/* Debug UART parameters for panic message */
#define CONFIG_UART_ADDRESS    0x4000c000
#define CONFIG_UART_DR_OFFSET  0x00
#define CONFIG_UART_SR_OFFSET  0x18
#define CONFIG_UART_SR_TXEMPTY 0x80

/****************************************************************************/
/* Memory mapping */

#define CONFIG_RAM_BASE             0x20000000
#define CONFIG_RAM_SIZE             0x00008000

/* System stack size */
#define CONFIG_STACK_SIZE           4096

#define CONFIG_FLASH_BASE           0x00000000
#define CONFIG_FLASH_BANK_SIZE      0x00000800  /* protect bank size */

/* This is the physical size of the flash on the chip. We'll reserve one bank
 * in order to emulate per-bank write-protection UNTIL REBOOT. The hardware
 * doesn't support a write-protect pin, and if we make the write-protection
 * permanent, it can't be undone easily enough to support RMA. */
#define CONFIG_FLASH_PHYSICAL_SIZE  0x00040000

/* This is the size that we pretend we have. This is what flashrom expects,
 * what the FMAP reports, and what size we build images for. */
#define CONFIG_FLASH_SIZE (CONFIG_FLASH_PHYSICAL_SIZE - CONFIG_FLASH_BANK_SIZE)

/****************************************************************************/
/* Define our flash layout. */

/* The EC needs its own region for run-time vboot stuff. We can put
 * that up at the top */
#define CONFIG_SECTION_ROLLBACK_SIZE  (1 * CONFIG_FLASH_BANK_SIZE)
#define CONFIG_SECTION_ROLLBACK_OFF   (CONFIG_FLASH_SIZE \
					- CONFIG_FLASH_ROLLBACK_SIZE)

/* Then there are the three major sections. */
#define CONFIG_SECTION_RO_SIZE	    (40 * CONFIG_FLASH_BANK_SIZE)
#define CONFIG_SECTION_RO_OFF       CONFIG_FLASH_BASE

#define CONFIG_SECTION_A_SIZE       (40 * CONFIG_FLASH_BANK_SIZE)
#define CONFIG_SECTION_A_OFF        (CONFIG_SECTION_RO_OFF \
					+ CONFIG_SECTION_RO_SIZE)

#define CONFIG_SECTION_B_SIZE       (40 * CONFIG_FLASH_BANK_SIZE)
#define CONFIG_SECTION_B_OFF        (CONFIG_SECTION_A_OFF \
					+ CONFIG_SECTION_A_SIZE)

/* The top of each section will hold the vboot stuff, since the firmware vector
 * table has to go at the start. The root key will fit in 2K, but the vblocks
 * need 4K. */
#define CONFIG_VBOOT_ROOTKEY_SIZE   0x800
#define CONFIG_VBLOCK_SIZE          0x1000

/* RO: firmware (+ FMAP), root keys */
#define CONFIG_FW_RO_OFF            CONFIG_SECTION_RO_OFF
#define CONFIG_FW_RO_SIZE           (CONFIG_SECTION_RO_SIZE \
					- CONFIG_VBOOT_ROOTKEY_SIZE)
#define CONFIG_VBOOT_ROOTKEY_OFF    (CONFIG_FW_RO_OFF + CONFIG_FW_RO_SIZE)

/* A: firmware, vblock */
#define CONFIG_FW_A_OFF             CONFIG_SECTION_A_OFF
#define CONFIG_FW_A_SIZE            (CONFIG_SECTION_A_SIZE \
					- CONFIG_VBLOCK_SIZE)
#define CONFIG_VBLOCK_A_OFF         (CONFIG_FW_A_OFF + CONFIG_FW_A_SIZE)

/* B: firmware, vblock */
#define CONFIG_FW_B_SIZE            (CONFIG_SECTION_B_SIZE \
					- CONFIG_VBLOCK_SIZE)
#define CONFIG_FW_B_OFF             (CONFIG_SECTION_A_OFF \
					+ CONFIG_SECTION_A_SIZE)
#define CONFIG_VBLOCK_B_OFF         (CONFIG_FW_B_OFF + CONFIG_FW_B_SIZE)


/****************************************************************************/
/* Customize the build */

/* Build with assertions and debug messages */
#define CONFIG_DEBUG

/* Optional features present on this chip */
#define CONFIG_ADC
#define CONFIG_EEPROM
#define CONFIG_FLASH
#define CONFIG_VBOOT
#define CONFIG_FPU
#define CONFIG_I2C

/* Compile for running from RAM instead of flash */
/* #define COMPILE_FOR_RAM */

#endif  /* __CROS_EC_CHIP_CONFIG_H */
