/*
 * Retu/Tahvo MFD driver interface
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License. See the file "COPYING" in the main directory of this
 * archive for more details.
 */

#ifndef __LINUX_MFD_RETU_H
#define __LINUX_MFD_RETU_H

struct retu_dev;

struct regmap *retu_get_regmap(struct retu_dev *);

int retu_read(struct retu_dev *, u8);
int retu_write(struct retu_dev *, u8, u16);

/* Registers */
#define RETU_REG_ASICR		0x00		/* ASIC ID and revision */
#define RETU_REG_ASICR_VILMA	(1 << 7)	/* Bit indicating Vilma */
#define RETU_REG_IDR		0x01		/* Interrupt ID */
#define RETU_REG_IMR		0x02		/* Interrupt mask (Retu) */
#define RETU_REG_RTCDSR		0x03		/* RTC seconds register */
#define RETU_REG_RTCHMR		0x04		/* RTC hours and minutes register */
#define RETU_REG_RTCHMAR	0x05		/* RTC hours and minutes alarm and time set register */
#define RETU_REG_RTCCALR	0x06		/* RTC calibration register */
#define RETU_REG_ADCR		0x08		/* ADC result */
#define RETU_REG_ADCSCR		0x09		/* ADC sample ctrl */
#define RETU_REG_CC1		0x0d		/* Common control register 1 */
#define RETU_REG_CC2		0x0e		/* Common control register 2 */
#define RETU_REG_CTRL_CLR	0x0f		/* Regulator clear register */
#define RETU_REG_CTRL_SET	0x10		/* Regulator set register */
#define RETU_REG_STATUS		0x16		/* Status register */
#define  RETU_REG_STATUS_BATAVAIL	0x0100	/* Battery available */
#define  RETU_REG_STATUS_CHGPLUG	0x1000	/* Charger is plugged in */
#define RETU_REG_WATCHDOG	0x17		/* Watchdog */

#define TAHVO_REG_IMR		0x03		/* Interrupt mask (Tahvo) */
#define TAHVO_REG_LEDPWM	0x05
#define TAHVO_REG_VCORE		0x07

/* Interrupt sources */
#define RETU_INT_PWR		0		/* Power button */
#define RETU_INT_CHAR		1
#define RETU_INT_RTCS		2
#define RETU_INT_RTCM		3
#define RETU_INT_RTCD		4
#define RETU_INT_RTCA		5
#define RETU_INT_HOOK		6
#define RETU_INT_HEAD		7
#define RETU_INT_ADCS		8
#define TAHVO_INT_VBUS		0		/* VBUS state */

/* Interrupt status */
#define TAHVO_STAT_VBUS		(1 << TAHVO_INT_VBUS)

#endif /* __LINUX_MFD_RETU_H */
