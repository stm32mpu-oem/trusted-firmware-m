// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-3-Clause)
/*
 * Copyright (C) 2018-2022, STMicroelectronics - All Rights Reserved
 */
#define DT_DRV_COMPAT st_stm32mp25_rcc

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <lib/mmio.h>
#include <lib/utils_def.h>
#include <stdint.h>
#include <lib/timeout.h>
#include <lib/delay.h>
#include <debug.h>
#include <clk.h>
#include <stm32mp_clkfunc.h>
#include <stm32mp25_clk.h>
#include <stm32mp25_rcc.h>

#include <dt-bindings/clock/stm32mp25-clksrc.h>
#include <dt-bindings/clock/stm32mp25-clks.h>
#include <dt-bindings/rif/stm32mp25-rifsc.h>

#include "clk-stm32-core.h"

#define __WORD_BIT 32

#define TIMEOUT_US_100MS			U(100000)
#define TIMEOUT_US_200MS			U(200000)
#define TIMEOUT_US_1S				U(1000000)

#define PLLRDY_TIMEOUT		TIMEOUT_US_200MS
#define CLKSRC_TIMEOUT		TIMEOUT_US_200MS
#define CLKDIV_TIMEOUT		TIMEOUT_US_200MS
#define OSCRDY_TIMEOUT		TIMEOUT_US_1S

#define XBAR_CHANNEL_NB				U(64)
#define XBAR_ROOT_CHANNEL_NB			U(7)

/* PLL configuration registers offsets from RCC_PLLxCFGR1 */
#define RCC_OFFSET_PLLXCFGR1		0x00
#define RCC_OFFSET_PLLXCFGR2		0x04
#define RCC_OFFSET_PLLXCFGR3		0x08
#define RCC_OFFSET_PLLXCFGR4		0x0C
#define RCC_OFFSET_PLLXCFGR5		0x10
#define RCC_OFFSET_PLLXCFGR6		0x18
#define RCC_OFFSET_PLLXCFGR7		0x1C

/* PLL minimal frequencies for clock sources */
#define PLL_REFCLK_MIN			UL(5000000)
#define PLL_FRAC_REFCLK_MIN		UL(10000000)

/* RCC_PLLxCFGR1 register fields */
#define RCC_PLLxCFGR1_SSMODRST			BIT(0)
#define RCC_PLLxCFGR1_PLLEN			BIT(8)
#define RCC_PLLxCFGR1_PLLRDY			BIT(24)
#define RCC_PLLxCFGR1_CKREFST			BIT(28)

/* RCC_PLLxCFGR2 register fields */
#define RCC_PLLxCFGR2_FREFDIV_MASK		GENMASK_32(5, 0)
#define RCC_PLLxCFGR2_FREFDIV_SHIFT		0
#define RCC_PLLxCFGR2_FBDIV_MASK		GENMASK_32(27, 16)
#define RCC_PLLxCFGR2_FBDIV_SHIFT		16

/* RCC_PLLxCFGR3 register fields */
#define RCC_PLLxCFGR3_FRACIN_MASK		GENMASK_32(23, 0)
#define RCC_PLLxCFGR3_FRACIN_SHIFT		0
#define RCC_PLLxCFGR3_DOWNSPREAD		BIT(24)
#define RCC_PLLxCFGR3_DACEN			BIT(25)
#define RCC_PLLxCFGR3_SSCGDIS			BIT(26)

/* RCC_PLLxCFGR4 register fields */
#define RCC_PLLxCFGR4_DSMEN			BIT(8)
#define RCC_PLLxCFGR4_FOUTPOSTDIVEN		BIT(9)
#define RCC_PLLxCFGR4_BYPASS			BIT(10)

/* RCC_PLLxCFGR5 register fields */
#define RCC_PLLxCFGR5_DIVVAL_MASK		GENMASK_32(3, 0)
#define RCC_PLLxCFGR5_DIVVAL_SHIFT		0
#define RCC_PLLxCFGR5_SPREAD_MASK		GENMASK_32(20, 16)
#define RCC_PLLxCFGR5_SPREAD_SHIFT		16

/* RCC_PLLxCFGR6 register fields */
#define RCC_PLLxCFGR6_POSTDIV1_MASK		GENMASK_32(2, 0)
#define RCC_PLLxCFGR6_POSTDIV1_SHIFT		0

/* RCC_PLLxCFGR7 register fields */
#define RCC_PLLxCFGR7_POSTDIV2_MASK		GENMASK_32(2, 0)
#define RCC_PLLxCFGR7_POSTDIV2_SHIFT		0

#define RCC_0_MHZ	UL(0)
#define RCC_4_MHZ	UL(4000000)
#define RCC_16_MHZ	UL(16000000)

/*
 * CIDCFGR register bitfields
 */
#define RCC_CIDCFGR_SEMWL_MASK		GENMASK_32(23, 16)
#define RCC_CIDCFGR_SCID_MASK		GENMASK_32(6, 4)
#define RCC_CIDCFGR_CONF_MASK		(_CIDCFGR_CFEN |	\
					 _CIDCFGR_SEMEN |	\
					 RCC_CIDCFGR_SCID_MASK |\
					 RCC_CIDCFGR_SEMWL_MASK)

/*
 * PRIVCFGR register bitfields
 */
#define RCC_PRIVCFGR_MASK		GENMASK_32(31, 0)

/*
 * RCFGLOCKR register bitfields
 */
#define RCC_RCFGLOCKR_MASK		GENMASK_32(31, 0)

/*
 * SECCFGR register bitfields
 */
#define RCC_SECCFGR_EN			BIT(0)
#define RCC_SECCFGR_MASK		GENMASK_32(31, 0)

/*
 * SEMCR register bitfields
 */
#define RCC_SEMCR_SCID_MASK		GENMASK_32(6, 4)
#define RCC_SEMCR_SCID_SHIFT		U(4)

/*
 * RIF miscellaneous
 */
#define RCC_NB_RIF_RES			U(114)
#define RCC_NB_CONFS			((RCC_NB_RIF_RES / 32) + 1)

#define RCC_NB_MAX_CID_SUPPORTED	U(7)

/* Register: RCC_RxCIDCFGR */
#define RCC_CIDCFGR_CFEN		BIT(0)
#define RCC_CIDCFGR_SEM_EN		BIT(1)
#define RCC_CIDCFGR_SCID_SHIFT		4
#define RCC_SEMWL_SHIFT			6

/* Compartiment IDs */
#define RIF_CID0		0x0
#define RIF_CID1		0x1
#define RIF_CID2		0x2

/* RCC RIF resource IDs  */
#define RCC_RIF_PLL4_TO_8	64
#define RCC_RIF_FCAL		65
#define RCC_RIF_SYSTEM_RESET	66
#define RCC_RIF_RCC_CPU_BOOT	67
#define RCC_RIF_RESET_DURATION	68
#define RCC_RIF_OSCILLATORS	69
#define RCC_RIF_DEBUG_TRACE	73
#define RCC_RIF_SYSRAM		74
#define RCC_RIF_VDERAM		75
#define RCC_RIF_RETRAM		76
#define RCC_RIF_BKPSRAM		77
#define RCC_RIF_SRAM1		78
#define RCC_RIF_SRAM2		79
#define RCC_RIF_LPSRAM1		80
#define RCC_RIF_LPSRAM2		81
#define RCC_RIF_LPSRAM3		82
#define RCC_RIF_HPDMA1		83
#define RCC_RIF_HPDMA2		84
#define RCC_RIF_HPDMA3		85
#define RCC_RIF_LPDMA		86
#define RCC_RIF_IPCC1		87
#define RCC_RIF_IPCC2		88
#define RCC_RIF_HSEM		89
#define RCC_RIF_GPIOA		90
#define RCC_RIF_GPIOB		91
#define RCC_RIF_GPIOC		92
#define RCC_RIF_GPIOD		93
#define RCC_RIF_GPIOE		94
#define RCC_RIF_GPIOF		95
#define RCC_RIF_GPIOG		96
#define RCC_RIF_GPIOH		97
#define RCC_RIF_GPIOI		98
#define RCC_RIF_GPIOJ		99
#define RCC_RIF_GPIOK		100
#define RCC_RIF_GPIOZ		101
#define RCC_RIF_RTC_TAMP	102
#define RCC_RIF_BSEC		103
#define RCC_RIF_DDR_PLL2	104
#define RCC_RIF_PLL3		105
#define RCC_RIF_SYSCPU1		106
#define RCC_RIF_IS2M		107
#define RCC_RIF_MCO1		108
#define RCC_RIF_MCO2		109
#define RCC_RIF_OSPI1		110
#define RCC_RIF_OSPI2		111
#define RCC_RIF_FMC		112
#define RCC_RIF_HSIFMON		113

/*
 * GATE CONFIG
 */

/* WARNING GATE_XXX_RDY MUST FOLOW GATE_XXX */
enum enum_gate_cfg {
	GATE_HSI,
	GATE_HSI_RDY,
	GATE_HSE,
	GATE_HSE_RDY,
	GATE_LSE,
	GATE_LSE_RDY,
	GATE_LSI,
	GATE_LSI_RDY,
	GATE_MSI,
	GATE_MSI_RDY,
	GATE_PLL1,
	GATE_PLL1_RDY,
	GATE_PLL2,
	GATE_PLL2_RDY,
	GATE_PLL3,
	GATE_PLL3_RDY,
	GATE_PLL4,
	GATE_PLL4_RDY,
	GATE_PLL5,
	GATE_PLL5_RDY,
	GATE_PLL6,
	GATE_PLL6_RDY,
	GATE_PLL7,
	GATE_PLL7_RDY,
	GATE_PLL8,
	GATE_PLL8_RDY,
	GATE_PLL4_CKREFST,
	GATE_PLL5_CKREFST,
	GATE_PLL6_CKREFST,
	GATE_PLL7_CKREFST,
	GATE_PLL8_CKREFST,
	GATE_HSEDIV2,
	GATE_APB1DIV_RDY,
	GATE_APB2DIV_RDY,
	GATE_APB3DIV_RDY,
	GATE_APB4DIV_RDY,
	GATE_APBDBGDIV_RDY,
	GATE_TIMG1PRE_RDY,
	GATE_TIMG2PRE_RDY,
	GATE_LSMCUDIV_RDY,
	GATE_RTCCK,
	GATE_C3,
	GATE_LPTIM3C3,
	GATE_LPTIM4C3,
	GATE_LPTIM5C3,
	GATE_SPI8C3,
	GATE_LPUART1C3,
	GATE_I2C8C3,
	GATE_ADF1C3,
	GATE_GPIOZC3,
	GATE_LPDMAC3,
	GATE_RTCC3,
	GATE_I3C4C3,
	GATE_MCO1,
	GATE_MCO2,
	GATE_DDRCP,
	GATE_DDRCAPB,
	GATE_DDRPHYCAPB,
	GATE_DDRPHYC,
	GATE_DDRCFG,
	GATE_SYSRAM,
	GATE_VDERAM,
	GATE_SRAM1,
	GATE_SRAM2,
	GATE_RETRAM,
	GATE_BKPSRAM,
	GATE_LPSRAM1,
	GATE_LPSRAM2,
	GATE_LPSRAM3,
	GATE_OSPI1,
	GATE_OSPI2,
	GATE_FMC,
	GATE_DBG,
	GATE_TRACE,
	GATE_STM500,
	GATE_ETR,
	GATE_GPIOA,
	GATE_GPIOB,
	GATE_GPIOC,
	GATE_GPIOD,
	GATE_GPIOE,
	GATE_GPIOF,
	GATE_GPIOG,
	GATE_GPIOH,
	GATE_GPIOI,
	GATE_GPIOJ,
	GATE_GPIOK,
	GATE_GPIOZ,
	GATE_HPDMA1,
	GATE_HPDMA2,
	GATE_HPDMA3,
	GATE_LPDMA,
	GATE_HSEM,
	GATE_IPCC1,
	GATE_IPCC2,
	GATE_RTC,
	GATE_SYSCPU1,
	GATE_BSEC,
	GATE_IS2M,
	GATE_HSIMON,
	GATE_TIM1,
	GATE_TIM2,
	GATE_TIM3,
	GATE_TIM4,
	GATE_TIM5,
	GATE_TIM6,
	GATE_TIM7,
	GATE_TIM8,
	GATE_TIM10,
	GATE_TIM11,
	GATE_TIM12,
	GATE_TIM13,
	GATE_TIM14,
	GATE_TIM15,
	GATE_TIM16,
	GATE_TIM17,
	GATE_TIM20,
	GATE_LPTIM1,
	GATE_LPTIM2,
	GATE_LPTIM3,
	GATE_LPTIM4,
	GATE_LPTIM5,
	GATE_SPI1,
	GATE_SPI2,
	GATE_SPI3,
	GATE_SPI4,
	GATE_SPI5,
	GATE_SPI6,
	GATE_SPI7,
	GATE_SPI8,
	GATE_SPDIFRX,
	GATE_USART1,
	GATE_USART2,
	GATE_USART3,
	GATE_UART4,
	GATE_UART5,
	GATE_USART6,
	GATE_UART7,
	GATE_UART8,
	GATE_UART9,
	GATE_LPUART1,
	GATE_I2C1,
	GATE_I2C2,
	GATE_I2C3,
	GATE_I2C4,
	GATE_I2C5,
	GATE_I2C6,
	GATE_I2C7,
	GATE_I2C8,
	GATE_SAI1,
	GATE_SAI2,
	GATE_SAI3,
	GATE_SAI4,
	GATE_MDF1,
	GATE_ADF1,
	GATE_FDCAN,
	GATE_HDP,
	GATE_ADC12,
	GATE_ADC3,
	GATE_ETH1MAC,
	GATE_ETH1,
	GATE_ETH1TX,
	GATE_ETH1RX,
	GATE_ETH1STP,
	GATE_ETH2MAC,
	GATE_ETH2,
	GATE_ETH2STP,
	GATE_ETH2TX,
	GATE_ETH2RX,
	GATE_USB2,
	GATE_USB2PHY1,
	GATE_USB2PHY2,
	GATE_USB3DRD,
	GATE_USB3PCIEPHY,
	GATE_PCIE,
	GATE_USBTC,
	GATE_ETHSWMAC,
	GATE_ETHSW,
	GATE_ETHSWREF,
	GATE_STGEN,
	GATE_SDMMC1,
	GATE_SDMMC2,
	GATE_SDMMC3,
	GATE_GPU,
	GATE_LTDC,
	GATE_DSI,
	GATE_LVDS,
	GATE_CSI,
	GATE_DCMIPP,
	GATE_CCI,
	GATE_VDEC,
	GATE_VENC,
	GATE_RNG,
	GATE_PKA,
	GATE_SAES,
	GATE_HASH,
	GATE_CRYP1,
	GATE_CRYP2,
	GATE_IWDG1,
	GATE_IWDG2,
	GATE_IWDG3,
	GATE_IWDG4,
	GATE_IWDG5,
	GATE_WWDG1,
	GATE_WWDG2,
	GATE_BUSPERFM,
	GATE_VREF,
	GATE_DTS,
	GATE_CRC,
	GATE_SERC,
	GATE_OSPIIOM,
	GATE_GICV2M,
	GATE_I3C1,
	GATE_I3C2,
	GATE_I3C3,
	GATE_I3C4,
	GATE_NB
};

#define GATE_CFG(_id, _offset, _bit_idx, _offset_clr)\
	[(_id)] = {\
		.offset		= (_offset),\
		.bit_idx	= (_bit_idx),\
		.set_clr	= (_offset_clr),\
	}

