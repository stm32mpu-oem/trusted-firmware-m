/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * Copyright (C) 2023 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fork of zephyr
 */

/**
 * @file
 * Public APIs for pin control drivers
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_PINCTRL_H_
#define ZEPHYR_INCLUDE_DRIVERS_PINCTRL_H_

/**
 * @brief Pin Controller Interface
 * @defgroup pinctrl_interface Pin Controller Interface
 * @ingroup io_interfaces
 * @{
 */

#include <errno.h>
#include <lib/utils_def.h>

#include <device.h>
#include <devicetree.h>
#include <devicetree/pinctrl.h>
#include <pinctrl_soc.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Pin control states
 * @anchor PINCTRL_STATES
 * @{
 */

/** Default state (state used when the device is in operational state). */
#define PINCTRL_STATE_DEFAULT 0U
/** Sleep state (state used when the device is in low power mode). */
#define PINCTRL_STATE_SLEEP 1U

/** This and higher values refer to custom private states. */
#define PINCTRL_STATE_PRIV_START 2U

/** @} */

/** Pin control state configuration. */
struct pinctrl_state {
	/** Pin configurations. */
	const pinctrl_soc_pin_t *pins;
	/** Number of pin configurations. */
	uint8_t pin_cnt;
	/** State identifier (see @ref PINCTRL_STATES). */
	uint8_t id;
};

/** Pin controller configuration for a given device. */
struct pinctrl_dev_config {
	/** List of state configurations. */
	const struct pinctrl_state *states;
	/** Number of state configurations. */
	uint8_t state_cnt;
};

/** @cond INTERNAL_HIDDEN */

#ifndef CONFIG_PM_DEVICE
/** If device power management is not enabled, "sleep" state will be ignored. */
#define PINCTRL_SKIP_SLEEP 1
#endif

/**
 * @brief Obtain the state identifier for the given node and state index.
 *
 * @param state_idx State index.
 * @param node_id Node identifier.
 */
#define Z_PINCTRL_STATE_ID(state_idx, node_id)				       \
	_CONCAT(PINCTRL_STATE_,						       \
		DT_PINCTRL_IDX_TO_NAME_UPPER_TOKEN(node_id, state_idx))

/**
 * @brief Obtain the variable name storing pinctrl config for the given DT node
 * identifier.
 *
 * @param node_id Node identifier.
 */
#define Z_PINCTRL_DEV_CONFIG_NAME(node_id) \
	_CONCAT(__pinctrl_dev_config, DEVICE_DT_NAME_GET(node_id))

/**
 * @brief Obtain the variable name storing pinctrl states for the given DT node
 * identifier.
 *
 * @param node_id Node identifier.
 */
#define Z_PINCTRL_STATES_NAME(node_id) \
	_CONCAT(__pinctrl_states, DEVICE_DT_NAME_GET(node_id))

/**
 * @brief Obtain the variable name storing pinctrl pins for the given DT node
 * identifier and state index.
 *
 * @param state_idx State index.
 * @param node_id Node identifier.
 */
#define Z_PINCTRL_STATE_PINS_NAME(state_idx, node_id) \
	_CONCAT(__pinctrl_state_pins_ ## state_idx, DEVICE_DT_NAME_GET(node_id))

/**
 * @brief Utility macro to check if given state has to be skipped.
 *
 * If a certain state has to be skipped, a macro named PINCTRL_SKIP_<STATE>
 * can be defined evaluating to 1. This can be useful, for example, to
 * automatically ignore the sleep state if no device power management is
 * enabled.
 *
 * @param state_idx State index.
 * @param node_id Node identifier.
 */
#define Z_PINCTRL_SKIP_STATE(state_idx, node_id)			       \
	_CONCAT(PINCTRL_SKIP_,						       \
		DT_PINCTRL_IDX_TO_NAME_UPPER_TOKEN(node_id, state_idx))

/**
 * @brief Helper macro to define pins for a given pin control state.
 *
 * @param state_idx State index.
 * @param node_id Node identifier.
 */
#define Z_PINCTRL_STATE_PINS_DEFINE(state_idx, node_id)			       \
        COND_CODE_1(Z_PINCTRL_SKIP_STATE(state_idx, node_id), (),	       \
	(static const pinctrl_soc_pin_t					       \
	Z_PINCTRL_STATE_PINS_NAME(state_idx, node_id)[] =		       \
	Z_PINCTRL_STATE_PINS_INIT(node_id, pinctrl_ ## state_idx)))

/**
 * @brief Helper macro to initialize a pin control state.
 *
 * @param state_idx State index.
 * @param node_id Node identifier.
 */
#define Z_PINCTRL_STATE_INIT(state_idx, node_id)			       \
	COND_CODE_1(Z_PINCTRL_SKIP_STATE(state_idx, node_id), (),	       \
	({								       \
		.id = Z_PINCTRL_STATE_ID(state_idx, node_id),		       \
		.pins = Z_PINCTRL_STATE_PINS_NAME(state_idx, node_id),	       \
		.pin_cnt = ARRAY_SIZE(Z_PINCTRL_STATE_PINS_NAME(state_idx,     \
								node_id))      \
	}))

/**
 * @brief Define all the states for the given node identifier.
 *
 * @param node_id Node identifier.
 */
#define Z_PINCTRL_STATES_DEFINE(node_id)				       \
	static const struct pinctrl_state				       \
	Z_PINCTRL_STATES_NAME(node_id)[] = {				       \
		LISTIFY(DT_NUM_PINCTRL_STATES(node_id),			       \
			     Z_PINCTRL_STATE_INIT, (,), node_id)	       \
	};

/**
 * @brief Helper macro to initialize pin control config.
 *
 * @param node_id Node identifier.
 */

#define Z_PINCTRL_DEV_CONFIG_INIT(node_id)				       \
	{								       \
		.states = Z_PINCTRL_STATES_NAME(node_id),		       \
		.state_cnt = ARRAY_SIZE(Z_PINCTRL_STATES_NAME(node_id)),       \
	}

#define Z_PINCTRL_DEV_CONFIG_STATIC static

#define Z_PINCTRL_DEV_CONFIG_CONST const

/** @endcond */

/**
 * @brief Define all pin control information for the given node identifier.
 *
 * This helper macro should be called together with device definition. It
 * defines and initializes the pin control configuration for the device
 * represented by node_id. Each pin control state (pinctrl-0, ..., pinctrl-N) is
 * also defined and initialized. Note that states marked to be skipped will not
 * be defined (refer to Z_PINCTRL_SKIP_STATE for more details).
 *
 * @param node_id Node identifier.
 */
#define PINCTRL_DT_DEFINE(node_id)					       \
	LISTIFY(DT_NUM_PINCTRL_STATES(node_id),				       \
		     Z_PINCTRL_STATE_PINS_DEFINE, (;), node_id);	       \
	Z_PINCTRL_STATES_DEFINE(node_id)				       \
	Z_PINCTRL_DEV_CONFIG_STATIC Z_PINCTRL_DEV_CONFIG_CONST		       \
	struct pinctrl_dev_config Z_PINCTRL_DEV_CONFIG_NAME(node_id) =	       \
	Z_PINCTRL_DEV_CONFIG_INIT(node_id)

/**
 * @brief Define all pin control information for the given compatible index.
 *
 * @param inst Instance number.
 *
 * @see #PINCTRL_DT_DEFINE
 */
#define PINCTRL_DT_INST_DEFINE(inst) PINCTRL_DT_DEFINE(DT_DRV_INST(inst))

/**
 * @brief Obtain a reference to the pin control configuration given a node
 * identifier.
 *
 * @param node_id Node identifier.
 */
#define PINCTRL_DT_DEV_CONFIG_GET(node_id) &Z_PINCTRL_DEV_CONFIG_NAME(node_id)

/**
 * @brief Obtain a reference to the pin control configuration given current
 * compatible instance number.
 *
 * @param inst Instance number.
 *
 * @see #PINCTRL_DT_DEV_CONFIG_GET
 */
#define PINCTRL_DT_INST_DEV_CONFIG_GET(inst) \
	PINCTRL_DT_DEV_CONFIG_GET(DT_DRV_INST(inst))

/**
 * @brief Find the state configuration for the given state id.
 *
 * @param config Pin controller configuration.
 * @param id Pin controller state id (see @ref PINCTRL_STATES).
 * @param state Found state.
 *
 * @retval 0 If state has been found.
 * @retval -ENOENT If the state has not been found.
 */
int pinctrl_lookup_state(const struct pinctrl_dev_config *config, uint8_t id,
			 const struct pinctrl_state **state);

/**
 * @brief Configure a set of pins.
 *
 * This function will configure the necessary hardware blocks to make the
 * configuration immediately effective.
 *
 * @warning This function must never be used to configure pins used by an
 * instantiated device driver.
 *
 * @param pins List of pins to be configured.
 * @param pin_cnt Number of pins.
 *
 * @retval 0 If succeeded
 * @retval -errno Negative errno for other failures.
 */
int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt);

/**
 * @brief Apply a state directly from the provided state configuration.
 *
 * @param config Pin control configuration.
 * @param state State.
 *
 * @retval 0 If succeeded
 * @retval -errno Negative errno for other failures.
 */
static inline int pinctrl_apply_state_direct(
	const struct pinctrl_state *state)
{
	return pinctrl_configure_pins(state->pins, state->pin_cnt);
}

/**
 * @brief Apply a state from the given device configuration.
 *
 * @param config Pin control configuration.
 * @param id Id of the state to be applied (see @ref PINCTRL_STATES).
 *
 * @retval 0 If succeeded.
 * @retval -ENOENT If given state id does not exist.
 * @retval -errno Negative errno for other failures.
 */
static inline int pinctrl_apply_state(const struct pinctrl_dev_config *config,
				      uint8_t id)
{
	int ret;
	const struct pinctrl_state *state;

	ret = pinctrl_lookup_state(config, id, &state);
	if (ret < 0) {
		return ret;
	}

	return pinctrl_apply_state_direct(state);
}

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* ZEPHYR_INCLUDE_DRIVERS_PINCTRL_H_ */
