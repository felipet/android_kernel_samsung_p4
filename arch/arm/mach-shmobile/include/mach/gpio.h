/*
 * Generic GPIO API and pinmux table support
 *
 * Copyright (c) 2008  Magnus Damm
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#ifndef __ASM_ARCH_GPIO_H
#define __ASM_ARCH_GPIO_H

#include <linux/kernel.h>
#include <linux/errno.h>

#define ARCH_NR_GPIOS 1024
#include <linux/sh_pfc.h>

#ifdef CONFIG_GPIOLIB

#define __ARM_GPIOLIB_TRIVIAL

static inline int gpio_to_irq(unsigned gpio)
{
	return __gpio_to_irq(gpio);
}

static inline int irq_to_gpio(unsigned int irq)
{
	return -ENOSYS;
}

#endif /* CONFIG_GPIOLIB */

#endif /* __ASM_ARCH_GPIO_H */