static const struct gate_cfg gates_mp25[GATE_NB] = {
	GATE_CFG(GATE_LSE,		RCC_BDCR,		0,	0),
	GATE_CFG(GATE_LSE_RDY,		RCC_BDCR,		2,	0),
	GATE_CFG(GATE_LSI,		RCC_BDCR,		9,	0),
	GATE_CFG(GATE_LSI_RDY,		RCC_BDCR,		10,	0),
	GATE_CFG(GATE_RTCCK,		RCC_BDCR,		20,	0),
	GATE_CFG(GATE_MSI,		RCC_D3DCR,		0,	0),
	GATE_CFG(GATE_MSI_RDY,		RCC_D3DCR,		2,	0),
	GATE_CFG(GATE_PLL1,		RCC_PLL2CFGR1,		8,	0),
	GATE_CFG(GATE_PLL1_RDY,		RCC_PLL2CFGR1,		24,	0),
	GATE_CFG(GATE_PLL2,		RCC_PLL2CFGR1,		8,	0),
	GATE_CFG(GATE_PLL2_RDY,		RCC_PLL2CFGR1,		24,	0),
	GATE_CFG(GATE_PLL3,		RCC_PLL3CFGR1,		8,	0),
	GATE_CFG(GATE_PLL3_RDY,		RCC_PLL3CFGR1,		24,	0),
	GATE_CFG(GATE_PLL4,		RCC_PLL4CFGR1,		8,	0),
	GATE_CFG(GATE_PLL4_RDY,		RCC_PLL4CFGR1,		24,	0),
	GATE_CFG(GATE_PLL5,		RCC_PLL5CFGR1,		8,	0),
	GATE_CFG(GATE_PLL5_RDY,		RCC_PLL5CFGR1,		24,	0),
	GATE_CFG(GATE_PLL6,		RCC_PLL6CFGR1,		8,	0),
	GATE_CFG(GATE_PLL6_RDY,		RCC_PLL6CFGR1,		24,	0),
	GATE_CFG(GATE_PLL7,		RCC_PLL7CFGR1,		8,	0),
	GATE_CFG(GATE_PLL7_RDY,		RCC_PLL7CFGR1,		24,	0),
	GATE_CFG(GATE_PLL8,		RCC_PLL8CFGR1,		8,	0),
	GATE_CFG(GATE_PLL8_RDY,		RCC_PLL8CFGR1,		24,	0),
	GATE_CFG(GATE_PLL4_CKREFST,	RCC_PLL4CFGR1,		28,	0),
	GATE_CFG(GATE_PLL5_CKREFST,	RCC_PLL5CFGR1,		28,	0),
	GATE_CFG(GATE_PLL6_CKREFST,	RCC_PLL6CFGR1,		28,	0),
	GATE_CFG(GATE_PLL7_CKREFST,	RCC_PLL7CFGR1,		28,	0),
	GATE_CFG(GATE_PLL8_CKREFST,	RCC_PLL8CFGR1,		28,	0),
	GATE_CFG(GATE_C3,		RCC_C3CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM3C3,		RCC_C3CFGR,		16,	0),
	GATE_CFG(GATE_LPTIM4C3,		RCC_C3CFGR,		17,	0),
	GATE_CFG(GATE_LPTIM5C3,		RCC_C3CFGR,		18,	0),
	GATE_CFG(GATE_SPI8C3,		RCC_C3CFGR,		19,	0),
	GATE_CFG(GATE_LPUART1C3,	RCC_C3CFGR,		20,	0),
	GATE_CFG(GATE_I2C8C3,		RCC_C3CFGR,		21,	0),
	GATE_CFG(GATE_ADF1C3,		RCC_C3CFGR,		23,	0),
	GATE_CFG(GATE_GPIOZC3,		RCC_C3CFGR,		24,	0),
	GATE_CFG(GATE_LPDMAC3,		RCC_C3CFGR,		25,	0),
	GATE_CFG(GATE_RTCC3,		RCC_C3CFGR,		26,	0),
	GATE_CFG(GATE_I3C4C3,		RCC_C3CFGR,		27,	0),
	GATE_CFG(GATE_MCO1,		RCC_MCO1CFGR,		8,	0),
	GATE_CFG(GATE_MCO2,		RCC_MCO2CFGR,		8,	0),
	GATE_CFG(GATE_HSI,		RCC_OCENSETR,		0,	1),
	GATE_CFG(GATE_HSEDIV2,		RCC_OCENSETR,		5,	1),
	GATE_CFG(GATE_HSE,		RCC_OCENSETR,		8,	1),
	GATE_CFG(GATE_HSI_RDY,		RCC_OCRDYR,		0,	0),
	GATE_CFG(GATE_HSE_RDY,		RCC_OCRDYR,		8,	0),
	GATE_CFG(GATE_APB1DIV_RDY,	RCC_APB1DIVR,		31,	0),
	GATE_CFG(GATE_APB2DIV_RDY,	RCC_APB2DIVR,		31,	0),
	GATE_CFG(GATE_APB3DIV_RDY,	RCC_APB3DIVR,		31,	0),
	GATE_CFG(GATE_APB4DIV_RDY,	RCC_APB4DIVR,		31,	0),
	GATE_CFG(GATE_APBDBGDIV_RDY,	RCC_APBDBGDIVR,		31,	0),
	GATE_CFG(GATE_TIMG1PRE_RDY,	RCC_TIMG1PRER,		31,	0),
	GATE_CFG(GATE_TIMG2PRE_RDY,	RCC_TIMG2PRER,		31,	0),
	GATE_CFG(GATE_LSMCUDIV_RDY,	RCC_LSMCUDIVR,		31,	0),
	GATE_CFG(GATE_DDRCP,		RCC_DDRCPCFGR,		1,	0),
	GATE_CFG(GATE_DDRCAPB,		RCC_DDRCAPBCFGR,	1,	0),
	GATE_CFG(GATE_DDRPHYCAPB,	RCC_DDRPHYCAPBCFGR,	1,	0),
	GATE_CFG(GATE_DDRPHYC,		RCC_DDRPHYCCFGR,	1,	0),
	GATE_CFG(GATE_DDRCFG,		RCC_DDRCFGR,		1,	0),
	GATE_CFG(GATE_SYSRAM,		RCC_SYSRAMCFGR,		1,	0),
	GATE_CFG(GATE_VDERAM,		RCC_VDERAMCFGR,		1,	0),
	GATE_CFG(GATE_SRAM1,		RCC_SRAM1CFGR,		1,	0),
	GATE_CFG(GATE_SRAM2,		RCC_SRAM2CFGR,		1,	0),
	GATE_CFG(GATE_RETRAM,		RCC_RETRAMCFGR,		1,	0),
	GATE_CFG(GATE_BKPSRAM,		RCC_BKPSRAMCFGR,	1,	0),
	GATE_CFG(GATE_LPSRAM1,		RCC_LPSRAM1CFGR,	1,	0),
	GATE_CFG(GATE_LPSRAM2,		RCC_LPSRAM2CFGR,	1,	0),
	GATE_CFG(GATE_LPSRAM3,		RCC_LPSRAM3CFGR,	1,	0),
	GATE_CFG(GATE_OSPI1,		RCC_OSPI1CFGR,		1,	0),
	GATE_CFG(GATE_OSPI2,		RCC_OSPI2CFGR,		1,	0),
	GATE_CFG(GATE_FMC,		RCC_FMCCFGR,		1,	0),
	GATE_CFG(GATE_DBG,		RCC_DBGCFGR,		8,	0),
	GATE_CFG(GATE_TRACE,		RCC_DBGCFGR,		9,	0),
	GATE_CFG(GATE_STM500,		RCC_STM500CFGR,		1,	0),
	GATE_CFG(GATE_ETR,		RCC_ETRCFGR,		1,	0),
	GATE_CFG(GATE_GPIOA,		RCC_GPIOACFGR,		1,	0),
	GATE_CFG(GATE_GPIOB,		RCC_GPIOBCFGR,		1,	0),
	GATE_CFG(GATE_GPIOC,		RCC_GPIOCCFGR,		1,	0),
	GATE_CFG(GATE_GPIOD,		RCC_GPIODCFGR,		1,	0),
	GATE_CFG(GATE_GPIOE,		RCC_GPIOECFGR,		1,	0),
	GATE_CFG(GATE_GPIOF,		RCC_GPIOFCFGR,		1,	0),
	GATE_CFG(GATE_GPIOG,		RCC_GPIOGCFGR,		1,	0),
	GATE_CFG(GATE_GPIOH,		RCC_GPIOHCFGR,		1,	0),
	GATE_CFG(GATE_GPIOI,		RCC_GPIOICFGR,		1,	0),
	GATE_CFG(GATE_GPIOJ,		RCC_GPIOJCFGR,		1,	0),
	GATE_CFG(GATE_GPIOK,		RCC_GPIOKCFGR,		1,	0),
	GATE_CFG(GATE_GPIOZ,		RCC_GPIOZCFGR,		1,	0),
	GATE_CFG(GATE_HPDMA1,		RCC_HPDMA1CFGR,		1,	0),
	GATE_CFG(GATE_HPDMA2,		RCC_HPDMA2CFGR,		1,	0),
	GATE_CFG(GATE_HPDMA3,		RCC_HPDMA3CFGR,		1,	0),
	GATE_CFG(GATE_LPDMA,		RCC_LPDMACFGR,		1,	0),
	GATE_CFG(GATE_HSEM,		RCC_HSEMCFGR,		1,	0),
	GATE_CFG(GATE_IPCC1,		RCC_IPCC1CFGR,		1,	0),
	GATE_CFG(GATE_IPCC2,		RCC_IPCC2CFGR,		1,	0),
	GATE_CFG(GATE_RTC,		RCC_RTCCFGR,		1,	0),
	GATE_CFG(GATE_SYSCPU1,		RCC_SYSCPU1CFGR,	1,	0),
	GATE_CFG(GATE_BSEC,		RCC_BSECCFGR,		1,	0),
	GATE_CFG(GATE_IS2M,		RCC_IS2MCFGR,		1,	0),
	GATE_CFG(GATE_HSIMON,		RCC_HSIFMONCR,		15,	0),
	GATE_CFG(GATE_TIM1,		RCC_TIM1CFGR,		1,	0),
	GATE_CFG(GATE_TIM2,		RCC_TIM2CFGR,		1,	0),
	GATE_CFG(GATE_TIM3,		RCC_TIM3CFGR,		1,	0),
	GATE_CFG(GATE_TIM4,		RCC_TIM4CFGR,		1,	0),
	GATE_CFG(GATE_TIM5,		RCC_TIM5CFGR,		1,	0),
	GATE_CFG(GATE_TIM6,		RCC_TIM6CFGR,		1,	0),
	GATE_CFG(GATE_TIM7,		RCC_TIM7CFGR,		1,	0),
	GATE_CFG(GATE_TIM8,		RCC_TIM8CFGR,		1,	0),
	GATE_CFG(GATE_TIM10,		RCC_TIM10CFGR,		1,	0),
	GATE_CFG(GATE_TIM11,		RCC_TIM11CFGR,		1,	0),
	GATE_CFG(GATE_TIM12,		RCC_TIM12CFGR,		1,	0),
	GATE_CFG(GATE_TIM13,		RCC_TIM13CFGR,		1,	0),
	GATE_CFG(GATE_TIM14,		RCC_TIM14CFGR,		1,	0),
	GATE_CFG(GATE_TIM15,		RCC_TIM15CFGR,		1,	0),
	GATE_CFG(GATE_TIM16,		RCC_TIM16CFGR,		1,	0),
	GATE_CFG(GATE_TIM17,		RCC_TIM17CFGR,		1,	0),
	GATE_CFG(GATE_TIM20,		RCC_TIM20CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM1,		RCC_LPTIM1CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM2,		RCC_LPTIM2CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM3,		RCC_LPTIM3CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM4,		RCC_LPTIM4CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM5,		RCC_LPTIM5CFGR,		1,	0),
	GATE_CFG(GATE_SPI1,		RCC_SPI1CFGR,		1,	0),
	GATE_CFG(GATE_SPI2,		RCC_SPI2CFGR,		1,	0),
	GATE_CFG(GATE_SPI3,		RCC_SPI3CFGR,		1,	0),
	GATE_CFG(GATE_SPI4,		RCC_SPI4CFGR,		1,	0),
	GATE_CFG(GATE_SPI5,		RCC_SPI5CFGR,		1,	0),
	GATE_CFG(GATE_SPI6,		RCC_SPI6CFGR,		1,	0),
	GATE_CFG(GATE_SPI7,		RCC_SPI7CFGR,		1,	0),
	GATE_CFG(GATE_SPI8,		RCC_SPI8CFGR,		1,	0),
	GATE_CFG(GATE_SPDIFRX,		RCC_SPDIFRXCFGR,	1,	0),
	GATE_CFG(GATE_USART1,		RCC_USART1CFGR,		1,	0),
	GATE_CFG(GATE_USART2,		RCC_USART2CFGR,		1,	0),
	GATE_CFG(GATE_USART3,		RCC_USART3CFGR,		1,	0),
	GATE_CFG(GATE_UART4,		RCC_UART4CFGR,		1,	0),
	GATE_CFG(GATE_UART5,		RCC_UART5CFGR,		1,	0),
	GATE_CFG(GATE_USART6,		RCC_USART6CFGR,		1,	0),
	GATE_CFG(GATE_UART7,		RCC_UART7CFGR,		1,	0),
	GATE_CFG(GATE_UART8,		RCC_UART8CFGR,		1,	0),
	GATE_CFG(GATE_UART9,		RCC_UART9CFGR,		1,	0),
	GATE_CFG(GATE_LPUART1,		RCC_LPUART1CFGR,	1,	0),
	GATE_CFG(GATE_I2C1,		RCC_I2C1CFGR,		1,	0),
	GATE_CFG(GATE_I2C2,		RCC_I2C2CFGR,		1,	0),
	GATE_CFG(GATE_I2C3,		RCC_I2C3CFGR,		1,	0),
	GATE_CFG(GATE_I2C4,		RCC_I2C4CFGR,		1,	0),
	GATE_CFG(GATE_I2C5,		RCC_I2C5CFGR,		1,	0),
	GATE_CFG(GATE_I2C6,		RCC_I2C6CFGR,		1,	0),
	GATE_CFG(GATE_I2C7,		RCC_I2C7CFGR,		1,	0),
	GATE_CFG(GATE_I2C8,		RCC_I2C8CFGR,		1,	0),
	GATE_CFG(GATE_SAI1,		RCC_SAI1CFGR,		1,	0),
	GATE_CFG(GATE_SAI2,		RCC_SAI2CFGR,		1,	0),
	GATE_CFG(GATE_SAI3,		RCC_SAI3CFGR,		1,	0),
	GATE_CFG(GATE_SAI4,		RCC_SAI4CFGR,		1,	0),
	GATE_CFG(GATE_MDF1,		RCC_MDF1CFGR,		1,	0),
	GATE_CFG(GATE_ADF1,		RCC_ADF1CFGR,		1,	0),
	GATE_CFG(GATE_FDCAN,		RCC_FDCANCFGR,		1,	0),
	GATE_CFG(GATE_HDP,		RCC_HDPCFGR,		1,	0),
	GATE_CFG(GATE_ADC12,		RCC_ADC12CFGR,		1,	0),
	GATE_CFG(GATE_ADC3,		RCC_ADC3CFGR,		1,	0),
	GATE_CFG(GATE_ETH1MAC,		RCC_ETH1CFGR,		1,	0),
	GATE_CFG(GATE_ETH1STP,		RCC_ETH1CFGR,		4,	0),
	GATE_CFG(GATE_ETH1,		RCC_ETH1CFGR,		5,	0),
	GATE_CFG(GATE_ETH1TX,		RCC_ETH1CFGR,		8,	0),
	GATE_CFG(GATE_ETH1RX,		RCC_ETH1CFGR,		10,	0),
	GATE_CFG(GATE_ETH2MAC,		RCC_ETH2CFGR,		1,	0),
	GATE_CFG(GATE_ETH2STP,		RCC_ETH2CFGR,		4,	0),
	GATE_CFG(GATE_ETH2,		RCC_ETH2CFGR,		5,	0),
	GATE_CFG(GATE_ETH2TX,		RCC_ETH2CFGR,		8,	0),
	GATE_CFG(GATE_ETH2RX,		RCC_ETH2CFGR,		10,	0),
	GATE_CFG(GATE_USB2,		RCC_USB2CFGR,		1,	0),
	GATE_CFG(GATE_USB2PHY1,		RCC_USB2PHY1CFGR,	1,	0),
	GATE_CFG(GATE_USB2PHY2,		RCC_USB2PHY2CFGR,	1,	0),
	GATE_CFG(GATE_USB3DRD,		RCC_USB3DRDCFGR,	1,	0),
	GATE_CFG(GATE_USB3PCIEPHY,	RCC_USB3PCIEPHYCFGR,	1,	0),
	GATE_CFG(GATE_PCIE,		RCC_PCIECFGR,		1,	0),
	GATE_CFG(GATE_USBTC,		RCC_USBTCCFGR,		1,	0),
	GATE_CFG(GATE_ETHSWMAC,		RCC_ETHSWCFGR,		1,	0),
	GATE_CFG(GATE_ETHSW,		RCC_ETHSWCFGR,		5,	0),
	GATE_CFG(GATE_ETHSWREF,		RCC_ETHSWCFGR,		21,	0),
	GATE_CFG(GATE_STGEN,		RCC_STGENCFGR,		1,	0),
	GATE_CFG(GATE_SDMMC1,		RCC_SDMMC1CFGR,		1,	0),
	GATE_CFG(GATE_SDMMC2,		RCC_SDMMC2CFGR,		1,	0),
	GATE_CFG(GATE_SDMMC3,		RCC_SDMMC3CFGR,		1,	0),
	GATE_CFG(GATE_GPU,		RCC_GPUCFGR,		1,	0),
	GATE_CFG(GATE_LTDC,		RCC_LTDCCFGR,		1,	0),
	GATE_CFG(GATE_DSI,		RCC_DSICFGR,		1,	0),
	GATE_CFG(GATE_LVDS,		RCC_LVDSCFGR,		1,	0),
	GATE_CFG(GATE_CSI,		RCC_CSICFGR,		1,	0),
	GATE_CFG(GATE_DCMIPP,		RCC_DCMIPPCFGR,		1,	0),
	GATE_CFG(GATE_CCI,		RCC_CCICFGR,		1,	0),
	GATE_CFG(GATE_VDEC,		RCC_VDECCFGR,		1,	0),
	GATE_CFG(GATE_VENC,		RCC_VENCCFGR,		1,	0),
	GATE_CFG(GATE_RNG,		RCC_RNGCFGR,		1,	0),
	GATE_CFG(GATE_PKA,		RCC_PKACFGR,		1,	0),
	GATE_CFG(GATE_SAES,		RCC_SAESCFGR,		1,	0),
	GATE_CFG(GATE_HASH,		RCC_HASHCFGR,		1,	0),
	GATE_CFG(GATE_CRYP1,		RCC_CRYP1CFGR,		1,	0),
	GATE_CFG(GATE_CRYP2,		RCC_CRYP2CFGR,		1,	0),
	GATE_CFG(GATE_IWDG1,		RCC_IWDG1CFGR,		1,	0),
	GATE_CFG(GATE_IWDG2,		RCC_IWDG2CFGR,		1,	0),
	GATE_CFG(GATE_IWDG3,		RCC_IWDG3CFGR,		1,	0),
	GATE_CFG(GATE_IWDG4,		RCC_IWDG4CFGR,		1,	0),
	GATE_CFG(GATE_IWDG5,		RCC_IWDG5CFGR,		1,	0),
	GATE_CFG(GATE_WWDG1,		RCC_WWDG1CFGR,		1,	0),
	GATE_CFG(GATE_WWDG2,		RCC_WWDG2CFGR,		1,	0),
	GATE_CFG(GATE_BUSPERFM,		RCC_BUSPERFMCFGR,	1,	0),
	GATE_CFG(GATE_VREF,		RCC_VREFCFGR,		1,	0),
	GATE_CFG(GATE_DTS,		RCC_DTSCFGR,		1,	0),
	GATE_CFG(GATE_CRC,		RCC_CRCCFGR,		1,	0),
	GATE_CFG(GATE_SERC,		RCC_SERCCFGR,		1,	0),
	GATE_CFG(GATE_OSPIIOM,		RCC_OSPIIOMCFGR,	1,	0),
	GATE_CFG(GATE_GICV2M,		RCC_GICV2MCFGR,		1,	0),
	GATE_CFG(GATE_I3C1,		RCC_I3C1CFGR,		1,	0),
	GATE_CFG(GATE_I3C2,		RCC_I3C2CFGR,		1,	0),
	GATE_CFG(GATE_I3C3,		RCC_I3C3CFGR,		1,	0),
	GATE_CFG(GATE_I3C4,		RCC_I3C4CFGR,		1,	0),
};

/*
 * MUX CONFIG
 *
 */

#undef MUX_CFG

#define MUXRDY_CFG(_id, _offset, _shift, _witdh, _rdy)\
	[(_id)] = {\
			.offset	= (_offset),\
			.shift	= (_shift),\
			.width	= (_witdh),\
			.ready = _rdy,\
	}

#define MUX_CFG(_id, _offset, _shift, _witdh)\
	MUXRDY_CFG(_id, _offset, _shift, _witdh, MUX_NO_RDY)

static const struct mux_cfg parent_mp25[MUX_NB] = {
	MUXRDY_CFG(MUX_MUXSEL0,		RCC_MUXSELCFGR,		0,	2, GATE_PLL4_CKREFST),
	MUXRDY_CFG(MUX_MUXSEL1,		RCC_MUXSELCFGR,		4,	2, GATE_PLL5_CKREFST),
	MUXRDY_CFG(MUX_MUXSEL2,		RCC_MUXSELCFGR,		8,	2, GATE_PLL6_CKREFST),
	MUXRDY_CFG(MUX_MUXSEL3,		RCC_MUXSELCFGR,		12,	2, GATE_PLL7_CKREFST),
	MUXRDY_CFG(MUX_MUXSEL4,		RCC_MUXSELCFGR,		16,	2, GATE_PLL8_CKREFST),
	MUX_CFG(MUX_MUXSEL5,		RCC_MUXSELCFGR,		20,	2),
	MUX_CFG(MUX_MUXSEL6,		RCC_MUXSELCFGR,		24,	2),
	MUX_CFG(MUX_MUXSEL7,		RCC_MUXSELCFGR,		28,	2),
	MUX_CFG(MUX_XBARSEL,		RCC_XBAR0CFGR,		0,	4),
	MUX_CFG(MUX_RTC,		RCC_BDCR,		16,	2),
	MUX_CFG(MUX_D3PER,		RCC_D3DCR,		16,	2),
	MUX_CFG(MUX_MCO1,		RCC_MCO1CFGR,		0,	1),
	MUX_CFG(MUX_MCO2,		RCC_MCO2CFGR,		0,	1),
	MUX_CFG(MUX_ADC12,		RCC_ADC12CFGR,		12,	1),
	MUX_CFG(MUX_ADC3,		RCC_ADC3CFGR,		12,	2),
	MUX_CFG(MUX_USB2PHY1,		RCC_USB2PHY1CFGR,	15,	1),
	MUX_CFG(MUX_USB2PHY2,		RCC_USB2PHY2CFGR,	15,	1),
	MUX_CFG(MUX_USB3PCIEPHY,	RCC_USB3PCIEPHYCFGR,	15,	1),
	MUX_CFG(MUX_DSIBLANE,		RCC_DSICFGR,		12,	1),
	MUX_CFG(MUX_DSIPHY,		RCC_DSICFGR,		15,	1),
	MUX_CFG(MUX_LVDSPHY,		RCC_LVDSCFGR,		15,	1),
	MUX_CFG(MUX_DTS,		RCC_DTSCFGR,		12,	2),
};

/*
 * DIV CONFIG
 *
 */

static const struct div_table_cfg apb_div_table[] = {
	{ 0, 1 },  { 1, 2 },  { 2, 4 },  { 3, 8 }, { 4, 16 },
	{ 5, 16 }, { 6, 16 }, { 7, 16 }, { 0 },
};

#define DIVRDY_CFG(_id, _offset, _shift, _width, _flags, _table, _ready)\
	[(_id)] = {\
		.offset	= (_offset),\
		.shift	= (_shift),\
		.width	= (_width),\
		.flags	= (_flags),\
		.table	= (_table),\
		.ready	= (_ready),\
	}

#undef DIV_CFG
#define DIV_CFG(_id, _offset, _shift, _width, _flags, _table)\
	DIVRDY_CFG(_id, _offset, _shift, _width, _flags, _table, DIV_NO_RDY)

static const struct div_cfg dividers_mp25[DIV_NB] = {
	DIV_CFG(DIV_RTC,	RCC_RTCDIVR,	0, 6, 0, NULL),
	DIVRDY_CFG(DIV_APB1,	RCC_APB1DIVR,	0, 3, 0, apb_div_table,	GATE_APB1DIV_RDY),
	DIVRDY_CFG(DIV_APB2,	RCC_APB2DIVR,	0, 3, 0, apb_div_table,	GATE_APB2DIV_RDY),
	DIVRDY_CFG(DIV_APB3,	RCC_APB3DIVR,	0, 3, 0, apb_div_table,	GATE_APB3DIV_RDY),
	DIVRDY_CFG(DIV_APB4,	RCC_APB4DIVR,	0, 3, 0, apb_div_table,	GATE_APB4DIV_RDY),
	DIVRDY_CFG(DIV_APBDBG,	RCC_APBDBGDIVR,	0, 3, 0, apb_div_table,	GATE_APBDBGDIV_RDY),
	DIVRDY_CFG(DIV_LSMCU,	RCC_LSMCUDIVR,	0, 1, 0, NULL,		GATE_LSMCUDIV_RDY),
};

