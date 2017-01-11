/*
 * Lua RTOS, I2C driver
 *
 * Copyright (C) 2015 - 2016
 * IBEROXARXA SERVICIOS INTEGRALES, S.L. & CSS IBÉRICA, S.L.
 * 
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 * 
 * All rights reserved.  
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

#include "luartos.h"

#if USE_I2C

#include "freertos/FreeRTOS.h"
#include "driver/i2c.h"

#include <stdint.h>

#include <sys/list.h>
#include <sys/mutex.h>
#include <sys/driver.h>
#include <sys/syslog.h>

#include <drivers/gpio.h>
#include <drivers/cpu.h>
#include <drivers/i2c.h>

// Driver locks
driver_unit_lock_t i2c_locks[CPU_LAST_I2C + 1];

// Driver message errors
const char *i2c_errors[] = {
	"",
	"can't setup",
	"is not setup",
	"invalid unit",
	"invalid operation",
	"not enough memory",
	"invalid transaction",
	"not ack received",
	"timeout"
};

// i2c info needed by driver
static i2c_t i2c[CPU_LAST_I2C + 1] = {
	{0,0, MUTEX_INITIALIZER},
	{0,0, MUTEX_INITIALIZER},
};

// Transaction list
static struct list transactions;

/*
 * Helper functions
 */
static driver_error_t *i2c_lock_resources(int unit, i2c_resources_t *resources) {
	driver_unit_lock_error_t *lock_error = NULL;

	// Lock sda
	if ((lock_error = driver_lock(I2C_DRIVER, unit, GPIO_DRIVER, resources->sda))) {
    	return driver_lock_error(I2C_DRIVER, lock_error);
    }

	// Lock scl
	if ((lock_error = driver_lock(I2C_DRIVER, unit, GPIO_DRIVER, resources->scl))) {
    	return driver_lock_error(I2C_DRIVER, lock_error);
    }

	return NULL;
}

static driver_error_t *i2c_check(int unit) {
    // Sanity checks
	if (!((1 << unit) & CPU_I2C_ALL)) {
		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_UNIT, NULL);
	}

	if (!i2c[unit].setup) {
		return driver_operation_error(I2C_DRIVER, I2C_ERR_IS_NOT_SETUP, NULL);
	}

	return NULL;
}

/*
 * Operation functions
 */

void i2c_init() {
	int i;

	// Init transaction list
    list_init(&transactions, 0);

    // Init mutexes
    for(i=0;i < CPU_LAST_I2C;i++) {
        mtx_init(&i2c[i].mtx, NULL, NULL, 0);
    }
}

driver_error_t *i2c_setup(int unit, int mode, int speed, int sda, int scl, int addr10_en, int addr) {
	driver_error_t *error;

    // Sanity checks
	if (!((1 << unit) & CPU_I2C_ALL)) {
		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_UNIT, NULL);
	}

	mtx_lock(&i2c[unit].mtx);

	// If unit is setup, remove first
	if (i2c[unit].setup) {
		i2c_driver_delete(unit);

		i2c[unit].setup = 0;
	}

    // Lock resources
    i2c_resources_t resources;

    resources.sda = sda;
    resources.scl = scl;

    if ((error = i2c_lock_resources(unit, &resources))) {
    	mtx_unlock(&i2c[unit].mtx);

		return error;
	}

    // Setup
    int buff_len = 0;
    i2c_config_t conf;

    conf.sda_io_num = sda;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = scl;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;

    if (mode == I2C_MASTER) {
        conf.mode = I2C_MODE_MASTER;
        conf.master.clk_speed = speed * 1000;
        buff_len = 0;
    } else {
    	conf.mode = I2C_MODE_SLAVE;
    	conf.slave.addr_10bit_en = addr10_en;
    	conf.slave.slave_addr = addr;
    	buff_len = 1024;
    }

    i2c_param_config(unit, &conf);
    i2c_driver_install(unit, conf.mode, buff_len, buff_len, 0);

    i2c[unit].mode = mode;
    i2c[unit].setup = 1;

	mtx_unlock(&i2c[unit].mtx);

    syslog(LOG_INFO,
        "i2c%u at pins scl=%s%d/sdc=%s%d", unit,
        gpio_portname(scl), gpio_name(scl),
        gpio_portname(sda), gpio_name(sda)
    );

    return NULL;
}

