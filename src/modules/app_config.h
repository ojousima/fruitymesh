#pragma once

/**
 * @defgroup Configuration Application configuration
 */

/** @{ */
/**
 * @defgroup app_config Application configuration
 * @brief Configure application enabled modules and parameters.
 */
/** @ }*/
/**
 * @addtogroup Fruity
 */
/** @{ */
/**
 * @file app_config.h
 * @author Otso Jousimaa <otso@ojousima.net>
 * @date 2020-06-25
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 *
 */

#define RUUVI_FRUITY_ENABLED (1U)

#ifndef APP_FW_NAME
#   define APP_FW_NAME "Fruity Ruuvi"
#endif

/** @brief Logs reserve lot of flash, enable only on debug builds */
#ifndef RI_LOG_ENABLED
#   define RI_LOG_ENABLED (1U)
#   define APP_LOG_LEVEL RI_LOG_LEVEL_DEBUG
#endif

/*@}*/