struct clk_stm32_bypass {
	uint16_t offset;
	uint8_t bit_byp;
	uint8_t bit_digbyp;
};

struct clk_stm32_css {
	uint16_t offset;
	uint8_t bit_css;
};

struct clk_stm32_drive {
	uint16_t offset;
	uint8_t drv_shift;
	uint8_t drv_width;
	uint8_t drv_default;
};

struct clk_oscillator_data {
#if !CLK_MINIMAL_SZ
	const char *name;
#endif
	unsigned long frequency;
	uint16_t gate_id;
	struct clk_stm32_bypass *bypass;
	struct clk_stm32_css *css;
	struct clk_stm32_drive *drive;
};

#define BYPASS(_offset, _bit_byp, _bit_digbyp) &(struct clk_stm32_bypass){\
	.offset		= (_offset),\
	.bit_byp	= (_bit_byp),\
	.bit_digbyp	= (_bit_digbyp),\
}

#define CSS(_offset, _bit_css)	&(struct clk_stm32_css){\
	.offset		= (_offset),\
	.bit_css	= (_bit_css),\
}

#define DRIVE(_offset, _shift, _width, _default) &(struct clk_stm32_drive){\
	.offset		= (_offset),\
	.drv_shift	= (_shift),\
	.drv_width	= (_width),\
	.drv_default	= (_default),\
}

#define OSCILLATOR(idx_osc, _name, _gate_id, _bypass, _css, _drive) \
	[(idx_osc)] = (struct clk_oscillator_data){\
		CLOCK_NAME(#_name)\
		.gate_id	= (_gate_id),\
		.bypass		= (_bypass),\
		.css		= (_css),\
		.drive		= (_drive),\
	}

static struct clk_oscillator_data stm32mp25_osc_data[NB_OSCILLATOR] = {
	OSCILLATOR(OSC_HSI, "clk-hsi", GATE_HSI,
		   NULL, NULL, NULL),

	OSCILLATOR(OSC_LSI, "clk-lsi", GATE_LSI,
		   NULL, NULL, NULL),

	OSCILLATOR(OSC_MSI, "clk-msi", GATE_MSI,
		   NULL, NULL, NULL),

	OSCILLATOR(OSC_LSE, "clk-lse", GATE_LSE,
		   BYPASS(RCC_BDCR, 1, 3),
		   CSS(RCC_BDCR, 8),
		   DRIVE(RCC_BDCR, 4, 2, 2)),

	OSCILLATOR(OSC_HSE, "clk-hse", GATE_HSE,
		   BYPASS(RCC_OCENSETR, 10, 7),
		   CSS(RCC_OCENSETR, 11),
		   NULL),
};

static bool stm32_rcc_has_access_by_id(struct clk_stm32_priv *priv, uint16_t id)
{
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	unsigned int master = RIF_CID2;
	uint32_t cid_reg_value = 0;

	cid_reg_value = io_read32(rcc_base + RCC_R0CIDCFGR + 0x8 * id);

	/*
	 * First check conditions for semaphore mode, which doesn't
	 * take into account static CID.
	 */
	if (cid_reg_value & RCC_CIDCFGR_SEM_EN) {
		if (cid_reg_value & BIT(master + RCC_SEMWL_SHIFT))
			/* Static CID is irrelevant if semaphore mode */
			return true;
		else
			return false;
	}

	if (!(cid_reg_value & RCC_CIDCFGR_CFEN) ||
	    ((cid_reg_value & RCC_CIDCFGR_SCID_MASK) >>
	    RCC_CIDCFGR_SCID_SHIFT) == RIF_CID0)
		return true;

	/*
	 * Coherency check with the CID configuration
	 */
	if (((cid_reg_value & RCC_CIDCFGR_SCID_MASK) >>
	     RCC_CIDCFGR_SCID_SHIFT) != master)
		return false;

	return true;
}

static inline struct clk_oscillator_data *clk_oscillator_get_data(int osc_id)
{
	return &stm32mp25_osc_data[osc_id];
}

static unsigned long clk_stm32_get_rate_oscillateur(struct clk_stm32_priv *priv, int osc_id)
{
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	const struct stm32_osci_dt_cfg *osci = &rcc_cfg->osci[osc_id];

	return osci->freq;
}

static unsigned long clk_stm32_pll_get_oscillator_rate(struct clk_stm32_priv *priv, int sel)
{
	int osc[] = { OSC_HSI, OSC_HSE, OSC_MSI };

	return clk_stm32_get_rate_oscillateur(priv, osc[sel]);
}

static void clk_oscillator_set_bypass(struct clk_stm32_priv  *priv,
				      struct clk_oscillator_data *osc_data,
				      bool digbyp, bool bypass)
{
	struct clk_stm32_bypass *bypass_data = osc_data->bypass;
	uintptr_t address;

	if (bypass_data == NULL)
		return;

	address = clk_stm32_get_rcc_base(priv) + bypass_data->offset;

	if (digbyp)
		io_setbits32(address, BIT(bypass_data->bit_digbyp));

	if (bypass || digbyp)
		io_setbits32(address, BIT(bypass_data->bit_byp));
}

static void clk_oscillator_set_css(struct clk_stm32_priv  *priv,
				   struct clk_oscillator_data *osc_data,
				   bool css)
{
	struct clk_stm32_css *css_data = osc_data->css;
	uintptr_t address;

	if (css_data == NULL)
		return;

	address = clk_stm32_get_rcc_base(priv) + css_data->offset;

	if (css)
		io_setbits32(address, BIT(css_data->bit_css));
}

static void clk_oscillator_set_drive(struct clk_stm32_priv  *priv,
				     struct clk_oscillator_data *osc_data,
				     uint8_t lsedrv)
{
	struct clk_stm32_drive *drive_data = osc_data->drive;
	uintptr_t address;
	uint32_t mask;
	uint32_t value;

	if (drive_data == NULL)
		return;

	address = clk_stm32_get_rcc_base(priv) + drive_data->offset;

	mask = (BIT(drive_data->drv_width) - 1U) << drive_data->drv_shift;

	/*
	 * Warning: not recommended to switch directly from "high drive"
	 * to "medium low drive", and vice-versa.
	 */
	value = (io_read32(address) & mask) >> drive_data->drv_shift;

	while (value != lsedrv) {
		if (value > lsedrv)
			value--;
		else
			value++;

		io_clrsetbits32(address, mask, value << drive_data->drv_shift);
	}
}

static void stm32_enable_oscillator_hse(struct clk_stm32_priv *priv)
{
	struct clk_oscillator_data *osc_data = clk_oscillator_get_data(OSC_HSE);
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	const struct stm32_osci_dt_cfg *osci = &rcc_cfg->osci[OSC_HSE];

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return;

	if (osci->freq == 0U)
		return;

	clk_oscillator_set_bypass(priv, osc_data,  osci->digbyp, osci->bypass);

	/* Enable clock and wait ready bit */
	if (stm32_gate_rdy_enable(priv, osc_data->gate_id)) {
		EMSG("timeout to enable hse clock");
		panic();
	}

	clk_oscillator_set_css(priv, osc_data, osci->css);
}

static void stm32_enable_oscillator_lse(struct clk_stm32_priv *priv)
{
	struct clk_oscillator_data *osc_data = clk_oscillator_get_data(OSC_LSE);
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	const struct stm32_osci_dt_cfg *osci = &rcc_cfg->osci[OSC_LSE];

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return;

	if (osci->freq == 0U)
		return;

	if (stm32_gate_is_enabled(priv, osc_data->gate_id))
		return;

	clk_oscillator_set_bypass(priv, osc_data, osci->digbyp, osci->bypass);

	clk_oscillator_set_drive(priv, osc_data,  osci->drive);

	/* Enable lse clock, but don't wait ready bit */
	stm32_gate_enable(priv, osc_data->gate_id);
}

static void stm32_enable_oscillator_lsi(__maybe_unused struct clk_stm32_priv  *priv)
{
	struct clk_oscillator_data *osc_data = clk_oscillator_get_data(OSC_LSI);
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	const struct stm32_osci_dt_cfg *osci = &rcc_cfg->osci[OSC_LSI];

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return;

	if (osci->freq == 0U)
		return;

	/* Enable clock and wait ready bit */
	if (stm32_gate_rdy_enable(priv, osc_data->gate_id)) {
		EMSG("timeout to enable lsi clock");
		panic();
	}
}

static int clk_stm32_osc_msi_set_rate(struct clk_stm32_priv *priv,
				      unsigned long rate)
{
	uintptr_t address = clk_stm32_get_rcc_base(priv) + RCC_BDCR;
	uint32_t mask = RCC_BDCR_MSIFREQSEL;

	switch(rate) {
	case RCC_4_MHZ:
		io_clrbits32(address, mask);
		break;

	case RCC_16_MHZ:
		io_setbits32(address, mask);
		break;
	default:
		return -1;
	}

	return 0;
}

static void stm32_enable_oscillator_msi(struct clk_stm32_priv  *priv)
{
	struct clk_oscillator_data *osc_data = clk_oscillator_get_data(OSC_MSI);
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	const struct stm32_osci_dt_cfg *osci = &rcc_cfg->osci[OSC_MSI];

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return;

	if (osci->freq == 0U)
		return;

	if (clk_stm32_osc_msi_set_rate(priv, osci->freq) != 0)
		EMSG("invalid rate %ld Hz for MSI ! (4000000 or 16000000 only)\n", osci->freq);

	/* Enable clock and wait ready bit */
	if (stm32_gate_rdy_enable(priv, osc_data->gate_id)) {
		EMSG("timeout to enable msi clock");
		panic();
	}
}

static int stm32_clk_oscillators_lse_set_css(struct clk_stm32_priv  *priv)
{
	struct clk_oscillator_data *osc_data = clk_oscillator_get_data(OSC_LSE);
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	const struct stm32_osci_dt_cfg *osci = &rcc_cfg->osci[OSC_LSE];

	if (stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		clk_oscillator_set_css(priv, osc_data, osci->css);

	return 0;
}

static int stm32_clk_oscillators_wait_lse_ready(__maybe_unused struct clk_stm32_priv  *priv)
{
	struct clk_oscillator_data *osc_data = clk_oscillator_get_data(OSC_LSE);
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	const struct stm32_osci_dt_cfg *osci = &rcc_cfg->osci[OSC_LSE];
	int ret = 0;

	if (stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS)) {
		if (osci->freq != 0U)
			ret = stm32_gate_wait_ready(priv, osc_data->gate_id, true);
	}

	return ret;
}

static void stm32_clk_oscillators_enable(struct clk_stm32_priv  *priv)
{
	stm32_enable_oscillator_hse(priv);
	stm32_enable_oscillator_lse(priv);
	stm32_enable_oscillator_lsi(priv);
	stm32_enable_oscillator_msi(priv);
}

struct stm32_clk_pll {
	uint16_t gate_id;
	uint16_t mux_id;
	uint16_t reg_pllxcfgr1;
};

#define CLK_PLL_CFG(_idx, _gate_id, _mux_id, _reg)\
	[(_idx)] = {\
		.gate_id = (_gate_id),\
		.mux_id = (_mux_id),\
		.reg_pllxcfgr1 = (_reg),\
	}

static const struct stm32_clk_pll stm32mp25_clk_pll[PLL_NB] = {
	// CLK_PLL_CFG(PLL1_ID, GATE_PLL1, MUX_MUXSEL5, A35_SS_CHGCLKREQ),
	CLK_PLL_CFG(PLL2_ID, GATE_PLL2, MUX_MUXSEL6, RCC_PLL2CFGR1),
	CLK_PLL_CFG(PLL3_ID, GATE_PLL3, MUX_MUXSEL7, RCC_PLL3CFGR1),
	CLK_PLL_CFG(PLL4_ID, GATE_PLL4, MUX_MUXSEL0, RCC_PLL4CFGR1),
	CLK_PLL_CFG(PLL5_ID, GATE_PLL5, MUX_MUXSEL1, RCC_PLL5CFGR1),
	CLK_PLL_CFG(PLL6_ID, GATE_PLL6, MUX_MUXSEL2, RCC_PLL6CFGR1),
	CLK_PLL_CFG(PLL7_ID, GATE_PLL7, MUX_MUXSEL3, RCC_PLL7CFGR1),
	CLK_PLL_CFG(PLL8_ID, GATE_PLL8, MUX_MUXSEL4, RCC_PLL8CFGR1),
};

static const struct stm32_clk_pll *clk_stm32_pll_data(unsigned int idx)
{
	return &stm32mp25_clk_pll[idx];
}

static void __maybe_unused stm32mp2_clk_xbar_on_hsi(struct clk_stm32_priv *priv)
{
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	uint32_t i;

	for (i = 0; i < XBAR_ROOT_CHANNEL_NB; i++) {
		if (!stm32_rcc_has_access_by_id(priv, i))
			continue;

		io_clrsetbits32(rcc_base + RCC_XBAR0CFGR + (0x4 * i),
				_RCC_XBAR0CFGR_XBAR0SEL_MASK, XBAR_SRC_HSI);
	}
}

static int clk_stm32_pll_config_output(struct clk_stm32_priv *priv,
				       const struct stm32_clk_pll *pll,
				       uint32_t pllsrc,
				       const uint32_t *pllcfg,
				       uint32_t fracv)
{
	uintptr_t pllxcfgr1 = clk_stm32_get_rcc_base(priv) + pll->reg_pllxcfgr1;
	uintptr_t pllxcfgr2 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR2;
	uintptr_t pllxcfgr3 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR3;
	uintptr_t pllxcfgr4 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR4;
	uintptr_t pllxcfgr6 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR6;
	uintptr_t pllxcfgr7 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR7;
	int sel = (pllsrc & MUX_SEL_MASK) >> MUX_SEL_SHIFT;

	unsigned long refclk = clk_stm32_pll_get_oscillator_rate(priv, sel);

	if (fracv == 0U) {
		/* PLL in integer mode */

		/*
		 * No need to check max clock, as oscillator reference clocks
		 * will always be less than 1.2GHz
		 */
		if (refclk < PLL_REFCLK_MIN)
			panic();

		io_clrbits32(pllxcfgr3, RCC_PLLxCFGR3_FRACIN_MASK);
		io_clrbits32(pllxcfgr4, RCC_PLLxCFGR4_DSMEN);
		io_clrbits32(pllxcfgr3, RCC_PLLxCFGR3_DACEN);
		io_setbits32(pllxcfgr3, RCC_PLLxCFGR3_SSCGDIS);
		io_setbits32(pllxcfgr1, RCC_PLLxCFGR1_SSMODRST);
	} else {
		/* PLL in frac mode */

		/*
		 * No need to check max clock, as oscillator reference clocks
		 * will always be less than 1.2GHz
		 */
		if (refclk < PLL_FRAC_REFCLK_MIN)
			panic();

		io_clrsetbits32(pllxcfgr3, RCC_PLLxCFGR3_FRACIN_MASK,
				   fracv & RCC_PLLxCFGR3_FRACIN_MASK);
		io_setbits32(pllxcfgr3, RCC_PLLxCFGR3_SSCGDIS);
		io_setbits32(pllxcfgr4, RCC_PLLxCFGR4_DSMEN);
	}

	assert(pllcfg[REFDIV] != 0U);

	io_clrsetbits32(pllxcfgr2, RCC_PLLxCFGR2_FBDIV_MASK,
			   (pllcfg[FBDIV] << RCC_PLLxCFGR2_FBDIV_SHIFT) &
			   RCC_PLLxCFGR2_FBDIV_MASK);
	io_clrsetbits32(pllxcfgr2, RCC_PLLxCFGR2_FREFDIV_MASK,
			   pllcfg[REFDIV] & RCC_PLLxCFGR2_FREFDIV_MASK);
	io_clrsetbits32(pllxcfgr6, RCC_PLLxCFGR6_POSTDIV1_MASK,
			   pllcfg[POSTDIV1] & RCC_PLLxCFGR6_POSTDIV1_MASK);
	io_clrsetbits32(pllxcfgr7, RCC_PLLxCFGR7_POSTDIV2_MASK,
			   pllcfg[POSTDIV2] & RCC_PLLxCFGR7_POSTDIV2_MASK);


	if ((pllcfg[POSTDIV1] == 0U) || (pllcfg[POSTDIV2] == 0U)) {
		/* Bypass mode */
		io_setbits32(pllxcfgr4, RCC_PLLxCFGR4_BYPASS);
		io_clrbits32(pllxcfgr4, RCC_PLLxCFGR4_FOUTPOSTDIVEN);
	} else {
		io_clrbits32(pllxcfgr4, RCC_PLLxCFGR4_BYPASS);
		io_setbits32(pllxcfgr4, RCC_PLLxCFGR4_FOUTPOSTDIVEN);
	}

	return 0;
}

static void clk_stm32_pll_config_csg(struct clk_stm32_priv *priv,
				     const struct stm32_clk_pll *pll,
				     const uint32_t *csg)
{
	uintptr_t pllxcfgr1 = clk_stm32_get_rcc_base(priv) + pll->reg_pllxcfgr1;
	uintptr_t pllxcfgr3 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR3;
	uintptr_t pllxcfgr4 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR4;
	uintptr_t pllxcfgr5 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR5;


	io_clrsetbits32(pllxcfgr5, RCC_PLLxCFGR5_DIVVAL_MASK,
			   csg[DIVVAL] & RCC_PLLxCFGR5_DIVVAL_MASK);
	io_clrsetbits32(pllxcfgr5, RCC_PLLxCFGR5_SPREAD_MASK,
			   (csg[SPREAD] << RCC_PLLxCFGR5_SPREAD_SHIFT) &
			   RCC_PLLxCFGR5_SPREAD_MASK);

	if (csg[DOWNSPREAD] != 0)
		io_setbits32(pllxcfgr3, RCC_PLLxCFGR3_DOWNSPREAD);
	else
		io_clrbits32(pllxcfgr3, RCC_PLLxCFGR3_DOWNSPREAD);

	io_clrbits32(pllxcfgr3, RCC_PLLxCFGR3_SSCGDIS);

	io_clrbits32(pllxcfgr1, RCC_PLLxCFGR1_PLLEN);
	udelay(1);

	io_setbits32(pllxcfgr4, RCC_PLLxCFGR4_DSMEN);
	io_setbits32(pllxcfgr3, RCC_PLLxCFGR3_DACEN);
}

static int stm32_clk_configure_mux(struct clk_stm32_priv *priv, uint32_t data);

static inline const struct stm32_pll_dt_cfg *clk_stm32_pll_get_cfg(struct clk_stm32_priv *priv, int pll_idx)
{
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;

	return  &rcc_cfg->pll[pll_idx];
}

static int clk_stm32_pll_init(struct clk_stm32_priv *priv, int pll_idx,
			       const struct stm32_pll_dt_cfg *pll_conf)
{
	const struct stm32_clk_pll *pll = clk_stm32_pll_data(pll_idx);
	uintptr_t pllxcfgr1 = clk_stm32_get_rcc_base(priv) + pll->reg_pllxcfgr1;
	bool spread_spectrum = false;
	int ret = 0;

	/*
	 * TODO: check if pll has already good parameters or if we could make
	    a configuration on the fly.
	 */

	stm32_gate_rdy_disable(priv, pll->gate_id);

	ret = stm32_clk_configure_mux(priv, pll_conf->src);
	if (ret != 0)
		panic();

	ret = clk_stm32_pll_config_output(priv, pll,
					  pll_conf->src,
					  pll_conf->cfg,
					  pll_conf->frac);
	if (ret != 0)
		panic();

	if (pll_conf->csg_enabled == true) {
		clk_stm32_pll_config_csg(priv, pll, pll_conf->csg);
		spread_spectrum = true;
	}

	stm32_gate_rdy_enable(priv, pll->gate_id);

	if (spread_spectrum)
		io_clrbits32(pllxcfgr1, RCC_PLLxCFGR1_SSMODRST);

	return 0;
}

static int stm32_clk_pll_configure(struct clk_stm32_priv *priv)
{
	const struct stm32_pll_dt_cfg *pll_conf = NULL;
	int npll = priv->rcc_cfg->npll;
	size_t i = 0;

	for (i = 0; i < PLL_NB && i < npll; i++) {
		pll_conf = clk_stm32_pll_get_cfg(priv, i);

		if (pll_conf->enabled) {
			int err = 0;

			/* Skip the pll3 (need GPU regulator to configure) */
			if (i == PLL3_ID)
				continue;
#if !defined(STM32_BL2)
			/* Skip the pll2 (reserved to DDR) */
			if (i == PLL2_ID)
				continue;
#endif

			if (i == PLL1_ID) {
				continue;
			} else {
				err = clk_stm32_pll_init(priv, i, pll_conf);
				if (err)
					return err;
			}
		}
	}

	return 0;
}

static int wait_predivsr(uintptr_t rcc_base, uint16_t channel)
{
	uintptr_t previvsr;
	uint32_t channel_bit;
	uint64_t timeout;

	if (channel < __WORD_BIT) {
		previvsr = rcc_base + RCC_PREDIVSR1;
		channel_bit = BIT(channel);
	} else {
		previvsr = rcc_base + RCC_PREDIVSR2;
		channel_bit = BIT(channel - __WORD_BIT);
	}

	timeout = timeout_init_us(CLKDIV_TIMEOUT);
	while ((io_read32(previvsr) & channel_bit) != 0U) {
		if (timeout_elapsed(timeout)) {
			EMSG("Pre divider status: %x\n",
			      io_read32(previvsr));
			return -1;
		}
	}

	return 0;
}

static int wait_findivsr(uintptr_t rcc_base, uint16_t channel)
{
	uintptr_t finvivsr;
	uint32_t channel_bit;
	uint64_t timeout;

	if (channel < __WORD_BIT) {
		finvivsr = rcc_base + RCC_FINDIVSR1;
		channel_bit = BIT(channel);
	} else {
		finvivsr = rcc_base + RCC_FINDIVSR2;
		channel_bit = BIT(channel - __WORD_BIT);
	}

	timeout = timeout_init_us(CLKDIV_TIMEOUT);
	while ((io_read32(finvivsr) & channel_bit) != 0U) {
		if (timeout_elapsed(timeout)) {
			EMSG("Final divider status: %x\n",
			      io_read32(finvivsr));
			return -1;
		}
	}

	return 0;
}

static int wait_xbar_sts(uintptr_t rcc_base, uint16_t channel)
{
	uintptr_t xbar_cfgr = rcc_base + RCC_XBAR0CFGR + (0x4 * channel);
	uint64_t timeout;

	timeout = timeout_init_us(CLKDIV_TIMEOUT);
	while ((io_read32(xbar_cfgr) & RCC_XBARxCFGR_XBARxSTS_Msk) != 0U) {
		if (timeout_elapsed(timeout)) {
			EMSG("XBAR%dCFGR: %x\n", channel,
			      io_read32(xbar_cfgr));

			return -1;
		}
	}

	return 0;
}

static void flexclkgen_config_channel(struct clk_stm32_priv *priv,
				      uint16_t channel, unsigned int clk_src,
				      unsigned int prediv, unsigned int findiv)
{
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);

	if (wait_predivsr(rcc_base, channel) != 0)
		panic();

	io_clrsetbits32(rcc_base + RCC_PREDIV0CFGR + (0x4 * channel),
			   RCC_PREDIVxCFGR_PREDIVx_Msk,
			   prediv);

	if (wait_predivsr(rcc_base, channel) != 0)
		panic();

	if (wait_findivsr(rcc_base, channel) != 0)
		panic();

	io_clrsetbits32(rcc_base + RCC_FINDIV0CFGR + (0x4 * channel),
			RCC_FINDIVxCFGR_FINDIVx_Msk,
			findiv);

	if (wait_findivsr(rcc_base, channel) != 0)
		panic();

	if (wait_xbar_sts(rcc_base, channel) != 0)
		panic();

	io_clrsetbits32(rcc_base + RCC_XBAR0CFGR + (0x4 * channel),
			RCC_XBARxCFGR_XBARxSEL_Msk,
			clk_src);

	io_setbits32(rcc_base + RCC_XBAR0CFGR + (0x4 * channel),
		     RCC_XBARxCFGR_XBARxEN_Msk);

	if (wait_xbar_sts(rcc_base, channel) != 0)
		panic();
}