driver_error_t *i2c_start(int unit, int *transaction) {
	driver_error_t *error;
	i2c_cmd_handle_t cmd;

	// Sanity checks
	if ((error = i2c_check(unit))) {
		return error;
	}

	if (i2c[unit].mode != I2C_MASTER) {
		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_OPERATION, NULL);
	}

	mtx_lock(&i2c[unit].mtx);

	// If transaction is valid get command, we don't need to create
	if (*transaction != I2C_TRANSACTION_INITIALIZER) {
	    if (list_get(&transactions, *transaction, (void **)&cmd)) {
	    	mtx_unlock(&i2c[unit].mtx);

			return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_TRANSACTION, NULL);
	    }
	} else {
		// Create command
		cmd = i2c_cmd_link_create();
		if (!cmd) {
			mtx_unlock(&i2c[unit].mtx);
			return driver_operation_error(I2C_DRIVER, I2C_ERR_NOT_ENOUGH_MEMORY, NULL);
		}

		// Add transaction to list
		if (list_add(&transactions, cmd, transaction)) {
			*transaction = I2C_TRANSACTION_INITIALIZER;

			i2c_cmd_link_delete(cmd);
			mtx_unlock(&i2c[unit].mtx);
			return driver_operation_error(I2C_DRIVER, I2C_ERR_NOT_ENOUGH_MEMORY, NULL);
		}
	}

	i2c_master_start(cmd);

	mtx_unlock(&i2c[unit].mtx);

	return NULL;
}

driver_error_t *i2c_stop(int unit, int *transaction) {
	driver_error_t *error;

	// Sanity checks
	if ((error = i2c_check(unit))) {
		return error;
	}

	if (i2c[unit].mode != I2C_MASTER) {
		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_OPERATION, NULL);
	}

	mtx_lock(&i2c[unit].mtx);

	// Get command
	i2c_cmd_handle_t cmd;
    if (list_get(&transactions, *transaction, (void **)&cmd)) {
    	mtx_unlock(&i2c[unit].mtx);

		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_TRANSACTION, NULL);
    }

	i2c_master_stop(cmd);
	esp_err_t err = i2c_master_cmd_begin(unit, cmd, 1000 / portTICK_RATE_MS);
	i2c_cmd_link_delete(cmd);
	list_remove(&transactions, *transaction, 0);

	*transaction = I2C_TRANSACTION_INITIALIZER;

	if (err == ESP_FAIL) {
    	mtx_unlock(&i2c[unit].mtx);
		return driver_operation_error(I2C_DRIVER, I2C_ERR_NOT_ACK, NULL);
	} else if (err == ESP_ERR_TIMEOUT) {
    	mtx_unlock(&i2c[unit].mtx);
		return driver_operation_error(I2C_DRIVER, I2C_ERR_TIMEOUT, NULL);
	}

	mtx_unlock(&i2c[unit].mtx);

	return NULL;
}

driver_error_t *i2c_write_address(int unit, int *transaction, char address, int read) {
	driver_error_t *error;

	// Sanity checks
	if ((error = i2c_check(unit))) {
		return error;
	}

	if (i2c[unit].mode != I2C_MASTER) {
		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_OPERATION, NULL);
	}

	mtx_lock(&i2c[unit].mtx);

	// Get command
	i2c_cmd_handle_t cmd;
    if (list_get(&transactions, *transaction, (void **)&cmd)) {
    	mtx_unlock(&i2c[unit].mtx);

		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_TRANSACTION, NULL);
    }

    i2c_master_write_byte(cmd, address << 1 | (read?I2C_MASTER_READ:I2C_MASTER_WRITE), 1);

    mtx_unlock(&i2c[unit].mtx);

	return NULL;
}

driver_error_t *i2c_write(int unit, int *transaction, char *data, int len) {
	driver_error_t *error;

	// Sanity checks
	if ((error = i2c_check(unit))) {
		return error;
	}

	if (i2c[unit].mode != I2C_MASTER) {
		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_OPERATION, NULL);
	}

	mtx_lock(&i2c[unit].mtx);

	// Get command
	i2c_cmd_handle_t cmd;
    if (list_get(&transactions, *transaction, (void **)&cmd)) {
    	mtx_unlock(&i2c[unit].mtx);

		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_TRANSACTION, NULL);
    }

    i2c_master_write(cmd, (uint8_t *)data, len, 1);

    mtx_unlock(&i2c[unit].mtx);

    return NULL;
}

driver_error_t *i2c_read(int unit, int *transaction, char *data, int len) {
	driver_error_t *error;

	// Sanity checks
	if ((error = i2c_check(unit))) {
		return error;
	}

	if (i2c[unit].mode != I2C_MASTER) {
		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_OPERATION, NULL);
	}

	mtx_lock(&i2c[unit].mtx);

	// Get command
	i2c_cmd_handle_t cmd;
    if (list_get(&transactions, *transaction, (void **)&cmd)) {
    	mtx_unlock(&i2c[unit].mtx);

		return driver_operation_error(I2C_DRIVER, I2C_ERR_INVALID_TRANSACTION, NULL);
    }

    i2c_master_read(cmd, (uint8_t *)data, len, 1);
    mtx_unlock(&i2c[unit].mtx);

    return NULL;
}

#endif
