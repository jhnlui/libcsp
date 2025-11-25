/****************************************************************************
 * **File:** csp/drivers/i2c_linux.h
 *
 * **Description:** Linux I2C driver binding the CSP I2C interface.
 ****************************************************************************/
#pragma once

#include <csp/interfaces/csp_if_i2c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open an I2C device and add a CSP interface.
 *
 * Parameters:
 * @param[in] device I2C device path (for example "/dev/i2c-1").
 * @param[in] ifname CSP interface name, use #CSP_IF_I2C_DEFAULT_NAME for default name.
 * @param[in] node_id CSP address of the interface.
 * @param[in] i2c_address I2C 7-bit address used when receiving frames on this interface.
 * @param[out] return_iface the added interface.
 * @return #CSP_ERR_NONE on success, otherwise an error code.
 */
int csp_i2c_linux_open_and_add_interface(const char * device, const char * ifname, uint8_t node_id, uint8_t i2c_address, csp_iface_t ** return_iface);

/**
 * Convenience wrapper for csp_i2c_linux_open_and_add_interface().
 *
 * Parameters:
 * @param[in] device I2C device path (for example "/dev/i2c-1").
 * @param[in] node_id CSP address of the interface.
 * @param[in] i2c_address I2C 7-bit address used when receiving frames on this interface.
 * @return The added interface, or NULL in case of failure.
 */
csp_iface_t * csp_i2c_linux_init(const char * device, uint8_t node_id, uint8_t i2c_address);

/**
 * Stop the RX thread and free resources (testing helper).
 *
 * .. note:: This will invalidate CSP, because an interface can't be removed.
 *                       This is primarily for testing.
 *
 * Parameters:
 * @param[in] iface interface to stop.
 * @return #CSP_ERR_NONE on success, otherwise an error code.
 */
int csp_i2c_linux_stop(csp_iface_t * iface);

#ifdef __cplusplus
}
#endif