static int stm32mp2_clk_flexgen_configure(struct clk_stm32_priv *priv)
{
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	uint32_t i;

	for (i = 0; i < rcc_cfg->nflexgen; i++) {
		uint32_t val = rcc_cfg->flexgen[i];
		uint32_t cmd, cmd_data;
		unsigned int channel, clk_src, pdiv, fdiv;

		cmd = (val & CMD_MASK) >> CMD_SHIFT;
		cmd_data = val & ~CMD_MASK;

		if (cmd != CMD_FLEXGEN)
			continue;

		channel = (cmd_data & FLEX_ID_MASK) >> FLEX_ID_SHIFT;

		/*
		 * Skip ck_ker_stgen configuration, will be done by
		 * stgen driver.
		 */
		if (channel == 33U)
			continue;

		clk_src = (cmd_data & FLEX_SEL_MASK) >> FLEX_SEL_SHIFT;
		pdiv = (cmd_data & FLEX_PDIV_MASK) >> FLEX_PDIV_SHIFT;
		fdiv = (cmd_data & FLEX_FDIV_MASK) >> FLEX_FDIV_SHIFT;

		/* TODO: check if channel can be reconfigured */
		flexclkgen_config_channel(priv, channel, clk_src, pdiv, fdiv);
	}

	return 0;
}

static int stm32_clk_configure_div(__maybe_unused struct clk_stm32_priv *priv,
				   uint32_t data)
{
	int div_id, div_n;

	div_id = (data & DIV_ID_MASK) >> DIV_ID_SHIFT;
	div_n = (data & DIV_DIVN_MASK) >> DIV_DIVN_SHIFT;

	return stm32_div_set_value(priv, div_id, div_n);
}

static int stm32_clk_configure_mux(__maybe_unused struct clk_stm32_priv *priv,
				   uint32_t data)
{
	int mux = (data & MUX_ID_MASK) >> MUX_ID_SHIFT;
	int sel = (data & MUX_SEL_MASK) >> MUX_SEL_SHIFT;

	return stm32_mux_set_parent(priv, mux, sel);
}

static int stm32_clk_configure_by_addr_val(struct clk_stm32_priv *priv,
					   uint32_t data)
{
	uint32_t addr = data >> CLK_ADDR_SHIFT;
	uint32_t val = data & CLK_ADDR_VAL_MASK;

	io_setbits32(clk_stm32_get_rcc_base(priv) + addr, val);

	return 0;
}

static int stm32_clk_configure(struct clk_stm32_priv *priv, uint32_t val)
{
	uint32_t cmd_data = 0;
	uint32_t cmd = 0;
	int ret = 0;

	if (val & CMD_ADDR_BIT) {
		cmd_data = val & ~CMD_ADDR_BIT;

		return stm32_clk_configure_by_addr_val(priv, cmd_data);
	}

	cmd = (val & CMD_MASK) >> CMD_SHIFT;
	cmd_data = val & ~CMD_MASK;

	switch (cmd) {
	case CMD_DIV:
		ret = stm32_clk_configure_div(priv, cmd_data);
		break;

	case CMD_MUX:
		ret = stm32_clk_configure_mux(priv, cmd_data);
		break;

	default:
		EMSG("%s: cmd unknown ! : 0x%x\n", __func__, val);
		ret = -1;
	}

	return ret;
}

static int stm32_clk_bus_configure(struct clk_stm32_priv *priv)
{
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	uint32_t i;

	for (i = 0; i < rcc_cfg->nbusclk; i++) {
		int ret;

		ret = stm32_clk_configure(priv, rcc_cfg->busclk[i]);
		if (ret != 0)
			return ret;
	}

	return 0;
}

static int stm32_clk_kernel_configure(struct clk_stm32_priv *priv)
{
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	uint32_t i;

	for (i = 0; i < rcc_cfg->nkernelclk; i++) {
		int ret;

		ret = stm32_clk_configure(priv, rcc_cfg->kernelclk[i]);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int stm32mp2_init_clock_tree(struct clk_stm32_priv *priv)
{
	int ret;

	stm32_clk_oscillators_enable(priv);

	/* Come back to HSI for flexgen */
	stm32mp2_clk_xbar_on_hsi(priv);

	ret = stm32_clk_pll_configure(priv);
	if (ret != 0)
		panic();

	/* Wait LSE ready before to use it */
	ret = stm32_clk_oscillators_wait_lse_ready(priv);
	if (ret != 0)
		panic();

	ret = stm32mp2_clk_flexgen_configure(priv);
	if (ret != 0)
		panic();

	ret = stm32_clk_bus_configure(priv);
	if (ret != 0)
		panic();

	ret = stm32_clk_kernel_configure(priv);
	if (ret != 0)
		panic();

	/* Configure LSE css after RTC source configuration */
	ret = stm32_clk_oscillators_lse_set_css(priv);
	if (ret != 0)
		panic();

	return 0;
}


static int clk_stm32_osc_enable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return 0;

	if (clk_stm32_gate_ready_enable(clk)) {
#if !CLK_MINIMAL_SZ
		EMSG("%s timeout\n", clk_get_name(clk));
#endif
		panic();
	}

	return 0;
}

static void clk_stm32_osc_disable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return;

	clk_stm32_gate_ready_disable(clk);
}

static const struct clk_ops clk_stm32_osc_ops = {
	.enable		= clk_stm32_osc_enable,
	.disable	= clk_stm32_osc_disable,
	.is_enabled	= clk_stm32_gate_is_enabled,
};

static unsigned long clk_stm32_msi_get_rate(__maybe_unused struct clk *clk,
					    __maybe_unused unsigned long prate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);

	if ((io_read32(rcc_base + RCC_BDCR) & RCC_BDCR_MSIFREQSEL))
		return RCC_16_MHZ;

	return RCC_4_MHZ;
}

static int clk_stm32_msi_set_rate(__maybe_unused struct clk *clk,
				   unsigned long rate,
				   __maybe_unused unsigned long prate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return 0;

	return clk_stm32_osc_msi_set_rate(priv, rate);
}

static const struct clk_ops clk_stm32_osc_msi_ops = {
	.enable	= clk_stm32_osc_enable,
	.disable	= clk_stm32_osc_disable,
	.is_enabled	= clk_stm32_gate_is_enabled,
	.get_rate	= clk_stm32_msi_get_rate,
	.set_rate	= clk_stm32_msi_set_rate,
};

static int clk_stm32_hse_div_set_rate(struct clk *clk,
				       unsigned long rate,
				       unsigned long parent_rate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return 0;

	return clk_stm32_divider_set_rate(clk, rate, parent_rate);
}

static const struct clk_ops  clk_stm32_hse_div_ops = {
	.get_rate = clk_stm32_divider_get_rate,
	.set_rate = clk_stm32_hse_div_set_rate,
};

static int clk_stm32_hsediv2_enable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return 0;

	clk_stm32_gate_enable(clk);

	return 0;
}

static void clk_stm32_hsediv2_disable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_OSCILLATORS))
		return;

	clk_stm32_gate_disable(clk);
}

static unsigned long clk_stm32_hsediv2_get_rate(__maybe_unused struct clk *clk,
						unsigned long prate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	uintptr_t addr = rcc_base + RCC_OCENSETR;

	if ((io_read32(addr) & RCC_OCENSETR_HSEDIV2BYP) != 0U)
		return prate;

	return prate / 2;
}

static const struct clk_ops clk_hsediv2_ops = {
	.enable		= clk_stm32_hsediv2_enable,
	.disable	= clk_stm32_hsediv2_disable,
	.is_enabled	= clk_stm32_gate_is_enabled,
	.get_rate	= clk_stm32_hsediv2_get_rate,
};

struct clk_stm32_pll_cfg {
	uint32_t pll_offset;
	int gate_id;
	int mux_id;
};

static size_t clk_stm32_pll_get_parent(struct clk *clk)
{
	struct clk_stm32_pll_cfg *cfg = clk->priv;

	return stm32_mux_get_parent(clk, cfg->mux_id);
}

static unsigned long clk_get_pll_fvco(uint32_t pll_base,
				      unsigned long prate)
{
	uintptr_t pllxcfgr1 = pll_base;
	uintptr_t pllxcfgr2 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR2;
	uintptr_t pllxcfgr3 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR3;
	unsigned long fvco = 0UL;
	uint32_t fracin, fbdiv, refdiv;

	fracin = io_read32(pllxcfgr3) & RCC_PLLxCFGR3_FRACIN_MASK;
	fbdiv = (io_read32(pllxcfgr2) & RCC_PLLxCFGR2_FBDIV_MASK) >>
		RCC_PLLxCFGR2_FBDIV_SHIFT;

	refdiv = io_read32(pllxcfgr2) & RCC_PLLxCFGR2_FREFDIV_MASK;

	if (fracin != 0U) {
		uint64_t numerator, denominator;

		numerator = ((uint64_t)fbdiv << 24) + fracin;
		numerator = prate * numerator;
		denominator = (uint64_t)refdiv << 24;
		fvco = (unsigned long)(numerator / denominator);
	} else {
		fvco = ((uint64_t)prate * fbdiv) / refdiv;
	}

	return fvco;
}

static unsigned long clk_stm32_pll_get_rate(__maybe_unused struct clk *clk,
					    unsigned long prate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_pll_cfg *cfg = clk->priv;
	uintptr_t pllxcfgr1 = rcc_base + cfg->pll_offset;
	uintptr_t pllxcfgr4 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR4;
	uintptr_t pllxcfgr6 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR6;
	uintptr_t pllxcfgr7 = pllxcfgr1 + RCC_OFFSET_PLLXCFGR7;
	unsigned long dfout;
	uint32_t postdiv1, postdiv2;

	postdiv1 = io_read32(pllxcfgr6) & RCC_PLLxCFGR6_POSTDIV1_MASK;
	postdiv2 = io_read32(pllxcfgr7) & RCC_PLLxCFGR7_POSTDIV2_MASK;

	if ((io_read32(pllxcfgr4) & RCC_PLLxCFGR4_BYPASS) != 0U) {
		dfout = prate;
	} else {
		if (postdiv1 == 0U || postdiv2 == 0U)
			dfout = prate;
		else
			dfout = clk_get_pll_fvco(pllxcfgr1, prate)
				/ (postdiv1 * postdiv2);
	}

	return dfout;
}

static int clk_stm32_pll_enable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_pll_cfg *cfg = clk->priv;

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_PLL4_TO_8))
		return 0;

	if (stm32_gate_rdy_enable(priv, cfg->gate_id) != 0U) {
#if !CLK_MINIMAL_SZ
		EMSG("%s timeout\n", clk_get_name(clk));
#endif
		panic();
	}

	return 0;
}

static void clk_stm32_pll_disable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_pll_cfg *cfg = clk->priv;

	if (!stm32_rcc_has_access_by_id(priv, RCC_RIF_PLL4_TO_8))
		return;

	if (stm32_gate_rdy_disable(priv, cfg->gate_id) != 0U) {
#if !CLK_MINIMAL_SZ
		EMSG("%s timeout\n", clk_get_name(clk));
#endif
		panic();
	}
}

static bool clk_stm32_pll_is_enabled(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_pll_cfg *cfg = clk->priv;

	return stm32_gate_is_enabled(priv, cfg->gate_id);
}
static const struct clk_ops clk_stm32_pll_ops = {
	.get_parent	= clk_stm32_pll_get_parent,
	.get_rate	= clk_stm32_pll_get_rate,
	.enable		= clk_stm32_pll_enable,
	.disable	= clk_stm32_pll_disable,
	.is_enabled	= clk_stm32_pll_is_enabled,
};

#define XBAR_PARENTS { &ck_pll4, &ck_pll5, &ck_pll6, &ck_pll7, &ck_pll8,\
			&ck_hsi, &ck_hse, &ck_msi, &ck_hsi, &ck_hse, &ck_msi,\
			&spdifsymb, &i2sckin, &ck_lsi, &ck_lse }

struct clk_stm32_flexgen_cfg {
	int flex_id;
};

static size_t clk_stm32_flexgen_get_parent(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_flexgen_cfg *cfg = clk->priv;
	uint32_t address = 0;

	address = rcc_base + RCC_XBAR0CFGR + (cfg->flex_id * 4);

	return io_read32(address) & RCC_XBARxCFGR_XBARxSEL_Msk;
}

static int clk_stm32_flexgen_set_parent(struct clk *clk, size_t pidx)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_flexgen_cfg *cfg = clk->priv;
	uint16_t channel = cfg->flex_id * 4;

	if (!stm32_rcc_has_access_by_id(priv, cfg->flex_id))
		return 0;

	io_clrsetbits32(rcc_base + RCC_XBAR0CFGR + (channel),
			RCC_XBARxCFGR_XBARxSEL_Msk, pidx);

	if (wait_xbar_sts(rcc_base, channel) != 0)
		return -1;

	return 0;
}

static unsigned long clk_stm32_flexgen_get_rate(__maybe_unused struct clk *clk,
						unsigned long prate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_flexgen_cfg *cfg = clk->priv;
	uint32_t prediv, findiv;
	uint8_t channel = cfg->flex_id;
	unsigned long freq = prate;

	prediv = io_read32(rcc_base + RCC_PREDIV0CFGR + (0x4 * channel)) &
		RCC_PREDIVxCFGR_PREDIVx_Msk;
	findiv = io_read32(rcc_base + RCC_FINDIV0CFGR + (0x4 * channel)) &
		RCC_FINDIVxCFGR_FINDIVx_Msk;

	if (freq == 0)
		return 0;

	switch (prediv) {
	case 0x0:
		break;

	case 0x1:
		freq /= 2;
		break;

	case 0x3:
		freq /= 4;
		break;

	case 0x3FF:
		freq /= 1024;
		break;

	default:
		EMSG("Unsupported PREDIV value (%x)", prediv);
		panic();
		break;
	}

	freq /= (findiv + 1);

	return freq;
}

static int clk_stm32_flexgen_set_rate(struct clk *clk,
					     unsigned long rate,
					     unsigned long parent_rate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_flexgen_cfg *cfg = clk->priv;
	uint8_t channel = cfg->flex_id;
	// unsigned long ratio = UDIV_ROUND_NEAREST((uint64_t)parent_rate, rate);TO CHECK
	unsigned long ratio = div_round_up((uint64_t)parent_rate, rate);
	unsigned int prediv = 0;
	unsigned int findiv = 0;

	if (!stm32_rcc_has_access_by_id(priv, cfg->flex_id))
		return 0;

	if (ratio <= 64) {
		prediv = 0x0;
		findiv = ratio - 1;
	} else if (ratio <= 128) {
		prediv = 0x1;
		findiv = (ratio / 2) - 1;
	} else if (ratio <= 256) {
		prediv = 0x3;
		findiv = (ratio / 4) - 1;
	} else {
		prediv = 0x3FF;
		findiv = (ratio / 1024) - 1;
	}

	if (wait_predivsr(rcc_base, channel) != 0)
		panic();

	io_clrsetbits32(rcc_base + RCC_PREDIV0CFGR + (0x4 * channel),
			RCC_PREDIVxCFGR_PREDIVx_Msk,
			prediv);

	if (wait_predivsr(rcc_base, channel) != 0)
		panic();

	if (wait_findivsr(rcc_base, channel) != 0)
		panic();

	io_clrsetbits32(rcc_base + RCC_FINDIV0CFGR + (0x4 * channel),
			RCC_FINDIVxCFGR_FINDIVx_Msk,
			findiv);

	if (wait_findivsr(rcc_base, channel) != 0)
		panic();

	return 0;
}

static int clk_stm32_flexgen_enable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_flexgen_cfg *cfg = clk->priv;
	uint8_t channel = cfg->flex_id;

	if (!stm32_rcc_has_access_by_id(priv, cfg->flex_id))
		return 0;

	io_setbits32(rcc_base + RCC_FINDIV0CFGR + (0x4 * channel),
		     RCC_FINDIVxCFGR_FINDIVxEN);

	return 0;
}

static void clk_stm32_flexgen_disable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_flexgen_cfg *cfg = clk->priv;
	uint8_t channel = cfg->flex_id;

	if (!stm32_rcc_has_access_by_id(priv, cfg->flex_id))
		return 0;

	io_clrbits32(rcc_base + RCC_FINDIV0CFGR + (0x4 * channel),
		     RCC_FINDIVxCFGR_FINDIVxEN);
}

static bool clk_stm32_flexgen_is_enabled(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_flexgen_cfg *cfg = clk->priv;
	uint8_t channel = cfg->flex_id;

	return !!(io_read32(rcc_base + RCC_FINDIV0CFGR + (0x4 * channel)) &
		RCC_FINDIVxCFGR_FINDIVxEN);
}

