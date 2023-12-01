/*
 * Copyright (c) 2023, STMicroelectronics - All Rights Reserved
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT st_stm32_gpio

#include <lib/utils_def.h>
#include <lib/mmio.h>

#include <pinctrl.h>
#include <clk.h>
#include <devicetree/gpio.h>
#include <stm32_gpio.h>

#define GPIO_MODE_OFFSET	U(0x00)
#define GPIO_TYPE_OFFSET	U(0x04)
#define GPIO_SPEED_OFFSET	U(0x08)
#define GPIO_PUPD_OFFSET	U(0x0C)
#define GPIO_BSRR_OFFSET	U(0x18)
#define GPIO_AFRL_OFFSET	U(0x20)
#define GPIO_AFRH_OFFSET	U(0x24)
#define GPIO_SECR_OFFSET	U(0x30)

#define GPIO_ALTERNATE_MASK	U(0x0F)
#define GPIO_ALT_LOWER_LIMIT	U(0x08)

struct stm32_gpio_config {
	uintptr_t base;
	const struct device *clk_dev;
	const clk_subsys_t clk_subsys;
	int gpio_offset;
	int pinctrl_offset;
	int npins;
};

static void _stm32_gpio_set_mode(uintptr_t base, struct gpio_cfg *gpio)
{
	mmio_clrbits_32(base + GPIO_MODE_OFFSET,
			((uint32_t)GPIO_MODE_MASK << (gpio->pin << 1)));
	mmio_setbits_32(base + GPIO_MODE_OFFSET,
			(gpio->mode & ~GPIO_OPEN_DRAIN) << (gpio->pin << 1));
}

static void _stm32_gpio_set_type(uintptr_t base, struct gpio_cfg *gpio)
{
	if (gpio->type & GPIO_OPEN_DRAIN) {
		mmio_setbits_32(base + GPIO_TYPE_OFFSET, BIT(gpio->pin));
	} else {
		mmio_clrbits_32(base + GPIO_TYPE_OFFSET, BIT(gpio->pin));
	}
}

static void _stm32_gpio_set_speed(uintptr_t base, struct gpio_cfg *gpio)
{
	mmio_clrbits_32(base + GPIO_SPEED_OFFSET,
			((uint32_t)GPIO_SPEED_MASK << (gpio->pin << 1)));
	mmio_setbits_32(base + GPIO_SPEED_OFFSET, gpio->speed << (gpio->pin << 1));
}

static void _stm32_gpio_set_pupd(uintptr_t base, struct gpio_cfg *gpio)
{
	mmio_clrbits_32(base + GPIO_PUPD_OFFSET,
			((uint32_t)GPIO_PULL_MASK << (gpio->pin << 1)));
	mmio_setbits_32(base + GPIO_PUPD_OFFSET, gpio->pull << (gpio->pin << 1));
}

static void _stm32_gpio_set_altx(uintptr_t base, struct gpio_cfg *gpio)
{
	if (gpio->pin < GPIO_ALT_LOWER_LIMIT) {
		mmio_clrbits_32(base + GPIO_AFRL_OFFSET,
				((uint32_t)GPIO_ALTERNATE_MASK << (gpio->pin << 2)));
		mmio_setbits_32(base + GPIO_AFRL_OFFSET,
				gpio->alternate << (gpio->pin << 2));
	} else {
		mmio_clrbits_32(base + GPIO_AFRH_OFFSET,
				((uint32_t)GPIO_ALTERNATE_MASK <<
				 ((gpio->pin - GPIO_ALT_LOWER_LIMIT) << 2)));
		mmio_setbits_32(base + GPIO_AFRH_OFFSET,
				gpio->alternate << ((gpio->pin - GPIO_ALT_LOWER_LIMIT) <<
					      2));
	}
}

static int stm32_gpio_set(const struct device *dev, struct gpio_cfg *gpio)
{
	const struct stm32_gpio_config *cfg = dev_get_config(dev);
	uintptr_t base = cfg->base;
	struct clk *clk;
	int err;

	clk = clk_get(cfg->clk_dev, cfg->clk_subsys);
	if (!clk)
		return -EINVAL;

	err = clk_enable(clk);
	if (err)
		return err;

	_stm32_gpio_set_mode(base, gpio);
	_stm32_gpio_set_type(base, gpio);
	_stm32_gpio_set_speed(base, gpio);
	_stm32_gpio_set_pupd(base, gpio);
	_stm32_gpio_set_altx(base, gpio);

	clk_disable(clk);

	return 0;
}

static int stm32_gpio_pin_is_valid(const struct device *dev, int pin_no)
{
	const struct stm32_gpio_config *cfg = dev_get_config(dev);

	pin_no -= cfg->pinctrl_offset;
	if (pin_no < 0 || pin_no >= cfg->npins)
		return 0;

	return 1;
}

#define STM32_GPIO_INIT(n)						\
static const struct stm32_gpio_config stm32_gpio_cfg_##n = {		\
	.base = DT_INST_REG_ADDR(n),					\
	.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),		\
	.clk_subsys = (clk_subsys_t) DT_INST_CLOCKS_CELL(n, bits),	\
	.gpio_offset = DT_INST_GPIO_RANGES_CELL(n, gpio_offset),	\
	.pinctrl_offset = DT_INST_GPIO_RANGES_CELL(n, pin_offset),	\
	.npins = DT_INST_GPIO_RANGES_CELL(n, npins),			\
};									\
									\
DEVICE_DT_INST_DEFINE(n,						\
		      NULL,						\
		      NULL, &stm32_gpio_cfg_##n,			\
		      CORE, 0,						\
		      NULL);

DT_INST_FOREACH_STATUS_OKAY(STM32_GPIO_INIT)

// Array containing pointers to each GPIO port.
#define DEFINE_STM32PORT_DEVICE(n) DEVICE_DT_INST_GET(n),

static const struct device *port_devices[] = {
	DT_INST_FOREACH_STATUS_OKAY(DEFINE_STM32PORT_DEVICE)
};

int stm32_pinctrl_to_gpio(const pinctrl_soc_pin_t *pin, struct gpio_cfg *gpio)
{
	uint32_t func;

	gpio->speed = _FLD_GET(_PINCFG_OSPEED, pin->pincfg);
	gpio->pull = _FLD_GET(_PINCFG_PUPD, pin->pincfg);
	gpio->type = _FLD_GET(_PINCFG_OTYPE, pin->pincfg);
	gpio->pin = _FLD_GET(_PINMUX_PIN, pin->pinmux);
	gpio->alternate = 0;

	func = _FLD_GET(_PINMUX_FUNC, pin->pinmux);

	switch (func) {
	case 0:
		/* input or ouput */
		gpio->mode = _FLD_GET(_PINCFG_MODE, pin->pincfg);
		break;
	case 1 ... 16:
		gpio->alternate = func - 1U;
		gpio->mode = GPIO_MODE_ALTERNATE;
	case 17:
		gpio->mode = GPIO_MODE_ANALOG;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

const struct device *stm32_pinctrl_get_port(int pin_no)
{
	int i;

	for(i = 0; i < ARRAY_SIZE(port_devices); i++) {
		if (stm32_gpio_pin_is_valid(port_devices[i], pin_no))
			return port_devices[i];
	}

	return NULL;
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt)
{
	struct gpio_cfg gpio;
	int i, err;

	for (i = 0; i < pin_cnt; i++) {
		const pinctrl_soc_pin_t *pin = &pins[i];
		int pin_no = _FLD_GET(_PINMUX_PINNO, pin->pinmux);
		const struct device *port;

		err = stm32_pinctrl_to_gpio(pin, &gpio);
		if (err)
			return err;

		port = stm32_pinctrl_get_port(pin_no);
		if (!port)
			return -EINVAL;

                err = stm32_gpio_set(port, &gpio);
                if (err)
			return err;
	}

	return 0;
}