static unsigned long clk_stm32_flexgen_round_rate(struct clk *clk __unused,
						  unsigned long rate,
						  unsigned long prate)
{
	unsigned long ratio = div_round_up((uint64_t)prate, rate);
	unsigned int prediv = 0;
	unsigned int findiv = 0;

	if (ratio <= 64) {
		prediv = 0x0;
		findiv = ratio - 1;
	} else if (ratio <= 128) {
		prediv = 0x1;
		findiv = (ratio / 2) - 1;
	} else if (ratio <= 256) {
		prediv = 0x3;
		findiv = (ratio / 4) - 1;
	} else {
		prediv = 0x3FF;
		findiv = (ratio / 1024) - 1;
	}

	return (prate / (prediv + 1)) / (findiv + 1);
}

static const struct clk_ops clk_stm32_flexgen_ops = {
	.get_rate	= clk_stm32_flexgen_get_rate,
	.set_rate	= clk_stm32_flexgen_set_rate,
	.get_parent	= clk_stm32_flexgen_get_parent,
	.set_parent	= clk_stm32_flexgen_set_parent,
	.enable		= clk_stm32_flexgen_enable,
	.disable	= clk_stm32_flexgen_disable,
	.is_enabled	= clk_stm32_flexgen_is_enabled,
	.round_rate	= clk_stm32_flexgen_round_rate,
};

#define APB_DIV_MASK	GENMASK_32(2, 0)
#define TIM_PRE_MASK	BIT(0)

static unsigned long ck_timer_get_rate_ops(struct clk *clk, unsigned long prate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	uintptr_t rcc_base = clk_stm32_get_rcc_base(priv);
	struct clk_stm32_timer_cfg *cfg = clk->priv;
	uint32_t prescaler, timpre;

	prescaler = io_read32(rcc_base + cfg->apbdiv) & APB_DIV_MASK;

	timpre = io_read32(rcc_base + cfg->timpre) & TIM_PRE_MASK;

	if (prescaler == 0U)
		return prate;

	return prate * (timpre + 1U) * 2U;
};

struct clk_stm32_rif_gate_cfg {
	int sec_id;
	int gate_id;
};

static int clk_stm32_rif_gate_enable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_gate_cfg *cfg = clk->priv;

	if (!stm32_rcc_has_access_by_id(priv, cfg->sec_id))
		return 0;

	stm32_gate_enable(priv, cfg->gate_id);

	return 0;
}

static void clk_stm32_rif_gate_disable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_gate_cfg *cfg = clk->priv;

	if (!stm32_rcc_has_access_by_id(priv, cfg->sec_id))
		return;

	stm32_gate_disable(priv, cfg->gate_id);
}

static bool clk_stm32_rif_gate_is_enabled(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_gate_cfg *cfg = clk->priv;

	return stm32_gate_is_enabled(priv, cfg->gate_id);
}

static const struct clk_ops  clk_stm32_rif_gate_ops = {
	.enable	= clk_stm32_rif_gate_enable,
	.disable	= clk_stm32_rif_gate_disable,
	.is_enabled	= clk_stm32_rif_gate_is_enabled,
};

struct clk_stm32_rif_composite_cfg {
	int sec_id;
	int gate_id;
	int div_id;
	int mux_id;
};

static size_t clk_stm32_rif_composite_get_parent(struct clk *clk)
{
	struct clk_stm32_rif_composite_cfg *cfg = clk->priv;

	if (cfg->mux_id == NO_MUX)
		return 0;

	return stm32_mux_get_parent(clk, cfg->mux_id);
}

static int clk_stm32_rif_composite_set_parent(struct clk *clk,
						     size_t pidx)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_composite_cfg *cfg = clk->priv;

	if (cfg->mux_id == NO_MUX)
		panic();

	if (!stm32_rcc_has_access_by_id(priv, cfg->sec_id))
		return 0;

	return stm32_mux_set_parent(priv, cfg->mux_id, pidx);
}

static unsigned long clk_stm32_rif_composite_get_rate(struct clk *clk,
						      unsigned long parent_rate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_composite_cfg *cfg = clk->priv;

	if (cfg->div_id == NO_DIV)
		return parent_rate;

	return stm32_div_get_rate(priv, cfg->div_id, parent_rate);
}

static int clk_stm32_rif_composite_set_rate(struct clk *clk,
						   unsigned long rate,
						   unsigned long parent_rate)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_composite_cfg *cfg = clk->priv;

	if (cfg->div_id == NO_DIV)
		return 0;

	if (!stm32_rcc_has_access_by_id(priv, cfg->sec_id))
		return 0;

	return stm32_div_set_rate(priv, cfg->div_id, rate, parent_rate);
}

static int clk_stm32_rif_composite_gate_enable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_composite_cfg *cfg = clk->priv;

	if (!stm32_rcc_has_access_by_id(priv, cfg->sec_id))
		return 0;

	stm32_gate_enable(priv, cfg->gate_id);

	return 0;
}

static void clk_stm32_rif_composite_gate_disable(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_composite_cfg *cfg = clk->priv;

	if (!stm32_rcc_has_access_by_id(priv, cfg->sec_id))
		return;

	stm32_gate_disable(priv, cfg->gate_id);
}

static bool clk_stm32_rif_composite_gate_is_enabled(struct clk *clk)
{
	struct clk_stm32_priv *priv = dev_get_data(clk_get_dev(clk));
	struct clk_stm32_rif_composite_cfg *cfg = clk->priv;

	return stm32_gate_is_enabled(priv, cfg->gate_id);
}

static const struct clk_ops clk_stm32_rif_composite_ops = {
	.get_parent	= clk_stm32_rif_composite_get_parent,
	.set_parent	= clk_stm32_rif_composite_set_parent,
	.get_rate	= clk_stm32_rif_composite_get_rate,
	.set_rate	= clk_stm32_rif_composite_set_rate,
	.enable		= clk_stm32_rif_composite_gate_enable,
	.disable	= clk_stm32_rif_composite_gate_disable,
	.is_enabled	= clk_stm32_rif_composite_gate_is_enabled,
};

const struct clk_ops ck_timer_ops = {
	.get_rate	= ck_timer_get_rate_ops,
};

#define STM32_OSC(_name, _parent, _flags, _gate_id)\
	struct clk _name = {\
		.ops = &clk_stm32_osc_ops,\
		.priv = &(struct clk_stm32_gate_cfg) {\
			.gate_id = _gate_id,\
		},\
		CLOCK_NAME(#_name)\
		.flags = (_flags),\
		.num_parents = 1,\
		.parents = PARENT(_parent),\
		.dev = DT_RCC_DEVICE,\
	}

#define STM32_OSC_MSI(_name, _parent, _flags, _gate_id)\
	struct clk _name = {\
		.ops = &clk_stm32_osc_msi_ops,\
		.priv = &(struct clk_stm32_gate_cfg) {\
			.gate_id = _gate_id,\
		},\
		CLOCK_NAME(#_name)\
		.flags = (_flags),\
		.num_parents = 1,\
		.parents = PARENT(_parent),\
		.dev = DT_RCC_DEVICE,\
	}

#define STM32_HSE_DIV2(_name, _parent, _flags, _gate_id)\
	struct clk _name = {\
		.ops = &clk_hsediv2_ops,\
		.priv = &(struct clk_stm32_gate_cfg) {\
			.gate_id = (_gate_id),\
		},\
		CLOCK_NAME(#_name)\
		.flags = (_flags),\
		.num_parents = 1,\
		.parents = PARENT(_parent),\
		.dev = DT_RCC_DEVICE,\
	}

#define STM32_HSE_RTC(_name, _parent, _flags, _div_id)\
	struct clk _name = {\
		.ops = &clk_stm32_hse_div_ops,\
		.priv = &(struct clk_stm32_div_cfg) {\
			.div_id = (_div_id),\
		},\
		CLOCK_NAME(#_name)\
		.flags = (_flags),\
		.num_parents = 1,\
		.parents = PARENT(_parent),\
		.dev = DT_RCC_DEVICE,\
	}

#define STM32_PLL2(_name, _flags, _reg, _gate_id, _mux_id)\
	struct clk _name = {\
		.ops	= &clk_stm32_pll_ops,\
		.priv		= &(struct clk_stm32_pll_cfg) {\
			.pll_offset	= (_reg),\
			.gate_id	= (_gate_id),\
			.mux_id		= (_mux_id),\
		},\
		CLOCK_NAME(#_name)\
		.flags		= (_flags),\
		.num_parents	= 3,\
		.parents	= (pll_parents),\
		.dev = DT_RCC_DEVICE,\
	}

#define STM32_PLLS(_name, _flags, _reg, _gate_id, _mux_id)\
	struct clk _name = {\
		.ops	= &clk_stm32_pll_ops,\
		.priv		= &(struct clk_stm32_pll_cfg) {\
			.pll_offset	= (_reg),\
			.gate_id	= (_gate_id),\
			.mux_id		= (_mux_id),\
		},\
		CLOCK_NAME(#_name)\
		.flags		= (_flags),\
		.num_parents	= 3,\
		.parents	= (pll_parents),\
		.dev = DT_RCC_DEVICE,\
	}

#define RIF_GATE(_name, _parent, _flags, _gate_id, _sec_id)\
	struct clk _name = {\
		.ops = &clk_stm32_rif_gate_ops,\
		.priv = &(struct clk_stm32_rif_gate_cfg) {\
			.sec_id	= (_sec_id),\
			.gate_id = (_gate_id),\
		},\
		CLOCK_NAME(#_name)\
		.flags = (_flags),\
		.num_parents = 1,\
		.parents = PARENT(_parent),\
		.dev = DT_RCC_DEVICE,\
	}

#define RIF_COMPOSITE(_name, _nb_parents, _parents, _flags,\
			_gate_id, _div_id, _mux_id, _sec_id)\
	struct clk _name = {\
		.ops = &clk_stm32_rif_composite_ops,\
		.priv = &(struct clk_stm32_rif_composite_cfg) {\
			.sec_id	= _sec_id,\
			.gate_id = (_gate_id),\
			.div_id = (_div_id),\
			.mux_id = (_mux_id),\
		},\
		CLOCK_NAME(#_name)\
		.flags = (_flags),\
		.num_parents = (_nb_parents),\
		.parents = _parents,\
		.dev = DT_RCC_DEVICE,\
	}

#define STM32_FLEXGEN(_name, _flags, _flex_id)\
struct clk _name = {\
	.ops	= &clk_stm32_flexgen_ops,\
	.priv		= &(struct clk_stm32_flexgen_cfg) {\
		.flex_id	= (_flex_id),\
	},\
	CLOCK_NAME(#_name)\
	.flags		= (_flags | CLK_SET_RATE_UNGATE),\
	.num_parents	= 15,\
	.parents	= xbar_parents,\
	.dev = DT_RCC_DEVICE,\
}

#define STM32_TIMER(_name, _parent, _flags, _apbdiv, _timpre)\
struct clk _name = {\
	.ops	= &ck_timer_ops,\
	.priv		= &(struct clk_stm32_timer_cfg) {\
		.apbdiv		= (_apbdiv),\
		.timpre		= (_timpre),\
	},\
	CLOCK_NAME(#_name)\
	.flags		= (0 | (_flags)),\
	.num_parents	= 1,\
	.parents = PARENT(_parent),\
	.dev = DT_RCC_DEVICE,\
}

#define STM32_DT_OSC(_name)\
	struct clk _name = {\
		.ops = &clk_fixed_clk_ops,\
		.priv = &(struct clk_fixed_rate_cfg) {\
			.rate = 0,\
		},\
		CLOCK_NAME(#_name)\
		.flags = 0,\
		.num_parents = 0,\
		.dev = DT_RCC_DEVICE,\
	}

static STM32_FIXED_RATE(ck_off, RCC_0_MHZ);

static STM32_FIXED_RATE(ck_obser0, RCC_0_MHZ);
static STM32_FIXED_RATE(ck_obser1, RCC_0_MHZ);
static STM32_FIXED_RATE(spdifsymb, RCC_0_MHZ);
static STM32_FIXED_RATE(ck_dsi_phy, RCC_0_MHZ);
static STM32_FIXED_RATE(i2sckin, RCC_0_MHZ);

static STM32_DT_OSC(clk_hsi);
static STM32_DT_OSC(clk_hse);
static STM32_DT_OSC(clk_msi);
static STM32_DT_OSC(clk_lsi);
static STM32_DT_OSC(clk_lse);

/* Oscillator clocks */
static STM32_OSC(ck_hsi, &clk_hsi,0, GATE_HSI);
static STM32_OSC(ck_hse, &clk_hse, 0, GATE_HSE);
static STM32_OSC_MSI(ck_msi, &clk_msi, 0, GATE_MSI);
static STM32_OSC(ck_lsi, &clk_lsi, 0, GATE_LSI);
static STM32_OSC(ck_lse, &clk_lse, 0, GATE_LSE);

static struct clk *pll_parents[] = {
	&ck_hsi,
	&ck_hse,
	&ck_msi
};

static STM32_HSE_DIV2(ck_hse_div2, &ck_hse, 0, GATE_HSEDIV2);
static STM32_HSE_RTC(ck_hse_rtc, &ck_hse, 0, DIV_RTC);

/* PLL clocks */
static STM32_PLL2(ck_pll2, 0, RCC_PLL2CFGR1, GATE_PLL2, MUX_MUXSEL6);
static STM32_PLLS(ck_pll4, 0, RCC_PLL4CFGR1, GATE_PLL4, MUX_MUXSEL0);
static STM32_PLLS(ck_pll5, 0, RCC_PLL5CFGR1, GATE_PLL5, MUX_MUXSEL1);
static STM32_PLLS(ck_pll6, 0, RCC_PLL6CFGR1, GATE_PLL6, MUX_MUXSEL2);
static STM32_PLLS(ck_pll7, 0, RCC_PLL7CFGR1, GATE_PLL7, MUX_MUXSEL3);
static STM32_PLLS(ck_pll8, 0, RCC_PLL8CFGR1, GATE_PLL8, MUX_MUXSEL4);

static struct clk *xbar_parents[] = {
	&ck_pll4,
	&ck_pll5,
	&ck_pll6,
	&ck_pll7,
	&ck_pll8,
	&ck_hsi,
	&ck_hse,
	&ck_msi,
	&ck_hsi,
	&ck_hse,
	&ck_msi,
	&spdifsymb,
	&i2sckin,
	&ck_lsi,
	&ck_lse
};

/* Flexgen clocks */
static STM32_FLEXGEN(ck_icn_hs_mcu, 0, 0);
static STM32_FLEXGEN(ck_icn_sdmmc, 0, 1);
static STM32_FLEXGEN(ck_icn_ddr, 0, 2);
static STM32_FLEXGEN(ck_icn_display, 0, 3);
static STM32_FLEXGEN(ck_icn_hsl, 0, 4);
static STM32_FLEXGEN(ck_icn_nic, 0, 5);
static STM32_FLEXGEN(ck_icn_vid, 0, 6);
static STM32_DIVIDER(ck_icn_ls_mcu, &ck_icn_hs_mcu, 0, DIV_LSMCU);
static STM32_FLEXGEN(ck_flexgen_07, 0, 7);
static STM32_FLEXGEN(ck_flexgen_08, 0, 8);
static STM32_FLEXGEN(ck_flexgen_09, 0, 9);
static STM32_FLEXGEN(ck_flexgen_10, 0, 10);
static STM32_FLEXGEN(ck_flexgen_11, 0, 11);
static STM32_FLEXGEN(ck_flexgen_12, 0, 12);
static STM32_FLEXGEN(ck_flexgen_13, 0, 13);
static STM32_FLEXGEN(ck_flexgen_14, 0, 14);
static STM32_FLEXGEN(ck_flexgen_15, 0, 15);
static STM32_FLEXGEN(ck_flexgen_16, 0, 16);
static STM32_FLEXGEN(ck_flexgen_17, 0, 17);
static STM32_FLEXGEN(ck_flexgen_18, 0, 18);
static STM32_FLEXGEN(ck_flexgen_19, 0, 19);
static STM32_FLEXGEN(ck_flexgen_20, 0, 20);
static STM32_FLEXGEN(ck_flexgen_21, 0, 21);
static STM32_FLEXGEN(ck_flexgen_22, 0, 22);
static STM32_FLEXGEN(ck_flexgen_23, 0, 23);
static STM32_FLEXGEN(ck_flexgen_24, 0, 24);
static STM32_FLEXGEN(ck_flexgen_25, 0, 25);
static STM32_FLEXGEN(ck_flexgen_26, 0, 26);
static STM32_FLEXGEN(ck_flexgen_27, 0, 27);
static STM32_FLEXGEN(ck_flexgen_28, 0, 28);
static STM32_FLEXGEN(ck_flexgen_29, 0, 29);
static STM32_FLEXGEN(ck_flexgen_30, 0, 30);
static STM32_FLEXGEN(ck_flexgen_31, 0, 31);
static STM32_FLEXGEN(ck_flexgen_32, 0, 32);
static STM32_FLEXGEN(ck_flexgen_33, 0, 33);
static STM32_FLEXGEN(ck_flexgen_34, 0, 34);
static STM32_FLEXGEN(ck_flexgen_35, 0, 35);
static STM32_FLEXGEN(ck_flexgen_36, 0, 36);
static STM32_FLEXGEN(ck_flexgen_37, 0, 37);
static STM32_FLEXGEN(ck_flexgen_38, 0, 38);
static STM32_FLEXGEN(ck_flexgen_39, 0, 39);
static STM32_FLEXGEN(ck_flexgen_40, 0, 40);
static STM32_FLEXGEN(ck_flexgen_41, 0, 41);
static STM32_FLEXGEN(ck_flexgen_42, 0, 42);
static STM32_FLEXGEN(ck_flexgen_43, 0, 43);
static STM32_FLEXGEN(ck_flexgen_44, 0, 44);
static STM32_FLEXGEN(ck_flexgen_45, 0, 45);
static STM32_FLEXGEN(ck_flexgen_46, 0, 46);
static STM32_FLEXGEN(ck_flexgen_47, 0, 47);
static STM32_FLEXGEN(ck_flexgen_48, 0, 48);
static STM32_FLEXGEN(ck_flexgen_49, 0, 49);
static STM32_FLEXGEN(ck_flexgen_50, 0, 50);
static STM32_FLEXGEN(ck_flexgen_51, 0, 51);
static STM32_FLEXGEN(ck_flexgen_52, 0, 52);
static STM32_FLEXGEN(ck_flexgen_53, 0, 53);
static STM32_FLEXGEN(ck_flexgen_54, 0, 54);
static STM32_FLEXGEN(ck_flexgen_55, 0, 55);
static STM32_FLEXGEN(ck_flexgen_56, 0, 56);
static STM32_FLEXGEN(ck_flexgen_57, 0, 57);
static STM32_FLEXGEN(ck_flexgen_58, 0, 58);
static STM32_FLEXGEN(ck_flexgen_59, 0, 59);
static STM32_FLEXGEN(ck_flexgen_60, 0, 60);
static STM32_FLEXGEN(ck_flexgen_61, 0, 61);
static STM32_FLEXGEN(ck_flexgen_62, 0, 62);
static STM32_FLEXGEN(ck_flexgen_63, 0, 63);

/* Bus Clocks */
static STM32_DIVIDER(ck_icn_apb1, &ck_icn_ls_mcu, 0, DIV_APB1);
static STM32_DIVIDER(ck_icn_apb2, &ck_icn_ls_mcu, 0, DIV_APB2);
static STM32_DIVIDER(ck_icn_apb3, &ck_icn_ls_mcu, 0, DIV_APB3);
static STM32_DIVIDER(ck_icn_apb4, &ck_icn_ls_mcu, 0, DIV_APB4);
static STM32_DIVIDER(ck_icn_apbdbg, &ck_icn_ls_mcu, 0, DIV_APBDBG);

/* Kernel Timers */
static STM32_TIMER(ck_timg1, &ck_icn_apb1, 0, RCC_APB1DIVR, RCC_TIMG1PRER);
static STM32_TIMER(ck_timg2, &ck_icn_apb2, 0, RCC_APB2DIVR, RCC_TIMG2PRER);

/* Clocks under RCC RIF protection */
static RIF_GATE(ck_sys_dbg, &ck_icn_apbdbg, 0, GATE_DBG, RCC_RIF_DEBUG_TRACE);
static RIF_GATE(ck_icn_s_stm500, &ck_icn_ls_mcu, 0, GATE_STM500,
		RCC_RIF_DEBUG_TRACE);
static RIF_GATE(ck_ker_tsdbg, &ck_flexgen_43, 0, GATE_DBG, RCC_RIF_DEBUG_TRACE);
static RIF_GATE(ck_ker_tpiu, &ck_flexgen_44, 0, GATE_TRACE,
		RCC_RIF_DEBUG_TRACE);
static RIF_GATE(ck_icn_m_etr, &ck_flexgen_45, 0, GATE_ETR, RCC_RIF_DEBUG_TRACE);
static RIF_GATE(ck_sys_atb, &ck_flexgen_45, 0, GATE_DBG, RCC_RIF_DEBUG_TRACE);
static RIF_GATE(ck_icn_s_sysram, &ck_icn_hs_mcu, 0, GATE_SYSRAM,
		RCC_RIF_SYSRAM);
static RIF_GATE(ck_icn_s_vderam, &ck_icn_hs_mcu, 0, GATE_VDERAM,
		RCC_RIF_VDERAM);
static RIF_GATE(ck_icn_s_retram, &ck_icn_hs_mcu, 0, GATE_RETRAM,
		RCC_RIF_RETRAM);
static RIF_GATE(ck_icn_s_bkpsram, &ck_icn_ls_mcu, 0, GATE_BKPSRAM,
		RCC_RIF_BKPSRAM);
static RIF_GATE(ck_icn_s_sram1, &ck_icn_hs_mcu, 0, GATE_SRAM1, RCC_RIF_SRAM1);
static RIF_GATE(ck_icn_s_sram2, &ck_icn_hs_mcu, 0, GATE_SRAM2, RCC_RIF_SRAM2);
static RIF_GATE(ck_icn_s_lpsram1, &ck_icn_ls_mcu, 0, GATE_LPSRAM1,
		RCC_RIF_LPSRAM1);
static RIF_GATE(ck_icn_s_lpsram2, &ck_icn_ls_mcu, 0, GATE_LPSRAM2,
		RCC_RIF_LPSRAM2);
static RIF_GATE(ck_icn_s_lpsram3, &ck_icn_ls_mcu, 0, GATE_LPSRAM3,
		RCC_RIF_LPSRAM3);
static RIF_GATE(ck_icn_p_hpdma1, &ck_icn_ls_mcu, 0, GATE_HPDMA1,
		RCC_RIF_HPDMA1);
static RIF_GATE(ck_icn_p_hpdma2, &ck_icn_ls_mcu, 0, GATE_HPDMA2,
		RCC_RIF_HPDMA2);
static RIF_GATE(ck_icn_p_hpdma3, &ck_icn_ls_mcu, 0, GATE_HPDMA3,
		RCC_RIF_HPDMA3);
static RIF_GATE(ck_icn_p_lpdma, &ck_icn_ls_mcu, 0, GATE_LPDMA,
		RCC_RIF_LPDMA);
static RIF_GATE(ck_icn_p_ipcc1, &ck_icn_ls_mcu, 0, GATE_IPCC1, RCC_RIF_IPCC1);
static RIF_GATE(ck_icn_p_ipcc2, &ck_icn_ls_mcu, 0, GATE_IPCC2, RCC_RIF_IPCC2);
static RIF_GATE(ck_icn_p_hsem, &ck_icn_ls_mcu, 0, GATE_HSEM, RCC_RIF_HSEM);
static RIF_GATE(ck_icn_p_gpioa, &ck_icn_ls_mcu, 0, GATE_GPIOA, RCC_RIF_GPIOA);
static RIF_GATE(ck_icn_p_gpiob, &ck_icn_ls_mcu, 0, GATE_GPIOB, RCC_RIF_GPIOB);
static RIF_GATE(ck_icn_p_gpioc, &ck_icn_ls_mcu, 0, GATE_GPIOC, RCC_RIF_GPIOC);
static RIF_GATE(ck_icn_p_gpiod, &ck_icn_ls_mcu, 0, GATE_GPIOD, RCC_RIF_GPIOD);
static RIF_GATE(ck_icn_p_gpioe, &ck_icn_ls_mcu, 0, GATE_GPIOE, RCC_RIF_GPIOE);
static RIF_GATE(ck_icn_p_gpiof, &ck_icn_ls_mcu, 0, GATE_GPIOF, RCC_RIF_GPIOF);
static RIF_GATE(ck_icn_p_gpiog, &ck_icn_ls_mcu, 0, GATE_GPIOG, RCC_RIF_GPIOG);
static RIF_GATE(ck_icn_p_gpioh, &ck_icn_ls_mcu, 0, GATE_GPIOH, RCC_RIF_GPIOH);
static RIF_GATE(ck_icn_p_gpioi, &ck_icn_ls_mcu, 0, GATE_GPIOI, RCC_RIF_GPIOI);
static RIF_GATE(ck_icn_p_gpioj, &ck_icn_ls_mcu, 0, GATE_GPIOJ, RCC_RIF_GPIOJ);
static RIF_GATE(ck_icn_p_gpiok, &ck_icn_ls_mcu, 0, GATE_GPIOK, RCC_RIF_GPIOK);
static RIF_GATE(ck_icn_p_gpioz, &ck_icn_ls_mcu, 0, GATE_GPIOZ, RCC_RIF_GPIOZ);
static RIF_GATE(ck_icn_p_rtc, &ck_icn_ls_mcu, 0, GATE_RTC, RCC_RIF_RTC_TAMP);
static RIF_COMPOSITE(ck_rtc, 4, PARENTS(&ck_off, &ck_lse, &ck_lsi, &ck_hse_rtc),
		     0, GATE_RTCCK, NO_DIV, MUX_RTC, RCC_RIF_RTC_TAMP);
static RIF_GATE(ck_icn_p_bsec, &ck_icn_apb3, 0, GATE_BSEC, RCC_RIF_BSEC);
static RIF_GATE(ck_icn_p_ddrphyc, &ck_icn_ls_mcu, 0, GATE_DDRPHYCAPB,
		RCC_RIF_DDR_PLL2);
static RIF_GATE(ck_icn_p_risaf4, &ck_icn_ls_mcu, 0, GATE_DDRCP,
		RCC_RIF_DDR_PLL2);
static RIF_GATE(ck_icn_s_ddr, &ck_icn_ddr, 0, GATE_DDRCP, RCC_RIF_DDR_PLL2);
static RIF_GATE(ck_icn_p_ddrc, &ck_icn_apb4, 0, GATE_DDRCAPB,
		RCC_RIF_DDR_PLL2);
static RIF_GATE(ck_icn_p_ddrcfg, &ck_icn_apb4, 0, GATE_DDRCFG,
		RCC_RIF_DDR_PLL2);
static RIF_GATE(ck_icn_p_syscpu1, &ck_icn_ls_mcu, 0, GATE_SYSCPU1,
		RCC_RIF_SYSCPU1);
static RIF_GATE(ck_icn_p_is2m, &ck_icn_apb3, 0, GATE_IS2M, RCC_RIF_IS2M);
static RIF_COMPOSITE(ck_mco1, 2, PARENTS(&ck_flexgen_61, &ck_obser0), 0,
		     GATE_MCO1, NO_DIV, MUX_MCO1, RCC_RIF_MCO1);
static RIF_COMPOSITE(ck_mco2, 2, PARENTS(&ck_flexgen_62, &ck_obser1), 0,
		     GATE_MCO2, NO_DIV, MUX_MCO2, RCC_RIF_MCO2);
static RIF_GATE(ck_icn_s_ospi1, &ck_icn_hs_mcu, 0, GATE_OSPI1, RCC_RIF_OSPI1);
static RIF_GATE(ck_ker_ospi1, &ck_flexgen_48, 0, GATE_OSPI1, RCC_RIF_OSPI1);
static RIF_GATE(ck_icn_s_ospi2, &ck_icn_hs_mcu, 0, GATE_OSPI2, RCC_RIF_OSPI2);
static RIF_GATE(ck_ker_ospi2, &ck_flexgen_49, 0, GATE_OSPI2, RCC_RIF_OSPI2);
static RIF_GATE(ck_icn_p_fmc, &ck_icn_ls_mcu, 0, GATE_FMC, RCC_RIF_FMC);
static RIF_GATE(ck_ker_fmc, &ck_flexgen_50, 0, GATE_FMC, RCC_RIF_FMC);

/* Kernel Clocks */
static STM32_GATE(ck_icn_p_cci, &ck_icn_ls_mcu, 0, GATE_CCI);
static STM32_GATE(ck_icn_p_crc, &ck_icn_ls_mcu, 0, GATE_CRC);
static STM32_GATE(ck_icn_p_ospiiom, &ck_icn_ls_mcu, 0, GATE_OSPIIOM);
static STM32_GATE(ck_icn_p_hash, &ck_icn_ls_mcu, 0, GATE_HASH);
static STM32_GATE(ck_icn_p_rng, &ck_icn_ls_mcu, 0, GATE_RNG);
static STM32_GATE(ck_icn_p_cryp1, &ck_icn_ls_mcu, 0, GATE_CRYP1);
static STM32_GATE(ck_icn_p_cryp2, &ck_icn_ls_mcu, 0, GATE_CRYP2);
static STM32_GATE(ck_icn_p_saes, &ck_icn_ls_mcu, 0, GATE_SAES);
static STM32_GATE(ck_icn_p_pka, &ck_icn_ls_mcu, 0, GATE_PKA);
static STM32_GATE(ck_icn_p_adf1, &ck_icn_ls_mcu, 0, GATE_ADF1);
static STM32_GATE(ck_icn_p_iwdg5, &ck_icn_ls_mcu, 0, GATE_IWDG5);
static STM32_GATE(ck_icn_p_wwdg2, &ck_icn_ls_mcu, 0, GATE_WWDG2);
static STM32_GATE(ck_icn_p_eth1, &ck_icn_ls_mcu, 0, GATE_ETH1);
static STM32_GATE(ck_icn_p_ethsw, &ck_icn_ls_mcu, 0, GATE_ETHSWMAC);
static STM32_GATE(ck_icn_p_eth2, &ck_icn_ls_mcu, 0, GATE_ETH2);
static STM32_GATE(ck_icn_p_pcie, &ck_icn_ls_mcu, 0, GATE_PCIE);
static STM32_GATE(ck_icn_p_adc12, &ck_icn_ls_mcu, 0, GATE_ADC12);
static STM32_GATE(ck_icn_p_adc3, &ck_icn_ls_mcu, 0, GATE_ADC3);
static STM32_GATE(ck_icn_p_mdf1, &ck_icn_ls_mcu, 0, GATE_MDF1);
static STM32_GATE(ck_icn_p_spi8, &ck_icn_ls_mcu, 0, GATE_SPI8);
static STM32_GATE(ck_icn_p_lpuart1, &ck_icn_ls_mcu, 0, GATE_LPUART1);
static STM32_GATE(ck_icn_p_i2c8, &ck_icn_ls_mcu, 0, GATE_I2C8);
static STM32_GATE(ck_icn_p_lptim3, &ck_icn_ls_mcu, 0, GATE_LPTIM3);
static STM32_GATE(ck_icn_p_lptim4, &ck_icn_ls_mcu, 0, GATE_LPTIM4);
static STM32_GATE(ck_icn_p_lptim5, &ck_icn_ls_mcu, 0, GATE_LPTIM5);
static STM32_GATE(ck_icn_m_sdmmc1, &ck_icn_sdmmc, 0, GATE_SDMMC1);
static STM32_GATE(ck_icn_m_sdmmc2, &ck_icn_sdmmc, 0, GATE_SDMMC2);
static STM32_GATE(ck_icn_m_sdmmc3, &ck_icn_sdmmc, 0, GATE_SDMMC3);
static STM32_GATE(ck_icn_m_usb2ohci, &ck_icn_hsl, 0, GATE_USB2);
static STM32_GATE(ck_icn_m_usb2ehci, &ck_icn_hsl, 0, GATE_USB2);
static STM32_GATE(ck_icn_m_usb3drd, &ck_icn_hsl, 0, GATE_USB3DRD);
static STM32_GATE(ck_icn_p_tim2, &ck_icn_apb1, 0, GATE_TIM2);
static STM32_GATE(ck_icn_p_tim3, &ck_icn_apb1, 0, GATE_TIM3);
static STM32_GATE(ck_icn_p_tim4, &ck_icn_apb1, 0, GATE_TIM4);
static STM32_GATE(ck_icn_p_tim5, &ck_icn_apb1, 0, GATE_TIM5);
static STM32_GATE(ck_icn_p_tim6, &ck_icn_apb1, 0, GATE_TIM6);
static STM32_GATE(ck_icn_p_tim7, &ck_icn_apb1, 0, GATE_TIM7);
static STM32_GATE(ck_icn_p_tim10, &ck_icn_apb1, 0, GATE_TIM10);
static STM32_GATE(ck_icn_p_tim11, &ck_icn_apb1, 0, GATE_TIM11);
static STM32_GATE(ck_icn_p_tim12, &ck_icn_apb1, 0, GATE_TIM12);
static STM32_GATE(ck_icn_p_tim13, &ck_icn_apb1, 0, GATE_TIM13);
static STM32_GATE(ck_icn_p_tim14, &ck_icn_apb1, 0, GATE_TIM14);
static STM32_GATE(ck_icn_p_lptim1, &ck_icn_apb1, 0, GATE_LPTIM1);
static STM32_GATE(ck_icn_p_lptim2, &ck_icn_apb1, 0, GATE_LPTIM2);
static STM32_GATE(ck_icn_p_spi2, &ck_icn_apb1, 0, GATE_SPI2);
static STM32_GATE(ck_icn_p_spi3, &ck_icn_apb1, 0, GATE_SPI3);
static STM32_GATE(ck_icn_p_spdifrx, &ck_icn_apb1, 0, GATE_SPDIFRX);
static STM32_GATE(ck_icn_p_usart2, &ck_icn_apb1, 0, GATE_USART2);
static STM32_GATE(ck_icn_p_usart3, &ck_icn_apb1, 0, GATE_USART3);
static STM32_GATE(ck_icn_p_uart4, &ck_icn_apb1, 0, GATE_UART4);
static STM32_GATE(ck_icn_p_uart5, &ck_icn_apb1, 0, GATE_UART5);
static STM32_GATE(ck_icn_p_i2c1, &ck_icn_apb1, 0, GATE_I2C1);
static STM32_GATE(ck_icn_p_i2c2, &ck_icn_apb1, 0, GATE_I2C2);
static STM32_GATE(ck_icn_p_i2c3, &ck_icn_apb1, 0, GATE_I2C3);
static STM32_GATE(ck_icn_p_i2c4, &ck_icn_apb1, 0, GATE_I2C4);
static STM32_GATE(ck_icn_p_i2c5, &ck_icn_apb1, 0, GATE_I2C5);
static STM32_GATE(ck_icn_p_i2c6, &ck_icn_apb1, 0, GATE_I2C6);
static STM32_GATE(ck_icn_p_i2c7, &ck_icn_apb1, 0, GATE_I2C7);
static STM32_GATE(ck_icn_p_i3c1, &ck_icn_apb1, 0, GATE_I3C1);
static STM32_GATE(ck_icn_p_i3c2, &ck_icn_apb1, 0, GATE_I3C2);
static STM32_GATE(ck_icn_p_i3c3, &ck_icn_apb1, 0, GATE_I3C3);
static STM32_GATE(ck_icn_p_i3c4, &ck_icn_ls_mcu, 0, GATE_I3C4);
static STM32_GATE(ck_icn_p_tim1, &ck_icn_apb2, 0, GATE_TIM1);
static STM32_GATE(ck_icn_p_tim8, &ck_icn_apb2, 0, GATE_TIM8);
static STM32_GATE(ck_icn_p_tim15, &ck_icn_apb2, 0, GATE_TIM15);
static STM32_GATE(ck_icn_p_tim16, &ck_icn_apb2, 0, GATE_TIM16);
static STM32_GATE(ck_icn_p_tim17, &ck_icn_apb2, 0, GATE_TIM17);
static STM32_GATE(ck_icn_p_tim20, &ck_icn_apb2, 0, GATE_TIM20);
static STM32_GATE(ck_icn_p_sai1, &ck_icn_apb2, 0, GATE_SAI1);
static STM32_GATE(ck_icn_p_sai2, &ck_icn_apb2, 0, GATE_SAI2);
static STM32_GATE(ck_icn_p_sai3, &ck_icn_apb2, 0, GATE_SAI3);
static STM32_GATE(ck_icn_p_sai4, &ck_icn_apb2, 0, GATE_SAI4);
static STM32_GATE(ck_icn_p_usart1, &ck_icn_apb2, 0, GATE_USART1);
static STM32_GATE(ck_icn_p_usart6, &ck_icn_apb2, 0, GATE_USART6);
static STM32_GATE(ck_icn_p_uart7, &ck_icn_apb2, 0, GATE_UART7);
static STM32_GATE(ck_icn_p_uart8, &ck_icn_apb2, 0, GATE_UART8);
static STM32_GATE(ck_icn_p_uart9, &ck_icn_apb2, 0, GATE_UART9);
static STM32_GATE(ck_icn_p_fdcan, &ck_icn_apb2, 0, GATE_FDCAN);
static STM32_GATE(ck_icn_p_spi1, &ck_icn_apb2, 0, GATE_SPI1);
static STM32_GATE(ck_icn_p_spi4, &ck_icn_apb2, 0, GATE_SPI4);
static STM32_GATE(ck_icn_p_spi5, &ck_icn_apb2, 0, GATE_SPI5);
static STM32_GATE(ck_icn_p_spi6, &ck_icn_apb2, 0, GATE_SPI6);
static STM32_GATE(ck_icn_p_spi7, &ck_icn_apb2, 0, GATE_SPI7);
static STM32_GATE(ck_icn_p_iwdg1, &ck_icn_apb3, 0, GATE_IWDG1);
static STM32_GATE(ck_icn_p_iwdg2, &ck_icn_apb3, 0, GATE_IWDG2);
static STM32_GATE(ck_icn_p_iwdg3, &ck_icn_apb3, 0, GATE_IWDG3);
static STM32_GATE(ck_icn_p_iwdg4, &ck_icn_apb3, 0, GATE_IWDG4);
static STM32_GATE(ck_icn_p_wwdg1, &ck_icn_apb3, 0, GATE_WWDG1);
static STM32_GATE(ck_icn_p_vref, &ck_icn_apb3, 0, GATE_VREF);
static STM32_GATE(ck_icn_p_dts, &ck_icn_apb3, 0, GATE_DTS);
static STM32_GATE(ck_icn_p_serc, &ck_icn_apb3, 0, GATE_SERC);
static STM32_GATE(ck_icn_p_hdp, &ck_icn_apb3, 0, GATE_HDP);
static STM32_GATE(ck_icn_p_dsi, &ck_icn_apb4, 0, GATE_DSI);
static STM32_GATE(ck_icn_p_ltdc, &ck_icn_apb4, 0, GATE_LTDC);
static STM32_GATE(ck_icn_p_csi2, &ck_icn_apb4, 0, GATE_CSI);
static STM32_GATE(ck_icn_p_dcmipp, &ck_icn_apb4, 0, GATE_DCMIPP);
static STM32_GATE(ck_icn_p_lvds, &ck_icn_apb4, 0, GATE_LVDS);
static STM32_GATE(ck_icn_p_gicv2m, &ck_icn_apb4, 0, GATE_GICV2M);
static STM32_GATE(ck_icn_p_usbtc, &ck_icn_apb4, 0, GATE_USBTC);
static STM32_GATE(ck_icn_p_busperfm, &ck_icn_apb4, 0, GATE_BUSPERFM);
static STM32_GATE(ck_icn_p_usb3pciephy, &ck_icn_apb4, 0, GATE_USB3PCIEPHY);
static STM32_GATE(ck_icn_p_stgen, &ck_icn_apb4, 0, GATE_STGEN);
static STM32_GATE(ck_icn_p_vdec, &ck_icn_apb4, 0, GATE_VDEC);
static STM32_GATE(ck_icn_p_venc, &ck_icn_apb4, 0, GATE_VENC);
static STM32_GATE(ck_ker_tim2, &ck_timg1, 0, GATE_TIM2);
static STM32_GATE(ck_ker_tim3, &ck_timg1, 0, GATE_TIM3);
static STM32_GATE(ck_ker_tim4, &ck_timg1, 0, GATE_TIM4);
static STM32_GATE(ck_ker_tim5, &ck_timg1, 0, GATE_TIM5);
static STM32_GATE(ck_ker_tim6, &ck_timg1, 0, GATE_TIM6);
static STM32_GATE(ck_ker_tim7, &ck_timg1, 0, GATE_TIM7);
static STM32_GATE(ck_ker_tim10, &ck_timg1, 0, GATE_TIM10);
static STM32_GATE(ck_ker_tim11, &ck_timg1, 0, GATE_TIM11);
static STM32_GATE(ck_ker_tim12, &ck_timg1, 0, GATE_TIM12);
static STM32_GATE(ck_ker_tim13, &ck_timg1, 0, GATE_TIM13);
static STM32_GATE(ck_ker_tim14, &ck_timg1, 0, GATE_TIM14);
static STM32_GATE(ck_ker_tim1, &ck_timg2, 0, GATE_TIM1);
static STM32_GATE(ck_ker_tim8, &ck_timg2, 0, GATE_TIM8);
static STM32_GATE(ck_ker_tim15, &ck_timg2, 0, GATE_TIM15);
static STM32_GATE(ck_ker_tim16, &ck_timg2, 0, GATE_TIM16);
static STM32_GATE(ck_ker_tim17, &ck_timg2, 0, GATE_TIM17);
static STM32_GATE(ck_ker_tim20, &ck_timg2, 0, GATE_TIM20);
static STM32_GATE(ck_ker_lptim1, &ck_flexgen_07, 0, GATE_LPTIM1);
static STM32_GATE(ck_ker_lptim2, &ck_flexgen_07, 0, GATE_LPTIM2);
static STM32_GATE(ck_ker_usart2, &ck_flexgen_08, 0, GATE_USART2);
static STM32_GATE(ck_ker_uart4, &ck_flexgen_08, 0, GATE_UART4);
static STM32_GATE(ck_ker_usart3, &ck_flexgen_09, 0, GATE_USART3);
static STM32_GATE(ck_ker_uart5, &ck_flexgen_09, 0, GATE_UART5);
static STM32_GATE(ck_ker_spi2, &ck_flexgen_10, 0, GATE_SPI2);
static STM32_GATE(ck_ker_spi3, &ck_flexgen_10, 0, GATE_SPI3);
static STM32_GATE(ck_ker_spdifrx, &ck_flexgen_11, 0, GATE_SPDIFRX);
static STM32_GATE(ck_ker_i2c1, &ck_flexgen_12, 0, GATE_I2C1);
static STM32_GATE(ck_ker_i2c2, &ck_flexgen_12, 0, GATE_I2C2);
static STM32_GATE(ck_ker_i3c1, &ck_flexgen_12, 0, GATE_I3C1);
static STM32_GATE(ck_ker_i3c2, &ck_flexgen_12, 0, GATE_I3C2);
static STM32_GATE(ck_ker_i2c3, &ck_flexgen_13, 0, GATE_I2C3);
static STM32_GATE(ck_ker_i2c5, &ck_flexgen_13, 0, GATE_I2C5);
static STM32_GATE(ck_ker_i3c3, &ck_flexgen_13, 0, GATE_I3C3);
static STM32_GATE(ck_ker_i2c4, &ck_flexgen_14, 0, GATE_I2C4);
static STM32_GATE(ck_ker_i2c6, &ck_flexgen_14, 0, GATE_I2C6);
static STM32_GATE(ck_ker_i2c7, &ck_flexgen_15, 0, GATE_I2C7);
static STM32_GATE(ck_ker_spi1, &ck_flexgen_16, 0, GATE_SPI1);
static STM32_GATE(ck_ker_spi4, &ck_flexgen_17, 0, GATE_SPI4);
static STM32_GATE(ck_ker_spi5, &ck_flexgen_17, 0, GATE_SPI5);
static STM32_GATE(ck_ker_spi6, &ck_flexgen_18, 0, GATE_SPI6);
static STM32_GATE(ck_ker_spi7, &ck_flexgen_18, 0, GATE_SPI7);
static STM32_GATE(ck_ker_usart1, &ck_flexgen_19, 0, GATE_USART1);
static STM32_GATE(ck_ker_usart6, &ck_flexgen_20, 0, GATE_USART6);
static STM32_GATE(ck_ker_uart7, &ck_flexgen_21, 0, GATE_UART7);
static STM32_GATE(ck_ker_uart8, &ck_flexgen_21, 0, GATE_UART8);
static STM32_GATE(ck_ker_uart9, &ck_flexgen_22, 0, GATE_UART9);
static STM32_GATE(ck_ker_mdf1, &ck_flexgen_23, 0, GATE_MDF1);
static STM32_GATE(ck_ker_sai1, &ck_flexgen_23, 0, GATE_SAI1);
static STM32_GATE(ck_ker_sai2, &ck_flexgen_24, 0, GATE_SAI2);
static STM32_GATE(ck_ker_sai3, &ck_flexgen_25, 0, GATE_SAI3);
static STM32_GATE(ck_ker_sai4, &ck_flexgen_25, 0, GATE_SAI4);
static STM32_GATE(ck_ker_fdcan, &ck_flexgen_26, 0, GATE_FDCAN);
static STM32_GATE(ck_ker_csi2, &ck_flexgen_29, 0, GATE_CSI);
static STM32_GATE(ck_ker_csi2txesc, &ck_flexgen_30, 0, GATE_CSI);
static STM32_GATE(ck_ker_csi2phy, &ck_flexgen_31, 0, GATE_CSI);
static STM32_GATE(ck_ker_stgen, &ck_flexgen_33, CLK_SET_RATE_PARENT,
		  GATE_STGEN);
static STM32_GATE(ck_ker_usbtc, &ck_flexgen_35, 0, GATE_USBTC);
static STM32_GATE(ck_ker_i3c4, &ck_flexgen_36, 0, GATE_I3C4);
static STM32_GATE(ck_ker_spi8, &ck_flexgen_37, 0, GATE_SPI8);
static STM32_GATE(ck_ker_i2c8, &ck_flexgen_38, 0, GATE_I2C8);
static STM32_GATE(ck_ker_lpuart1, &ck_flexgen_39, 0, GATE_LPUART1);
static STM32_GATE(ck_ker_lptim3, &ck_flexgen_40, 0, GATE_LPTIM3);
static STM32_GATE(ck_ker_lptim4, &ck_flexgen_41, 0, GATE_LPTIM4);
static STM32_GATE(ck_ker_lptim5, &ck_flexgen_41, 0, GATE_LPTIM5);
static STM32_GATE(ck_ker_adf1, &ck_flexgen_42, 0, GATE_ADF1);
static STM32_GATE(ck_ker_sdmmc1, &ck_flexgen_51, 0, GATE_SDMMC1);
static STM32_GATE(ck_ker_sdmmc2, &ck_flexgen_52, 0, GATE_SDMMC2);
static STM32_GATE(ck_ker_sdmmc3, &ck_flexgen_53, 0, GATE_SDMMC3);
static STM32_GATE(ck_ker_eth1, &ck_flexgen_54, 0, GATE_ETH1);
static STM32_GATE(ck_ker_ethsw, &ck_flexgen_54, 0, GATE_ETHSW);
static STM32_GATE(ck_ker_eth2, &ck_flexgen_55, 0, GATE_ETH2);
static STM32_GATE(ck_ker_eth1ptp, &ck_flexgen_56, 0, GATE_ETH1);
static STM32_GATE(ck_ker_eth2ptp, &ck_flexgen_56, 0, GATE_ETH2);
static STM32_GATE(ck_ker_usb2phy2, &ck_flexgen_58, 0, GATE_USB3DRD);
static STM32_GATE(ck_icn_m_gpu, &ck_flexgen_59, 0, GATE_GPU);
// static STM32_GATE(ck_ker_gpu, &ck_pll3, 0, GATE_GPU);
static STM32_GATE(ck_ker_ethswref, &ck_flexgen_60, 0, GATE_ETHSWREF);

static STM32_GATE(ck_ker_eth1stp, &ck_icn_ls_mcu, 0, GATE_ETH1STP);
static STM32_GATE(ck_ker_eth2stp, &ck_icn_ls_mcu, 0, GATE_ETH2STP);

static STM32_GATE(ck_ker_ltdc, &ck_flexgen_27, CLK_SET_RATE_PARENT,
		  GATE_LTDC);

static STM32_COMPOSITE(ck_ker_adc12, 2, PARENTS(&ck_flexgen_46, &ck_icn_ls_mcu),
		       0, GATE_ADC12, NO_DIV, MUX_ADC12);

static STM32_COMPOSITE(ck_ker_adc3, 3, PARENTS(&ck_flexgen_47, &ck_icn_ls_mcu,
		       &ck_flexgen_46),
		       0, GATE_ADC3, NO_DIV, MUX_ADC3);

static STM32_COMPOSITE(ck_ker_usb2phy1, 2, PARENTS(&ck_flexgen_57,
		       &ck_hse_div2),
		       0, GATE_USB2PHY1, NO_DIV, MUX_USB2PHY1);

static STM32_COMPOSITE(ck_ker_usb2phy2_en, 2, PARENTS(&ck_flexgen_58,
		       &ck_hse_div2),
		       0, GATE_USB2PHY2, NO_DIV, MUX_USB2PHY2);

static STM32_COMPOSITE(ck_ker_usb3pciephy, 2, PARENTS(&ck_flexgen_34,
		       &ck_hse_div2),
		       0, GATE_USB3PCIEPHY, NO_DIV, MUX_USB3PCIEPHY);

static STM32_COMPOSITE(ck_ker_dsiblane, 2, PARENTS(&ck_dsi_phy, &ck_flexgen_27),
		       0, GATE_DSI, NO_DIV, MUX_DSIBLANE);

static STM32_COMPOSITE(ck_phy_dsi, 2, PARENTS(&ck_flexgen_28, &ck_hse),
		       0, GATE_DSI, NO_DIV, MUX_DSIPHY);

static STM32_COMPOSITE(ck_ker_lvdsphy, 2, PARENTS(&ck_flexgen_32, &ck_hse),
		       0, GATE_LVDS, NO_DIV, MUX_LVDSPHY);

static STM32_COMPOSITE(ck_ker_dts, 3, PARENTS(&ck_hsi, &ck_hse, &ck_msi),
		       0, GATE_DTS, NO_DIV, MUX_DTS);

static STM32_GATE(ck_ker_eth1mac, &ck_icn_ls_mcu, 0, GATE_ETH1MAC);
static STM32_GATE(ck_ker_eth1tx, &ck_icn_ls_mcu, 0, GATE_ETH1TX);
static STM32_GATE(ck_ker_eth1rx, &ck_icn_ls_mcu, 0, GATE_ETH1RX);
static STM32_GATE(ck_ker_eth2mac, &ck_icn_ls_mcu, 0, GATE_ETH2MAC);
static STM32_GATE(ck_ker_eth2tx, &ck_icn_ls_mcu, 0, GATE_ETH2TX);
static STM32_GATE(ck_ker_eth2rx, &ck_icn_ls_mcu, 0, GATE_ETH2RX);

enum {
	CK_OFF = STM32MP25_LAST_CLK,
	I2SCKIN,
	SPDIFSYMB,
	CK_HSE_RTC,
	DSIPHY,
	CK_OBSER0,
	CK_OBSER1,
	CLK_HSI,
	CLK_HSE,
	CLK_MSI,
	CLK_LSI,
	CLK_LSE,
	STM32MP25_ALL_CLK_NB
};

static struct clk *stm32mp25_clk_provided[STM32MP25_ALL_CLK_NB] = {
	[CLK_HSI]		= &clk_hsi,
	[CLK_HSE]		= &clk_hse,
	[CLK_MSI]		= &clk_msi,
	[CLK_MSI]		= &clk_lsi,
	[CLK_LSE]		= &clk_lse,

	[HSI_CK]		= &ck_hsi,
	[HSE_CK]		= &ck_hse,
	[MSI_CK]		= &ck_msi,
	[LSI_CK]		= &ck_lsi,
	[LSE_CK]		= &ck_lse,
	[HSE_DIV2_CK]		= &ck_hse_div2,

	[PLL2_CK]		= &ck_pll2,
	// [PLL3_CK]		= &ck_pll3,
	[PLL4_CK]		= &ck_pll4,
	[PLL5_CK]		= &ck_pll5,
	[PLL6_CK]		= &ck_pll6,
	[PLL7_CK]		= &ck_pll7,
	[PLL8_CK]		= &ck_pll8,

	[CK_ICN_HS_MCU]		= &ck_icn_hs_mcu,
	[CK_ICN_LS_MCU]		= &ck_icn_ls_mcu,
	[CK_ICN_SDMMC]		= &ck_icn_sdmmc,
	[CK_ICN_DDR]		= &ck_icn_ddr,
	[CK_ICN_DISPLAY]	= &ck_icn_display,
	[CK_ICN_HSL]		= &ck_icn_hsl,
	[CK_ICN_NIC]		= &ck_icn_nic,
	[CK_ICN_VID]		= &ck_icn_vid,
	[CK_FLEXGEN_07]		= &ck_flexgen_07,
	[CK_FLEXGEN_08]		= &ck_flexgen_08,
	[CK_FLEXGEN_09]		= &ck_flexgen_09,
	[CK_FLEXGEN_10]		= &ck_flexgen_10,
	[CK_FLEXGEN_11]		= &ck_flexgen_11,
	[CK_FLEXGEN_12]		= &ck_flexgen_12,
	[CK_FLEXGEN_13]		= &ck_flexgen_13,
	[CK_FLEXGEN_14]		= &ck_flexgen_14,
	[CK_FLEXGEN_15]		= &ck_flexgen_15,
	[CK_FLEXGEN_16]		= &ck_flexgen_16,
	[CK_FLEXGEN_17]		= &ck_flexgen_17,
	[CK_FLEXGEN_18]		= &ck_flexgen_18,
	[CK_FLEXGEN_19]		= &ck_flexgen_19,
	[CK_FLEXGEN_20]		= &ck_flexgen_20,
	[CK_FLEXGEN_21]		= &ck_flexgen_21,
	[CK_FLEXGEN_22]		= &ck_flexgen_22,
	[CK_FLEXGEN_23]		= &ck_flexgen_23,
	[CK_FLEXGEN_24]		= &ck_flexgen_24,
	[CK_FLEXGEN_25]		= &ck_flexgen_25,
	[CK_FLEXGEN_26]		= &ck_flexgen_26,
	[CK_FLEXGEN_27]		= &ck_flexgen_27,
	[CK_FLEXGEN_28]		= &ck_flexgen_28,
	[CK_FLEXGEN_29]		= &ck_flexgen_29,
	[CK_FLEXGEN_30]		= &ck_flexgen_30,
	[CK_FLEXGEN_31]		= &ck_flexgen_31,
	[CK_FLEXGEN_32]		= &ck_flexgen_32,
	[CK_FLEXGEN_33]		= &ck_flexgen_33,
	[CK_FLEXGEN_34]		= &ck_flexgen_34,
	[CK_FLEXGEN_35]		= &ck_flexgen_35,
	[CK_FLEXGEN_36]		= &ck_flexgen_36,
	[CK_FLEXGEN_37]		= &ck_flexgen_37,
	[CK_FLEXGEN_38]		= &ck_flexgen_38,
	[CK_FLEXGEN_39]		= &ck_flexgen_39,
	[CK_FLEXGEN_40]		= &ck_flexgen_40,
	[CK_FLEXGEN_41]		= &ck_flexgen_41,
	[CK_FLEXGEN_42]		= &ck_flexgen_42,
	[CK_FLEXGEN_43]		= &ck_flexgen_43,
	[CK_FLEXGEN_44]		= &ck_flexgen_44,
	[CK_FLEXGEN_45]		= &ck_flexgen_45,
	[CK_FLEXGEN_46]		= &ck_flexgen_46,
	[CK_FLEXGEN_47]		= &ck_flexgen_47,
	[CK_FLEXGEN_48]		= &ck_flexgen_48,
	[CK_FLEXGEN_49]		= &ck_flexgen_49,
	[CK_FLEXGEN_50]		= &ck_flexgen_50,
	[CK_FLEXGEN_51]		= &ck_flexgen_51,
	[CK_FLEXGEN_52]		= &ck_flexgen_52,
	[CK_FLEXGEN_53]		= &ck_flexgen_53,
	[CK_FLEXGEN_54]		= &ck_flexgen_54,
	[CK_FLEXGEN_55]		= &ck_flexgen_55,
	[CK_FLEXGEN_56]		= &ck_flexgen_56,
	[CK_FLEXGEN_57]		= &ck_flexgen_57,
	[CK_FLEXGEN_58]		= &ck_flexgen_58,
	[CK_FLEXGEN_59]		= &ck_flexgen_59,
	[CK_FLEXGEN_60]		= &ck_flexgen_60,
	[CK_FLEXGEN_61]		= &ck_flexgen_61,
	[CK_FLEXGEN_62]		= &ck_flexgen_62,
	[CK_FLEXGEN_63]		= &ck_flexgen_63,

	[CK_ICN_APB1]		= &ck_icn_apb1,
	[CK_ICN_APB2]		= &ck_icn_apb2,
	[CK_ICN_APB3]		= &ck_icn_apb3,
	[CK_ICN_APB4]		= &ck_icn_apb4,
	[CK_ICN_APBDBG]		= &ck_icn_apbdbg,

	[TIMG1_CK]		= &ck_timg1,
	[TIMG2_CK]		= &ck_timg2,

	[CK_BUS_SYSRAM]		= &ck_icn_s_sysram,
	[CK_BUS_VDERAM]		= &ck_icn_s_vderam,
	[CK_BUS_RETRAM]		= &ck_icn_s_retram,
	[CK_BUS_SRAM1]		= &ck_icn_s_sram1,
	[CK_BUS_SRAM2]		= &ck_icn_s_sram2,
	[CK_BUS_OSPI1]		= &ck_icn_s_ospi1,
	[CK_BUS_OSPI2]		= &ck_icn_s_ospi2,
	[CK_BUS_BKPSRAM]	= &ck_icn_s_bkpsram,
	[CK_BUS_DDRPHYC]	= &ck_icn_p_ddrphyc,
	[CK_BUS_SYSCPU1]	= &ck_icn_p_syscpu1,
	[CK_BUS_HPDMA1]		= &ck_icn_p_hpdma1,
	[CK_BUS_HPDMA2]		= &ck_icn_p_hpdma2,
	[CK_BUS_HPDMA3]		= &ck_icn_p_hpdma3,
	[CK_BUS_IPCC1]		= &ck_icn_p_ipcc1,
	[CK_BUS_IPCC2]		= &ck_icn_p_ipcc2,
	[CK_BUS_CCI]		= &ck_icn_p_cci,
	[CK_BUS_CRC]		= &ck_icn_p_crc,
	[CK_BUS_OSPIIOM]	= &ck_icn_p_ospiiom,
	[CK_BUS_HASH]		= &ck_icn_p_hash,
	[CK_BUS_RNG]		= &ck_icn_p_rng,
	[CK_BUS_CRYP1]		= &ck_icn_p_cryp1,
	[CK_BUS_CRYP2]		= &ck_icn_p_cryp2,
	[CK_BUS_SAES]		= &ck_icn_p_saes,
	[CK_BUS_PKA]		= &ck_icn_p_pka,
	[CK_BUS_GPIOA]		= &ck_icn_p_gpioa,
	[CK_BUS_GPIOB]		= &ck_icn_p_gpiob,
	[CK_BUS_GPIOC]		= &ck_icn_p_gpioc,
	[CK_BUS_GPIOD]		= &ck_icn_p_gpiod,
	[CK_BUS_GPIOE]		= &ck_icn_p_gpioe,
	[CK_BUS_GPIOF]		= &ck_icn_p_gpiof,
	[CK_BUS_GPIOG]		= &ck_icn_p_gpiog,
	[CK_BUS_GPIOH]		= &ck_icn_p_gpioh,
	[CK_BUS_GPIOI]		= &ck_icn_p_gpioi,
	[CK_BUS_GPIOJ]		= &ck_icn_p_gpioj,
	[CK_BUS_GPIOK]		= &ck_icn_p_gpiok,
	[CK_BUS_LPSRAM1]	= &ck_icn_s_lpsram1,
	[CK_BUS_LPSRAM2]	= &ck_icn_s_lpsram2,
	[CK_BUS_LPSRAM3]	= &ck_icn_s_lpsram3,
	[CK_BUS_GPIOZ]		= &ck_icn_p_gpioz,
	[CK_BUS_LPDMA]		= &ck_icn_p_lpdma,
	[CK_BUS_ADF1]		= &ck_icn_p_adf1,
	[CK_BUS_HSEM]		= &ck_icn_p_hsem,
	[CK_BUS_RTC]		= &ck_icn_p_rtc,
	[CK_BUS_IWDG5]		= &ck_icn_p_iwdg5,
	[CK_BUS_WWDG2]		= &ck_icn_p_wwdg2,
	[CK_BUS_STM500]		= &ck_icn_s_stm500,
	[CK_BUS_FMC]		= &ck_icn_p_fmc,
	[CK_BUS_ETH1]		= &ck_icn_p_eth1,
	[CK_BUS_ETHSW]		= &ck_icn_p_ethsw,
	[CK_BUS_ETH2]		= &ck_icn_p_eth2,
	[CK_BUS_PCIE]		= &ck_icn_p_pcie,
	[CK_BUS_ADC12]		= &ck_icn_p_adc12,
	[CK_BUS_ADC3]		= &ck_icn_p_adc3,
	[CK_BUS_MDF1]		= &ck_icn_p_mdf1,
	[CK_BUS_SPI8]		= &ck_icn_p_spi8,
	[CK_BUS_LPUART1]	= &ck_icn_p_lpuart1,
	[CK_BUS_I2C8]		= &ck_icn_p_i2c8,
	[CK_BUS_LPTIM3]		= &ck_icn_p_lptim3,
	[CK_BUS_LPTIM4]		= &ck_icn_p_lptim4,
	[CK_BUS_LPTIM5]		= &ck_icn_p_lptim5,
	[CK_BUS_RISAF4]		= &ck_icn_p_risaf4,
	[CK_BUS_SDMMC1]		= &ck_icn_m_sdmmc1,
	[CK_BUS_SDMMC2]		= &ck_icn_m_sdmmc2,
	[CK_BUS_SDMMC3]		= &ck_icn_m_sdmmc3,
	[CK_BUS_DDR]		= &ck_icn_s_ddr,
	[CK_BUS_USB2OHCI]	= &ck_icn_m_usb2ohci,
	[CK_BUS_USB2EHCI]	= &ck_icn_m_usb2ehci,
	[CK_BUS_USB3DRD]	= &ck_icn_m_usb3drd,
	[CK_BUS_TIM2]		= &ck_icn_p_tim2,
	[CK_BUS_TIM3]		= &ck_icn_p_tim3,
	[CK_BUS_TIM4]		= &ck_icn_p_tim4,
	[CK_BUS_TIM5]		= &ck_icn_p_tim5,
	[CK_BUS_TIM6]		= &ck_icn_p_tim6,
	[CK_BUS_TIM7]		= &ck_icn_p_tim7,
	[CK_BUS_TIM10]		= &ck_icn_p_tim10,
	[CK_BUS_TIM11]		= &ck_icn_p_tim11,
	[CK_BUS_TIM12]		= &ck_icn_p_tim12,
	[CK_BUS_TIM13]		= &ck_icn_p_tim13,
	[CK_BUS_TIM14]		= &ck_icn_p_tim14,
	[CK_BUS_LPTIM1]		= &ck_icn_p_lptim1,
	[CK_BUS_LPTIM2]		= &ck_icn_p_lptim2,
	[CK_BUS_SPI2]		= &ck_icn_p_spi2,
	[CK_BUS_SPI3]		= &ck_icn_p_spi3,
	[CK_BUS_SPDIFRX]	= &ck_icn_p_spdifrx,
	[CK_BUS_USART2]		= &ck_icn_p_usart2,
	[CK_BUS_USART3]		= &ck_icn_p_usart3,
	[CK_BUS_UART4]		= &ck_icn_p_uart4,
	[CK_BUS_UART5]		= &ck_icn_p_uart5,
	[CK_BUS_I2C1]		= &ck_icn_p_i2c1,
	[CK_BUS_I2C2]		= &ck_icn_p_i2c2,
	[CK_BUS_I2C3]		= &ck_icn_p_i2c3,
	[CK_BUS_I2C4]		= &ck_icn_p_i2c4,
	[CK_BUS_I2C5]		= &ck_icn_p_i2c5,
	[CK_BUS_I2C6]		= &ck_icn_p_i2c6,
	[CK_BUS_I2C7]		= &ck_icn_p_i2c7,
	[CK_BUS_I3C1]		= &ck_icn_p_i3c1,
	[CK_BUS_I3C2]		= &ck_icn_p_i3c2,
	[CK_BUS_I3C3]		= &ck_icn_p_i3c3,
	[CK_BUS_I3C4]		= &ck_icn_p_i3c4,
	[CK_BUS_TIM1]		= &ck_icn_p_tim1,
	[CK_BUS_TIM8]		= &ck_icn_p_tim8,
	[CK_BUS_TIM15]		= &ck_icn_p_tim15,
	[CK_BUS_TIM16]		= &ck_icn_p_tim16,
	[CK_BUS_TIM17]		= &ck_icn_p_tim17,
	[CK_BUS_TIM20]		= &ck_icn_p_tim20,
	[CK_BUS_SAI1]		= &ck_icn_p_sai1,
	[CK_BUS_SAI2]		= &ck_icn_p_sai2,
	[CK_BUS_SAI3]		= &ck_icn_p_sai3,
	[CK_BUS_SAI4]		= &ck_icn_p_sai4,
	[CK_BUS_USART1]		= &ck_icn_p_usart1,
	[CK_BUS_USART6]		= &ck_icn_p_usart6,
	[CK_BUS_UART7]		= &ck_icn_p_uart7,
	[CK_BUS_UART8]		= &ck_icn_p_uart8,
	[CK_BUS_UART9]		= &ck_icn_p_uart9,
	[CK_BUS_FDCAN]		= &ck_icn_p_fdcan,
	[CK_BUS_SPI1]		= &ck_icn_p_spi1,
	[CK_BUS_SPI4]		= &ck_icn_p_spi4,
	[CK_BUS_SPI5]		= &ck_icn_p_spi5,
	[CK_BUS_SPI6]		= &ck_icn_p_spi6,
	[CK_BUS_SPI7]		= &ck_icn_p_spi7,
	[CK_BUS_BSEC]		= &ck_icn_p_bsec,
	[CK_BUS_IWDG1]		= &ck_icn_p_iwdg1,
	[CK_BUS_IWDG2]		= &ck_icn_p_iwdg2,
	[CK_BUS_IWDG3]		= &ck_icn_p_iwdg3,
	[CK_BUS_IWDG4]		= &ck_icn_p_iwdg4,
	[CK_BUS_WWDG1]		= &ck_icn_p_wwdg1,
	[CK_BUS_VREF]		= &ck_icn_p_vref,
	[CK_BUS_SERC]		= &ck_icn_p_serc,
	[CK_BUS_DTS]		= &ck_icn_p_dts,
	[CK_BUS_HDP]		= &ck_icn_p_hdp,
	[CK_BUS_IS2M]		= &ck_icn_p_is2m,
	[CK_BUS_DSI]		= &ck_icn_p_dsi,
	[CK_BUS_LTDC]		= &ck_icn_p_ltdc,
	[CK_BUS_CSI]		= &ck_icn_p_csi2,
	[CK_BUS_DCMIPP]		= &ck_icn_p_dcmipp,
	[CK_BUS_DDRC]		= &ck_icn_p_ddrc,
	[CK_BUS_DDRCFG]		= &ck_icn_p_ddrcfg,
	[CK_BUS_LVDS]		= &ck_icn_p_lvds,
	[CK_BUS_GICV2M]		= &ck_icn_p_gicv2m,
	[CK_BUS_USBTC]		= &ck_icn_p_usbtc,
	[CK_BUS_BUSPERFM]	= &ck_icn_p_busperfm,
	[CK_BUS_USB3PCIEPHY]	= &ck_icn_p_usb3pciephy,
	[CK_BUS_STGEN]		= &ck_icn_p_stgen,
	[CK_BUS_VDEC]		= &ck_icn_p_vdec,
	[CK_BUS_VENC]		= &ck_icn_p_venc,
	[CK_SYSDBG]		= &ck_sys_dbg,
	[CK_KER_TIM2]		= &ck_ker_tim2,
	[CK_KER_TIM3]		= &ck_ker_tim3,
	[CK_KER_TIM4]		= &ck_ker_tim4,
	[CK_KER_TIM5]		= &ck_ker_tim5,
	[CK_KER_TIM6]		= &ck_ker_tim6,
	[CK_KER_TIM7]		= &ck_ker_tim7,
	[CK_KER_TIM10]		= &ck_ker_tim10,
	[CK_KER_TIM11]		= &ck_ker_tim11,
	[CK_KER_TIM12]		= &ck_ker_tim12,
	[CK_KER_TIM13]		= &ck_ker_tim13,
	[CK_KER_TIM14]		= &ck_ker_tim14,
	[CK_KER_TIM1]		= &ck_ker_tim1,
	[CK_KER_TIM8]		= &ck_ker_tim8,
	[CK_KER_TIM15]		= &ck_ker_tim15,
	[CK_KER_TIM16]		= &ck_ker_tim16,
	[CK_KER_TIM17]		= &ck_ker_tim17,
	[CK_KER_TIM20]		= &ck_ker_tim20,
	[CK_KER_LPTIM1]		= &ck_ker_lptim1,
	[CK_KER_LPTIM2]		= &ck_ker_lptim2,
	[CK_KER_USART2]		= &ck_ker_usart2,
	[CK_KER_UART4]		= &ck_ker_uart4,
	[CK_KER_USART3]		= &ck_ker_usart3,
	[CK_KER_UART5]		= &ck_ker_uart5,
	[CK_KER_SPI2]		= &ck_ker_spi2,
	[CK_KER_SPI3]		= &ck_ker_spi3,
	[CK_KER_SPDIFRX]	= &ck_ker_spdifrx,
	[CK_KER_I2C1]		= &ck_ker_i2c1,
	[CK_KER_I2C2]		= &ck_ker_i2c2,
	[CK_KER_I3C1]		= &ck_ker_i3c1,
	[CK_KER_I3C2]		= &ck_ker_i3c2,
	[CK_KER_I2C3]		= &ck_ker_i2c3,
	[CK_KER_I2C5]		= &ck_ker_i2c5,
	[CK_KER_I3C3]		= &ck_ker_i3c3,
	[CK_KER_I2C4]		= &ck_ker_i2c4,
	[CK_KER_I2C6]		= &ck_ker_i2c6,
	[CK_KER_I2C7]		= &ck_ker_i2c7,
	[CK_KER_SPI1]		= &ck_ker_spi1,
	[CK_KER_SPI4]		= &ck_ker_spi4,
	[CK_KER_SPI5]		= &ck_ker_spi5,
	[CK_KER_SPI6]		= &ck_ker_spi6,
	[CK_KER_SPI7]		= &ck_ker_spi7,
	[CK_KER_USART1]		= &ck_ker_usart1,
	[CK_KER_USART6]		= &ck_ker_usart6,
	[CK_KER_UART7]		= &ck_ker_uart7,
	[CK_KER_UART8]		= &ck_ker_uart8,
	[CK_KER_UART9]		= &ck_ker_uart9,
	[CK_KER_MDF1]		= &ck_ker_mdf1,
	[CK_KER_SAI1]		= &ck_ker_sai1,
	[CK_KER_SAI2]		= &ck_ker_sai2,
	[CK_KER_SAI3]		= &ck_ker_sai3,
	[CK_KER_SAI4]		= &ck_ker_sai4,
	[CK_KER_FDCAN]		= &ck_ker_fdcan,
	[CK_KER_CSI]		= &ck_ker_csi2,
	[CK_KER_CSITXESC]	= &ck_ker_csi2txesc,
	[CK_KER_CSIPHY]		= &ck_ker_csi2phy,
	[CK_KER_STGEN]		= &ck_ker_stgen,
	[CK_KER_USBTC]		= &ck_ker_usbtc,
	[CK_KER_I3C4]		= &ck_ker_i3c4,
	[CK_KER_SPI8]		= &ck_ker_spi8,
	[CK_KER_I2C8]		= &ck_ker_i2c8,
	[CK_KER_LPUART1]	= &ck_ker_lpuart1,
	[CK_KER_LPTIM3]		= &ck_ker_lptim3,
	[CK_KER_LPTIM4]		= &ck_ker_lptim4,
	[CK_KER_LPTIM5]		= &ck_ker_lptim5,
	[CK_KER_ADF1]		= &ck_ker_adf1,
	[CK_KER_TSDBG]		= &ck_ker_tsdbg,
	[CK_KER_TPIU]		= &ck_ker_tpiu,
	[CK_BUS_ETR]		= &ck_icn_m_etr,
	[CK_BUS_SYSATB]		= &ck_sys_atb,
	[CK_KER_OSPI1]		= &ck_ker_ospi1,
	[CK_KER_OSPI2]		= &ck_ker_ospi2,
	[CK_KER_FMC]		= &ck_ker_fmc,
	[CK_KER_SDMMC1]		= &ck_ker_sdmmc1,
	[CK_KER_SDMMC2]		= &ck_ker_sdmmc2,
	[CK_KER_SDMMC3]		= &ck_ker_sdmmc3,
	[CK_KER_ETH1]		= &ck_ker_eth1,
	[CK_ETH1_STP]		= &ck_ker_eth1stp,
	[CK_KER_ETHSW]		= &ck_ker_ethsw,
	[CK_KER_ETH2]		= &ck_ker_eth2,
	[CK_ETH2_STP]		= &ck_ker_eth2stp,
	[CK_KER_ETH1PTP]	= &ck_ker_eth1ptp,
	[CK_KER_ETH2PTP]	= &ck_ker_eth2ptp,
	[CK_BUS_GPU]		= &ck_icn_m_gpu,
	// [CK_KER_GPU]		= &ck_ker_gpu,
	[CK_KER_ETHSWREF]	= &ck_ker_ethswref,

	[CK_MCO1]		= &ck_mco1,
	[CK_MCO2]		= &ck_mco2,
	[CK_KER_ADC12]		= &ck_ker_adc12,
	[CK_KER_ADC3]		= &ck_ker_adc3,
	[CK_KER_USB2PHY1]	= &ck_ker_usb2phy1,
	[CK_KER_USB2PHY2]	= &ck_ker_usb2phy2,
	[CK_KER_USB2PHY2EN]	= &ck_ker_usb2phy2_en,
	[CK_KER_USB3PCIEPHY]	= &ck_ker_usb3pciephy,
	[CK_KER_LTDC]		= &ck_ker_ltdc,
	[CK_KER_DSIBLANE]	= &ck_ker_dsiblane,
	[CK_KER_DSIPHY]		= &ck_phy_dsi,
	[CK_KER_LVDSPHY]	= &ck_ker_lvdsphy,
	[CK_KER_DTS]		= &ck_ker_dts,
	[RTC_CK]		= &ck_rtc,

	[CK_ETH1_MAC]		= &ck_ker_eth1mac,
	[CK_ETH1_TX]		= &ck_ker_eth1tx,
	[CK_ETH1_RX]		= &ck_ker_eth1rx,
	[CK_ETH2_MAC]		= &ck_ker_eth2mac,
	[CK_ETH2_TX]		= &ck_ker_eth2tx,
	[CK_ETH2_RX]		= &ck_ker_eth2rx,

	[CK_HSE_RTC]		= &ck_hse_rtc,
	[CK_OBSER0]		= &ck_obser0,
	[CK_OBSER1]		= &ck_obser1,
	[CK_OFF]		= &ck_off,
	[I2SCKIN]		= &i2sckin,
	[SPDIFSYMB]		= &spdifsymb,
	[DSIPHY]		= &ck_dsi_phy,
};

static void clk_stm32_set_flexgen_as_critical(struct clk_stm32_priv *priv)
{
	uint32_t i;

	for (i = 0; i < XBAR_CHANNEL_NB; i++) {
		unsigned long clock_id = CK_ICN_HS_MCU + i;
		struct clk *clk;

		if (!stm32_rcc_has_access_by_id(priv, i))
			continue;

		clk = stm32mp_rcc_clock_id_to_clk(priv, clock_id);
		assert(clk);

		clk_enable(clk);
	}
}

static bool clk_stm32_clock_is_critical(__maybe_unused struct clk *clk)
{
	struct clk *clk_criticals[] = {
		/* Fix me: activate temporay all clock required */
		&ck_hsi
		, &ck_hse
		, &ck_msi /*  critical in TF-A */
		, &ck_lsi
		, &ck_lse
		, &ck_icn_p_gpiog /*  uart */
		, &ck_icn_p_bsec /*  bsec  */
		, &ck_icn_p_gpiod /*  octospi */
		, &ck_icn_ddr /* switch on in TF-A before calling ddr init */
		, &ck_icn_s_sram2 /* sram2 is used to retrieve ddr binary */

#if defined(STM32_SEC)
		, &ck_icn_p_risaf4
		, &ck_icn_p_bsec
#endif
	};
	size_t i = 0;

	for (i = 0; i < ARRAY_SIZE(clk_criticals); i++) {
		struct clk *clk_critical = clk_criticals[i];

		if (clk == clk_critical)
			return true;
	}

	return false;
}

static uint8_t refcounts_mp25[GATE_NB];

static struct clk_stm32_priv stm32mp25_clock_data = {
	.muxes			= parent_mp25,
	.nb_muxes		= ARRAY_SIZE(parent_mp25),
	.gates			= gates_mp25,
	.nb_gates		= ARRAY_SIZE(gates_mp25),
	.div			= dividers_mp25,
	.nb_div			= ARRAY_SIZE(dividers_mp25),
	.nb_clk_refs		= STM32MP25_ALL_CLK_NB,
	.clk_refs		= stm32mp25_clk_provided,
	.is_critical		= clk_stm32_clock_is_critical,
	.gate_cpt		= refcounts_mp25,

};

static void clk_stm32_init_oscillators(struct clk_stm32_priv *priv)
{
	const int osc_tab[] = {OSC_HSE, OSC_HSI, OSC_LSE, OSC_LSI, OSC_MSI};
	struct clk *clks[] = {&clk_hse, &clk_hsi, &clk_lse, &clk_lsi, &clk_msi};
	const struct stm32_rcc_config *rcc_cfg = priv->rcc_cfg;
	size_t i = 0;

	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		struct clk_fixed_rate_cfg *cfg = clks[i]->priv;

		cfg->rate = rcc_cfg->osci[osc_tab[i]].freq;
	}
}

static int stm32mp25_clk_dt_init(const struct device *dev)
{
	struct clk_stm32_priv *priv = (struct clk_stm32_priv *)dev_get_data(dev);
	int err;

	priv->rcc_cfg = dev_get_config(dev);

	clk_stm32_init_oscillators(priv);

	err = stm32mp2_init_clock_tree(priv);
	if (err != 0)
		return err;

	clk_stm32_register_clocks(priv);

	/* todo after rif application */
	clk_stm32_set_flexgen_as_critical(priv);

#if defined(STM32_BL2)
	mmio_setbits_32(clk_stm32_get_rcc_base(priv) + RCC_DDRITFCFGR, RCC_DDRITFCFGR_DDRSHR);

	/* Enable DDR clock and remove reset */
	mmio_write_32(clk_stm32_get_rcc_base(priv) + RCC_DDRCPCFGR,
		      RCC_DDRCPCFGR_DDRCPEN | RCC_DDRCPCFGR_DDRCPLPEN);
#endif

	return 0;
}

static const uint32_t stm32mp25_bclk[] = DT_PROP_OR(DT_DRV_INST(0), st_busclk, {});
static const uint32_t stm32mp25_kclk[] = DT_PROP_OR(DT_DRV_INST(0), st_kerclk, {});
static const uint32_t stm32mp25_fclk[] = DT_PROP_OR(DT_DRV_INST(0), st_flexgen, {});

static const struct stm32_osci_dt_cfg stm32_osci[] = {
	DT_RCC_CLOCK_OSCI(DT_DRV_INST(0), OSC_LSI, clk_lsi),
	DT_RCC_CLOCK_OSCI(DT_DRV_INST(0), OSC_LSE, clk_lse),
	DT_RCC_CLOCK_OSCI(DT_DRV_INST(0), OSC_HSI, clk_hsi),
	DT_RCC_CLOCK_OSCI(DT_DRV_INST(0), OSC_HSE, clk_hse),
	DT_RCC_CLOCK_OSCI(DT_DRV_INST(0), OSC_MSI, clk_msi),
};

static const struct stm32_pll_dt_cfg stm32_pll[] = {
};

const struct stm32_rcc_config stm32mp25_rcc_cfg = {
	.base = DT_INST_REG_ADDR(0),
	.busclk = stm32mp25_bclk,
	.nbusclk = ARRAY_SIZE(stm32mp25_bclk),
	.kernelclk = stm32mp25_kclk,
	.nkernelclk = ARRAY_SIZE(stm32mp25_kclk),
	.flexgen = stm32mp25_fclk,
	.nflexgen = ARRAY_SIZE(stm32mp25_fclk),
	.osci = stm32_osci,
	.nosci = ARRAY_SIZE(stm32_osci),
	.pll = stm32_pll,
	.npll = ARRAY_SIZE(stm32_pll),
};

static struct clk_controller_api stm32mp25_clk_api = {
	.get = stm32_clk_get,
};

DEVICE_DT_INST_DEFINE(0,
		      &stm32mp25_clk_dt_init,
		      &stm32mp25_clock_data,
		      &stm32mp25_rcc_cfg,
		      PRE_CORE, 5,
		      &stm32mp25_clk_api);
