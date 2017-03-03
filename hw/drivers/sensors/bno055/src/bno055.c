/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * resarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>
#include <errno.h>
#include <assert.h>

#include "defs/error.h"
#include "os/os.h"
#include "sysinit/sysinit.h"
#include "hal/hal_i2c.h"
#include "sensor/sensor.h"
#include "sensor/accel.h"
#include "sensor/mag.h"
#include "sensor/quat.h"
#include "sensor/euler.h"
#include "bno055/bno055.h"
#include "bno055_priv.h"

#if MYNEWT_VAL(BNO055_LOG)
#include "log/log.h"
#endif

#if MYNEWT_VAL(BNO055_STATS)
#include "stats/stats.h"
#endif

#if MYNEWT_VAL(BNO055_STATS)
/* Define the stats section and records */
STATS_SECT_START(bno055_stat_section)
    STATS_SECT_ENTRY(errors)
STATS_SECT_END

/* Define stat names for querying */
STATS_NAME_START(bno055_stat_section)
    STATS_NAME(bno055_stat_section, errors)
STATS_NAME_END(bno055_stat_section)

/* Global variable used to hold stats data */
STATS_SECT_DECL(bno055_stat_section) g_bno055stats;
#endif

#if MYNEWT_VAL(BNO055_LOG)
#define LOG_MODULE_BNO055 (305)
#define BNO055_INFO(...)  LOG_INFO(&_log, LOG_MODULE_BNO055, __VA_ARGS__)
#define BNO055_ERR(...)   LOG_ERROR(&_log, LOG_MODULE_BNO055, __VA_ARGS__)
static struct log _log;
#else
#define BNO055_INFO(...)
#define BNO055_ERR(...)
#endif

/* Exports for the sensor interface.
 */
static void *bno055_sensor_get_interface(struct sensor *, sensor_type_t);
static int bno055_sensor_read(struct sensor *, sensor_type_t,
        sensor_data_func_t, void *, uint32_t);
static int bno055_sensor_get_config(struct sensor *, sensor_type_t,
        struct sensor_cfg *);

static const struct sensor_driver g_bno055_sensor_driver = {
    bno055_sensor_get_interface,
    bno055_sensor_read,
    bno055_sensor_get_config
};

/**
 * Writes a single byte to the specified register
 *
 * @param The register address to write to
 * @param The value to write
 *
 * @return 0 on success, non-zero error on failure.
 */
int
bno055_write8(uint8_t reg, uint8_t value)
{
    int rc;
    uint8_t payload[2] = { reg, value};

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(BNO055_I2CADDR),
        .len = 2,
        .buffer = payload
    };

    rc = hal_i2c_master_write(MYNEWT_VAL(BNO055_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC, 1);
    if (rc) {
        BNO055_ERR("Failed to write to 0x%02X:0x%02X with value 0x%02X\n",
                       data_struct.address, reg, value);
#if MYNEWT_VAL(BNO055_STATS)
        STATS_INC(g_bno055stats, errors);
#endif
    }

    return rc;
}

/**
 * Writes a multiple bytes to the specified register
 *
 * @param The register address to write to
 * @param The data buffer to write from
 *
 * @return 0 on success, non-zero error on failure.
 */
int
bno055_writelen(uint8_t reg, uint8_t *buffer, uint8_t len)
{
    int rc;
    uint8_t payload[23] = { reg, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0};

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(BNO055_I2CADDR),
        .len = 1,
        .buffer = payload
    };

    memcpy(&payload[1], buffer, len);

    /* Register write */
    rc = hal_i2c_master_write(MYNEWT_VAL(BNO055_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, 1);
    if (rc) {
        BNO055_ERR("I2C access failed at address 0x%02X\n", addr);
#if MYNEWT_VAL(BNO055_STATS)
        STATS_INC(g_bno055stats, errors);
#endif
        goto err;
    }

    memset(payload, 0, sizeof(payload));
    data_struct.len = len;
    rc = hal_i2c_master_write(MYNEWT_VAL(BNO055_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, len);

    if (rc) {
        BNO055_ERR("Failed to read from 0x%02X:0x%02X\n", addr, reg);
#if MYNEWT_VAL(BNO055_STATS)
        STATS_INC(g_bno055stats, errors);
#endif
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Reads a single byte from the specified register
 *
 * @param The register address to read from
 * @param Pointer to where the register value should be written
 *
 * @return 0 on success, non-zero error on failure.
 */
int
bno055_read8(uint8_t reg, uint8_t *value)
{
    int rc;
    uint8_t payload;

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(BNO055_I2CADDR),
        .len = 1,
        .buffer = &payload
    };

    /* Register write */
    payload = reg;
    rc = hal_i2c_master_write(MYNEWT_VAL(BNO055_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, 0);
    if (rc) {
        BNO055_ERR("I2C register write failed at address 0x%02X:0x%02X\n",
                   data_struct.address, reg);
#if MYNEWT_VAL(BNO055_STATS)
        STATS_INC(g_bno055stats, errors);
#endif
        goto err;
    }

    /* Read one byte back */
    payload = 0;
    rc = hal_i2c_master_read(MYNEWT_VAL(BNO055_I2CBUS), &data_struct,
                             OS_TICKS_PER_SEC / 10, 1);
    *value = payload;
    if (rc) {
        BNO055_ERR("Failed to read from 0x%02X:0x%02X\n", addr, reg);
#if MYNEWT_VAL(BNO055_STATS)
        STATS_INC(g_bno055stats, errors);
#endif
    }

err:
    return rc;
}

/**
 * Read data from the sensor of variable length (MAX: 8 bytes)
 *
 *
 * @param Register to read from
 * @param Bufer to read into
 * @param Length of the buffer
 *
 * @return 0 on success and non-zero on failure
 */
static int
bno055_readlen(uint8_t reg, uint8_t *buffer, uint8_t len)
{
    int rc;
    uint8_t payload[23] = { reg, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0};

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(BNO055_I2CADDR),
        .len = 1,
        .buffer = payload
    };

    /* Clear the supplied buffer */
    memset(buffer, 0, 22);

    /* Register write */
    rc = hal_i2c_master_write(MYNEWT_VAL(BNO055_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, 1);
    if (rc) {
        BNO055_ERR("I2C access failed at address 0x%02X\n", addr);
#if MYNEWT_VAL(BNO055_STATS)
        STATS_INC(g_bno055stats, errors);
#endif
        goto err;
    }

    /* Read len bytes back */
    memset(payload, 0, sizeof(payload));
    data_struct.len = len;
    rc = hal_i2c_master_read(MYNEWT_VAL(BNO055_I2CBUS), &data_struct,
                             OS_TICKS_PER_SEC / 10, 1);

    if (rc) {
        BNO055_ERR("Failed to read from 0x%02X:0x%02X\n", addr, reg);
#if MYNEWT_VAL(BNO055_STATS)
        STATS_INC(g_bno055stats, errors);
#endif
        goto err;
    }

    /* Copy the I2C results into the supplied buffer */
    memcpy(buffer, payload, len);

    return 0;
err:
    return rc;
}

/**
 * Setting operation mode for the bno055 sensor
 *
 * @param Operation mode for the sensor
 * @return 0 on success, non-zero on failure
 */
int
bno055_set_opr_mode(uint8_t mode)
{
    int rc;

    rc = bno055_write8(BNO055_OPR_MODE_ADDR, BNO055_OPR_MODE_CONFIG);
    if (rc) {
        goto err;
    }

    os_time_delay((OS_TICKS_PER_SEC * 19)/1000 + 1);

    rc = bno055_write8(BNO055_OPR_MODE_ADDR, mode);
    if (rc) {
        goto err;
    }

    /* Refer table 3-6 in the datasheet for the delay values */
    os_time_delay((OS_TICKS_PER_SEC * 7)/1000 + 1);

    return 0;
err:
    return rc;
}

/**
 * Setting power mode for the bno055 sensor
 *
 * @param power mode for the sensor
 * @return 0 on success, non-zero on failure
 */
int
bno055_set_pwr_mode(uint8_t mode)
{
    int rc;

    rc = bno055_write8(BNO055_PWR_MODE_ADDR, mode);
    if (rc) {
        goto err;
    }

    os_time_delay((OS_TICKS_PER_SEC * 1)/1000 + 1);

    return 0;
err:
    return rc;
}

/**
 * Read current power mode of the sensor
 *
 * @param ptr to mode variableto fill up
 * @return 0 on success, non-zero on failure
 */
int
bno055_get_pwr_mode(uint8_t *mode)
{
    int rc;
    uint8_t val;

    rc = bno055_read8(BNO055_PWR_MODE_ADDR, &val);
    if (rc) {
        goto err;
    }

    *mode = val;

    return 0;
err:
    return rc;
}

/**
 * Setting units for the bno055 sensor
 *
 * @param power mode for the sensor
 * @return 0 on success, non-zero on failure
 */
int
bno055_set_units(uint8_t val)
{
    int rc;

    rc = bno055_write8(BNO055_UNIT_SEL_ADDR, val);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Get units of the sensor
 *
 * @param ptr to the units variable
 * @return 0 on success, non-zero on failure
 */
int
bno055_get_units(uint8_t *units)
{
    int rc;
    uint8_t val;

    rc = bno055_read8(BNO055_UNIT_SEL_ADDR, &val);
    if (rc) {
        goto err;
    }

    *units = val;

    return 0;
err:
    return rc;
}

/**
 * Read current operational mode of the sensor
 *
 * @param ptr to mode variable to fill up
 * @return 0 on success, non-zero on failure
 */
int
bno055_get_opr_mode(uint8_t *mode)
{
    int rc;
    uint8_t val;

    rc = bno055_read8(BNO055_OPR_MODE_ADDR, &val);
    if (rc) {
        goto err;
    }

    *mode = val;

    return 0;
err:
    return rc;
}

static int
bno055_default_cfg(struct bno055_cfg *cfg)
{
    cfg->bc_opr_mode = BNO055_OPR_MODE_ACCONLY;
    cfg->bc_pwr_mode = BNO055_PWR_MODE_NORMAL;
    cfg->bc_units = BNO055_DO_FORMAT_ANDROID|
                    BNO055_ACC_UNIT_MS2|
                    BNO055_ANGRATE_UNIT_DPS|
                    BNO055_EULER_UNIT_DEG|
                    BNO055_TEMP_UNIT_DEGC;
    cfg->bc_placement = BNO055_AXIS_CFG_P1;
    cfg->bc_acc_range = BNO055_ACC_CFG_RNG_4G;
    cfg->bc_acc_bw = BNO055_ACC_CFG_BW_6_25HZ;
    cfg->bc_acc_res = 14;
    cfg->bc_gyro_range = BNO055_GYR_CFG_RNG_2000DPS;
    cfg->bc_gyro_bw = BNO055_GYR_CFG_BW_32HZ;
    cfg->bc_gyro_res = 16;
    cfg->bc_mag_odr = BNO055_MAG_CFG_ODR_2HZ;
    cfg->bc_mag_xy_rep = 15;
    cfg->bc_mag_z_rep = 16;
    cfg->bc_mag_res = BNO055_MAG_RES_13_13_15;

    return 0;
}

/**
 * Expects to be called back through os_dev_create().
 *
 * @param The device object associated with this accellerometer
 * @param Argument passed to OS device init, unused
 *
 * @return 0 on success, non-zero error on failure.
 */
int
bno055_init(struct os_dev *dev, void *arg)
{
    struct bno055 *bno055;
    struct sensor *sensor;
    int rc;

    bno055 = (struct bno055 *) dev;

    rc = bno055_default_cfg(&bno055->cfg);
    if (rc) {
        goto err;
    }

#if MYNEWT_VAL(BNO055_LOG)
    log_register("bno055", &_log, &log_console_handler, NULL, LOG_SYSLEVEL);
#endif

    sensor = &bno055->sensor;

#if MYNEWT_VAL(BNO055_STATS)
    /* Initialise the stats entry */
    rc = stats_init(
        STATS_HDR(g_bno055stats),
        STATS_SIZE_INIT_PARMS(g_bno055stats, STATS_SIZE_32),
        STATS_NAME_INIT_PARMS(bno055_stat_section));
    SYSINIT_PANIC_ASSERT(rc == 0);
    /* Register the entry with the stats registry */
    rc = stats_register("bno055", STATS_HDR(g_bno055stats));
    SYSINIT_PANIC_ASSERT(rc == 0);
#endif

    rc = sensor_init(sensor, dev);
    if (rc != 0) {
        goto err;
    }

    /* Add the accelerometer/magnetometer driver */
    rc = sensor_set_driver(sensor, SENSOR_TYPE_ACCELEROMETER         |
            SENSOR_TYPE_MAGNETIC_FIELD | SENSOR_TYPE_GYROSCOPE       |
            SENSOR_TYPE_TEMPERATURE    | SENSOR_TYPE_ROTATION_VECTOR |
            SENSOR_TYPE_GRAVITY        | SENSOR_TYPE_LINEAR_ACCEL    |
            SENSOR_TYPE_EULER, (struct sensor_driver *) &g_bno055_sensor_driver);
    if (rc != 0) {
        goto err;
    }

    rc = sensor_mgr_register(sensor);
    if (rc != 0) {
        goto err;
    }

    return (0);
err:
    return (rc);
}

/**
 * Get chip ID from the sensor
 *
 * @param Pointer to the variable to fill up chip ID in
 * @return 0 on success, non-zero on failure
 */
int
bno055_get_chip_id(uint8_t *id)
{
    int rc;
    uint8_t idtmp;

    /* Check if we can read the chip address */
    rc = bno055_read8(BNO055_CHIP_ID_ADDR, &idtmp);
    if (rc) {
        goto err;
    }

    *id = idtmp;

    return 0;
err:
    return rc;
}

/**
 * Use external crystal 32.768KHz
 *
 * @param operational mode of the sensor
 * @return 0 on success, non-zero on failure
 */
static int
bno055_set_ext_xtal_use(uint8_t use_xtal, uint8_t mode)
{
    int rc;

    if (mode != BNO055_OPR_MODE_CONFIG) {
        /* Switch to config mode */
        rc = bno055_set_opr_mode(BNO055_OPR_MODE_CONFIG);
        if (rc) {
            goto err;
        }
    }

    os_time_delay((OS_TICKS_PER_SEC * 25)/1000 + 1);

    rc = bno055_write8(BNO055_PAGE_ID_ADDR, 0);
    if (rc) {
        goto err;
    }

    if (use_xtal) {
        /* Use External Clock */
        rc = bno055_write8(BNO055_SYS_TRIGGER_ADDR, BNO055_SYS_TRIGGER_CLK_SEL);
        if (rc) {
            goto err;
        }
    } else {
        /* Use Internal clock */
        rc = bno055_write8(BNO055_SYS_TRIGGER_ADDR, 0x00);
        if (rc) {
            goto err;
        }
    }

    os_time_delay((OS_TICKS_PER_SEC * 10)/1000 + 1);

    /* Reset to previous operating mode */
    rc = bno055_set_opr_mode(mode);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

int
bno055_placement_cfg(uint8_t placement)
{
    uint8_t remap_cfg;
    uint8_t remap_sign;
    int rc;

    rc = SYS_EOK;

    switch(placement) {

    case BNO055_AXIS_CFG_P0:
        remap_cfg = BNO055_REMAP_CONFIG_P0;
        remap_sign = BNO055_REMAP_SIGN_P0;
        break;
    case BNO055_AXIS_CFG_P1:
        remap_cfg = BNO055_REMAP_CONFIG_P1;
        remap_sign = BNO055_REMAP_SIGN_P1;
        break;
    case BNO055_AXIS_CFG_P2:
        remap_cfg = BNO055_REMAP_CONFIG_P2;
        remap_sign = BNO055_REMAP_SIGN_P2;
        break;
    case BNO055_AXIS_CFG_P3:
        remap_cfg = BNO055_REMAP_CONFIG_P3;
        remap_sign = BNO055_REMAP_SIGN_P3;
        break;
    case BNO055_AXIS_CFG_P4:
        remap_cfg = BNO055_REMAP_CONFIG_P4;
        remap_sign = BNO055_REMAP_SIGN_P4;
        break;
    case BNO055_AXIS_CFG_P5:
        remap_cfg = BNO055_REMAP_CONFIG_P5;
        remap_sign = BNO055_REMAP_SIGN_P5;
        break;
    case BNO055_AXIS_CFG_P6:
        remap_cfg = BNO055_REMAP_CONFIG_P6;
        remap_sign = BNO055_REMAP_SIGN_P6;
        break;
    case BNO055_AXIS_CFG_P7:
        remap_cfg = BNO055_REMAP_CONFIG_P7;
        remap_sign = BNO055_REMAP_SIGN_P7;
        break;
    default:
        BNO055_ERR("Invalid Axis config, Assuming P1(default) \n");
        rc = SYS_EINVAL;
        goto err;
    }

    rc = bno055_write8(BNO055_AXIS_MAP_CONFIG_ADDR, remap_cfg);
    if (rc) {
        goto err;
    }

    rc = bno055_write8(BNO055_AXIS_MAP_SIGN_ADDR, remap_sign);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

int
bno055_acc_cfg(struct bno055_cfg *cfg)
{
    int rc;

    rc = bno055_write8(BNO055_ACCEL_CONFIG_ADDR, cfg->bc_acc_range|
                       cfg->bc_acc_bw|cfg->bc_acc_opr_mode);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

int
bno055_mag_cfg(struct bno055_cfg *cfg)
{
    int rc;

    rc = bno055_write8(BNO055_MAG_CONFIG_ADDR, cfg->bc_mag_odr|
                       cfg->bc_mag_pwr_mode|cfg->bc_mag_opr_mode);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

int
bno055_gyro_cfg(struct bno055_cfg *cfg)
{
    int rc;

    rc = bno055_write8(BNO055_GYRO_CONFIG_ADDR, cfg->bc_gyro_range|
                       cfg->bc_gyro_bw|cfg->bc_gyro_opr_mode);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

int
bno055_config(struct bno055 *bno055, struct bno055_cfg *cfg)
{
    int rc;
    uint8_t id;
    uint8_t mode;

    /* Check if we can read the chip address */
    rc = bno055_get_chip_id(&id);
    if (rc) {
        goto err;
    }

    if (id != BNO055_ID) {
        os_time_delay((OS_TICKS_PER_SEC * 100)/1000 + 1);

        rc = bno055_get_chip_id(&id);
        if (rc) {
            goto err;
        }

        if(id != BNO055_ID) {
            rc = SYS_EINVAL;
            goto err;
        }
    }

    /* Reset sensor */
    rc = bno055_write8(BNO055_SYS_TRIGGER_ADDR, BNO055_SYS_TRIGGER_RST_SYS);
    if (rc) {
        goto err;
    }

    os_time_delay(OS_TICKS_PER_SEC);

    rc = bno055_set_opr_mode(BNO055_OPR_MODE_CONFIG);
    if (rc) {
        goto err;
    }

    /* Set to normal power mode */
    rc = bno055_set_pwr_mode(cfg->bc_pwr_mode);
    if (rc) {
        goto err;
    }

    bno055->cfg.bc_pwr_mode = cfg->bc_pwr_mode;

    /**
     * As per Section 5.5 in the BNO055 Datasheet,
     * external crystal should be used for accurate
     * results
     */
    rc = bno055_set_ext_xtal_use(cfg->bc_use_ext_xtal, BNO055_OPR_MODE_CONFIG);
    if (rc) {
        goto err;
    }

    bno055->cfg.bc_use_ext_xtal = cfg->bc_use_ext_xtal;

    /* Setting units and data output format */
    rc = bno055_set_units(cfg->bc_units);
    if (rc) {
        goto err;
    }

    bno055->cfg.bc_units = cfg->bc_units;

    /* Change mode to requested mode */
    rc = bno055_set_opr_mode(cfg->bc_opr_mode);
    if (rc) {
        goto err;
    }

    os_time_delay(OS_TICKS_PER_SEC/2);

    rc = bno055_get_opr_mode(&mode);
    if (rc) {
        goto err;
    }

    if (cfg->bc_opr_mode != mode) {

        /* Trying to set operation mode again */
        rc = bno055_set_opr_mode(cfg->bc_opr_mode);
        if (rc) {
            goto err;
        }

        rc = bno055_get_opr_mode(&mode);

        if (rc) {
            goto err;
        }

        if (cfg->bc_opr_mode != mode) {
            BNO055_ERR("Config mode and read mode do not match.\n");
            rc = SYS_EINVAL;
            goto err;
        }
    }

    bno055->cfg.bc_opr_mode = cfg->bc_opr_mode;

    return 0;
err:
    return rc;
}

/**
 * Get quat data from sensor
 *
 * @param sensor quat data to be filled up
 * @return 0 on success, non-zero on error
 */
int
bno055_get_quat_data(void *datastruct)
{
    uint8_t buffer[8];
    double scale;
    int rc;
    struct sensor_quat_data *sqd;

    sqd = (struct sensor_quat_data *)datastruct;

    /* As per Section 3.6.5.5 Orientation (Quaternion) */
    scale = (1.0 / (1<<14));

    memset (buffer, 0, 8);

    /* Read quat data */
    rc = bno055_readlen(BNO055_QUATERNION_DATA_W_LSB_ADDR, buffer, 8);

    if (rc) {
        goto err;
    }

    sqd->sqd_w = ((((uint16_t)buffer[1]) << 8) | ((uint16_t)buffer[0])) * scale;
    sqd->sqd_x = ((((uint16_t)buffer[3]) << 8) | ((uint16_t)buffer[2])) * scale;
    sqd->sqd_y = ((((uint16_t)buffer[5]) << 8) | ((uint16_t)buffer[4])) * scale;
    sqd->sqd_z = ((((uint16_t)buffer[7]) << 8) | ((uint16_t)buffer[6])) * scale;

    return 0;
err:
    return rc;
}

/**
 * Find register based on sensor type
 *
 * @return register address
 */
static int
bno055_find_reg(sensor_type_t type, uint8_t *reg)
{
    int rc;

    rc = SYS_EOK;
    switch(type) {
        case SENSOR_TYPE_ACCELEROMETER:
            *reg = BNO055_ACCEL_DATA_X_LSB_ADDR;
            break;
        case SENSOR_TYPE_GYROSCOPE:
            *reg = BNO055_GYRO_DATA_X_LSB_ADDR;
            break;
        case SENSOR_TYPE_MAGNETIC_FIELD:
            *reg = BNO055_MAG_DATA_X_LSB_ADDR;
            break;
        case SENSOR_TYPE_EULER:
            *reg = BNO055_EULER_H_LSB_ADDR;
            break;
        case SENSOR_TYPE_LINEAR_ACCEL:
            *reg = BNO055_LINEAR_ACCEL_DATA_X_LSB_ADDR;
            break;
        case SENSOR_TYPE_GRAVITY:
            *reg = BNO055_GRAVITY_DATA_X_LSB_ADDR;
            break;
        default:
            BNO055_ERR("Not supported sensor type: %d\n", type);
            rc = SYS_EINVAL;
            break;
    }

    return rc;
}

/**
 * Get vector data from sensor
 *
 * @param pointer to teh structure to be filled up
 * @param Type of sensor
 * @return 0 on success, non-zero on error
 */
int
bno055_get_vector_data(void *datastruct, int type)
{

    uint8_t payload[6];
    int16_t x, y, z;
    struct sensor_mag_data *smd;
    struct sensor_accel_data *sad;
    struct sensor_euler_data *sed;
    uint8_t reg;
    uint8_t units;
    float acc_div;
    float gyro_div;
    float euler_div;

    int rc;

    memset (payload, 0, 6);

    x = y = z = 0;

    rc = bno055_find_reg(type, &reg);
    if (rc) {
        goto err;
    }

    rc = bno055_readlen(reg, payload, 6);
    if (rc) {
        goto err;
    }

    x = ((int16_t)payload[0]) | (((int16_t)payload[1]) << 8);
    y = ((int16_t)payload[2]) | (((int16_t)payload[3]) << 8);
    z = ((int16_t)payload[4]) | (((int16_t)payload[5]) << 8);

    rc = bno055_get_units(&units);
    if (rc) {
        goto err;
    }

    acc_div  = units & BNO055_ACC_UNIT_MG ? 1.0:100.0;
    gyro_div = units & BNO055_ANGRATE_UNIT_RPS ? 900.0:16.0;
    euler_div = units & BNO055_EULER_UNIT_RAD ? 16.0:900.0;

    /**
     * Convert the value to an appropriate range (section 3.6.4)
     */
    switch(type) {
        case SENSOR_TYPE_MAGNETIC_FIELD:
            smd = datastruct;
            /* 1uT = 16 LSB */
            smd->smd_x = ((double)x)/16.0;
            smd->smd_y = ((double)y)/16.0;
            smd->smd_z = ((double)z)/16.0;
            break;
        case SENSOR_TYPE_GYROSCOPE:
            sad = datastruct;
            /* 1rps = 900 LSB */
            sad->sad_x = ((double)x)/gyro_div;
            sad->sad_y = ((double)y)/gyro_div;
            sad->sad_z = ((double)z)/gyro_div;
            break;
        case SENSOR_TYPE_EULER:
            sad = datastruct;
            /* 1 degree = 16 LSB */
            sed->sed_h = ((double)x)/euler_div;
            sed->sed_r = ((double)y)/euler_div;
            sed->sed_p = ((double)z)/euler_div;
            break;
        case SENSOR_TYPE_ACCELEROMETER:
        case SENSOR_TYPE_LINEAR_ACCEL:
        case SENSOR_TYPE_GRAVITY:
            sad = datastruct;
            /* 1m/s^2 = 100 LSB */
            sad->sad_x = ((double)x)/acc_div;
            sad->sad_y = ((double)y)/acc_div;
            sad->sad_z = ((double)z)/acc_div;
            break;
        default:
            BNO055_ERR("Not supported sensor type: %d\n", type);
            rc = SYS_EINVAL;
            goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Get temperature from bno055 sensor
 *
 * @param pointer to the temperature variable to be filled up
 * @return 0 on success, non-zero on error
 */
int
bno055_get_temp(int8_t *temp)
{
    int rc;
    uint8_t units;
    uint8_t div;

    rc = bno055_read8(BNO055_TEMP_ADDR, (uint8_t *)temp);
    if (rc) {
        goto err;
    }

    rc = bno055_get_units(&units);
    if (rc) {
        goto err;
    }

    div = units & BNO055_TEMP_UNIT_DEGF ? 2 : 1;

    *temp = *temp/div;

    return 0;
err:
    return rc;
}

/**
 * Get sensor data of specific type. This function also allocates a buffer
 * to fill up the data in.
 *
 * @param Sensor structure used by the SensorAPI
 * @param Sensor type
 * @param Data function provided by the caler of the API
 * @param Argument for the data function
 * @param Timeout if any for reading
 * @return 0 on success, non-zero on error
 */
static int
bno055_sensor_read(struct sensor *sensor, sensor_type_t type,
        sensor_data_func_t data_func, void *data_arg, uint32_t timeout)
{
    void *databuf;
    int rc;

    /* Since this is the biggest struct, malloc space for it */
    databuf = malloc(sizeof(struct sensor_quat_data));

    if (type == SENSOR_TYPE_ROTATION_VECTOR) {
        /* Quaternion is a rotation vector */
        rc = bno055_get_quat_data(databuf);
        if (rc) {
            goto err;
        }
    } else if (type == SENSOR_TYPE_TEMPERATURE) {
        rc = bno055_get_temp(databuf);
        if (rc) {
            goto err;
        }
    } else {
        /* Get vector data, accel or gravity values */
        rc = bno055_get_vector_data(databuf, type);
        if (rc) {
            goto err;
        }
    }

    /* Call data function */
    rc = data_func(sensor, data_arg, databuf);
    if (rc) {
        goto err;
    }

    /* Free the data buffer */
    free(databuf);

    return 0;
err:
    return rc;
}

/**
 * Gets system status, test results and errors if any from the sensor
 *
 * @param ptr to system status
 * @param ptr to self test result
 * @param ptr to system error
 */
int
bno055_get_sys_status(uint8_t *system_status, uint8_t *self_test_result, uint8_t *system_error)
{
    int rc;

    rc = bno055_write8(BNO055_PAGE_ID_ADDR, 0);
    if (rc) {
        goto err;
    }
    /**
     * System Status (see section 4.3.58)
     * ---------------------------------
     * bit 0: Idle
     * bit 1: System Error
     * bit 2: Initializing Peripherals
     * bit 3: System Iniitalization
     * bit 4: Executing Self-Test
     * bit 5: Sensor fusion algorithm running
     * bit 6: System running without fusion algorithms
     */

    if (system_status != 0) {
        rc = bno055_read8(BNO055_SYS_STAT_ADDR, system_status);
        if (rc) {
            goto err;
        }
    }

    /**
     * Self Test Results (see section )
     * --------------------------------
     * 1: test passed, 0: test failed
     * bit 0: Accelerometer self test
     * bit 1: Magnetometer self test
     * bit 2: Gyroscope self test
     * bit 3: MCU self test
     *
     * 0x0F : All Good
     */

    if (self_test_result != 0) {
        rc = bno055_read8(BNO055_SELFTEST_RESULT_ADDR, self_test_result);
        if (rc) {
            goto err;
        }
    }

    /**
     * System Error (see section 4.3.59)
     * ---------------------------------
     * bit 0  : No error
     * bit 1  : Peripheral initialization error
     * bit 2  : System initialization error
     * bit 3  : Self test result failed
     * bit 4  : Register map value out of range
     * bit 5  : Register map address out of range
     * bit 6  : Register map write error
     * bit 7  : BNO low power mode not available for selected operat ion mode
     * bit 8  : Accelerometer power mode not available
     * bit 9  : Fusion algorithm configuration error
     * bit 10 : Sensor configuration error
     */

    if (system_error != 0) {
        rc = bno055_read8(BNO055_SYS_ERR_ADDR, system_error);
        if (rc) {
            goto err;
        }
    }

    os_time_delay((OS_TICKS_PER_SEC * 200)/1000 + 1);

    return 0;
err:
    return rc;
}

/**
 * Get Revision info for different sensors in the bno055
 *
 * @param pass the pointer to the revision structure
 * @return 0 on success, non-zero on error
 */
int
bno055_get_rev_info(struct bno055_rev_info *ri)
{
    uint8_t sw_rev_l, sw_rev_h;
    int rc;

    memset(ri, 0, sizeof(struct bno055_rev_info));

    /* Check the accelerometer revision */
    rc = bno055_read8(BNO055_ACCEL_REV_ID_ADDR, &(ri->bri_accel_rev));
    if (rc) {
        goto err;
    }

    /* Check the magnetometer revision */
    rc = bno055_read8(BNO055_MAG_REV_ID_ADDR, &(ri->bri_mag_rev));
    if (rc) {
        goto err;
    }

    /* Check the gyroscope revision */
    rc = bno055_read8(BNO055_GYRO_REV_ID_ADDR, &(ri->bri_gyro_rev));
    if (rc) {
        goto err;
    }

    rc = bno055_read8(BNO055_BL_REV_ID_ADDR, &(ri->bri_bl_rev));
    if (rc) {
        goto err;
    }

    /* Check the SW revision */
    rc = bno055_read8(BNO055_SW_REV_ID_LSB_ADDR, &sw_rev_l);
    if (rc) {
        goto err;
    }

    rc = bno055_read8(BNO055_SW_REV_ID_MSB_ADDR, &sw_rev_h);
    if (rc) {
        goto err;
    }

    ri->bri_sw_rev = (((uint16_t)sw_rev_h) << 8) | ((uint16_t)sw_rev_l);

    return 0;
err:
    return rc;
}

/**
 * Gets current calibration status
 *
 * @param Calibration info structure to fill up calib state
 * @return 0 on success, non-zero on failure
 */
int
bno055_get_calib_status(struct bno055_calib_info *bci)
{
    uint8_t status;
    int rc;

    rc = bno055_read8(BNO055_CALIB_STAT_ADDR, &status);
    if (rc) {
        goto err;
    }

    bci->bci_sys = (status >> 6) & 0x03;
    bci->bci_gyro = (status >> 4) & 0x03;
    bci->bci_accel = (status >> 2) & 0x03;
    bci->bci_mag = status & 0x03;

    return 0;
err:
    return rc;
}

/**
 * Checks if bno055 is fully calibrated
 *
 * @return 0 on success, non-zero on failure
 */
int
bno055_is_calib(void)
{
    struct bno055_calib_info bci;
    int rc;

    rc = bno055_get_calib_status(&bci);
    if (rc) {
        goto err;
    }

    if (bci.bci_sys< 3 || bci.bci_gyro < 3 || bci.bci_accel < 3 || bci.bci_mag < 3) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Reads the sensor's offset registers into a byte array
 *
 * @param byte array to return offsets into
 * @return 0 on success, non-zero on failure
 *
 */
int
bno055_get_raw_sensor_offsets(uint8_t *offsets)
{
    uint8_t prev_mode;
    int rc;

    rc = SYS_EOK;
    if (!bno055_is_calib()) {
        rc = bno055_get_opr_mode(&prev_mode);
        if (rc) {
            goto err;
        }

        rc = bno055_set_opr_mode(BNO055_OPR_MODE_CONFIG);
        if (rc) {
            goto err;
        }

        rc = bno055_readlen(BNO055_ACCEL_OFFSET_X_LSB_ADDR, offsets,
                            BNO055_NUM_OFFSET_REGISTERS);
        if (rc) {
            goto err;
        }

        rc = bno055_set_opr_mode(prev_mode);
        if (rc) {
            goto err;
        }

        return 0;
    }
err:
    return rc;
}

/**
 *
 * Reads the sensor's offset registers into an offset struct
 *
 * @param structure to fill up offsets data
 * @return 0 on success, non-zero on failure
 */
int
bno055_get_sensor_offsets(struct bno055_sensor_offsets *offsets)
{
    uint8_t payload[22];
    int rc;


    rc = bno055_get_raw_sensor_offsets(payload);
    if (rc) {
        goto err;
    }

    offsets->bso_acc_off_x  = (payload[1] << 8)  | payload[0];
    offsets->bso_acc_off_y  = (payload[3] << 8)  | payload[2];
    offsets->bso_acc_off_z  = (payload[5] << 8)  | payload[4];

    offsets->bso_gyro_off_x = (payload[7] << 8)  | payload[6];
    offsets->bso_gyro_off_y = (payload[9] << 8)  | payload[8];
    offsets->bso_gyro_off_z = (payload[11] << 8) | payload[10];

    offsets->bso_mag_off_x  = (payload[13] << 8) | payload[12];
    offsets->bso_mag_off_y  = (payload[15] << 8) | payload[14];
    offsets->bso_mag_off_z  = (payload[17] << 8) | payload[16];

    offsets->bso_acc_radius = (payload[19] << 8) | payload[18];
    offsets->bso_mag_radius = (payload[21] << 8) | payload[20];

    return 0;
err:
    return rc;
}


/**
 *
 * Writes calibration data to the sensor's offset registers
 *
 * @param calibration data
 * @param calibration data length
 * @return 0 on success, non-zero on success
 */
int
bno055_set_sensor_raw_offsets(uint8_t* calibdata, uint8_t len)
{
    uint8_t prev_mode;
    int rc;

    if (len != 22) {
        rc = SYS_EINVAL;
        goto err;
    }

    rc = bno055_get_opr_mode(&prev_mode);
    if (rc) {
        goto err;
    }

    rc = bno055_set_opr_mode(BNO055_OPR_MODE_CONFIG);
    if (rc) {
        goto err;
    }

    os_time_delay((25 * OS_TICKS_PER_SEC)/1000 + 1);

    rc = bno055_writelen(BNO055_ACCEL_OFFSET_X_LSB_ADDR, calibdata, len);
    if (rc) {
        goto err;
    }

    rc = bno055_set_opr_mode(prev_mode);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 *
 * Writes to the sensor's offset registers from an offset struct
 *
 * @param pointer to the offset structure
 * @return 0 on success, non-zero on failure
 */
int
bno055_set_sensor_offsets(struct bno055_sensor_offsets  *offsets)
{
    uint8_t prev_mode;
    int rc;

    rc = bno055_get_opr_mode(&prev_mode);
    if (rc) {
        goto err;
    }

    rc = bno055_set_opr_mode(BNO055_OPR_MODE_CONFIG);
    if (rc) {
        goto err;
    }

    os_time_delay((25 * OS_TICKS_PER_SEC)/1000 + 1);

    rc |= bno055_write8(BNO055_ACCEL_OFFSET_X_LSB_ADDR, (offsets->bso_acc_off_x) & 0x0FF);
    rc |= bno055_write8(BNO055_ACCEL_OFFSET_X_MSB_ADDR, (offsets->bso_acc_off_x >> 8) & 0x0FF);
    rc |= bno055_write8(BNO055_ACCEL_OFFSET_Y_LSB_ADDR, (offsets->bso_acc_off_y) & 0x0FF);
    rc |= bno055_write8(BNO055_ACCEL_OFFSET_Y_MSB_ADDR, (offsets->bso_acc_off_y >> 8) & 0x0FF);
    rc |= bno055_write8(BNO055_ACCEL_OFFSET_Z_LSB_ADDR, (offsets->bso_acc_off_z) & 0x0FF);
    rc |= bno055_write8(BNO055_ACCEL_OFFSET_Z_MSB_ADDR, (offsets->bso_acc_off_z >> 8) & 0x0FF);

    rc |= bno055_write8(BNO055_GYRO_OFFSET_X_LSB_ADDR, (offsets->bso_gyro_off_x) & 0x0FF);
    rc |= bno055_write8(BNO055_GYRO_OFFSET_X_MSB_ADDR, (offsets->bso_gyro_off_x >> 8) & 0x0FF);
    rc |= bno055_write8(BNO055_GYRO_OFFSET_Y_LSB_ADDR, (offsets->bso_gyro_off_y) & 0x0FF);
    rc |= bno055_write8(BNO055_GYRO_OFFSET_Y_MSB_ADDR, (offsets->bso_gyro_off_y >> 8) & 0x0FF);
    rc |= bno055_write8(BNO055_GYRO_OFFSET_Z_LSB_ADDR, (offsets->bso_gyro_off_z) & 0x0FF);
    rc |= bno055_write8(BNO055_GYRO_OFFSET_Z_MSB_ADDR, (offsets->bso_gyro_off_z >> 8) & 0x0FF);

    rc |= bno055_write8(BNO055_MAG_OFFSET_X_LSB_ADDR, (offsets->bso_mag_off_x) & 0x0FF);
    rc |= bno055_write8(BNO055_MAG_OFFSET_X_MSB_ADDR, (offsets->bso_mag_off_x >> 8) & 0x0FF);
    rc |= bno055_write8(BNO055_MAG_OFFSET_Y_LSB_ADDR, (offsets->bso_mag_off_y) & 0x0FF);
    rc |= bno055_write8(BNO055_MAG_OFFSET_Y_MSB_ADDR, (offsets->bso_mag_off_y >> 8) & 0x0FF);
    rc |= bno055_write8(BNO055_MAG_OFFSET_Z_LSB_ADDR, (offsets->bso_mag_off_z) & 0x0FF);
    rc |= bno055_write8(BNO055_MAG_OFFSET_Z_MSB_ADDR, (offsets->bso_mag_off_z >> 8) & 0x0FF);

    rc |= bno055_write8(BNO055_ACCEL_RADIUS_LSB_ADDR, (offsets->bso_acc_radius) & 0x0FF);
    rc |= bno055_write8(BNO055_ACCEL_RADIUS_MSB_ADDR, (offsets->bso_acc_radius >> 8) & 0x0FF);

    rc |= bno055_write8(BNO055_MAG_RADIUS_LSB_ADDR, (offsets->bso_mag_radius) & 0x0FF);
    rc |= bno055_write8(BNO055_MAG_RADIUS_MSB_ADDR, (offsets->bso_mag_radius >> 8) & 0x0FF);

    rc |= bno055_set_opr_mode(prev_mode);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

static void *
bno055_sensor_get_interface(struct sensor *sensor, sensor_type_t type)
{
    return (NULL);
}

static int
bno055_sensor_get_config(struct sensor *sensor, sensor_type_t type,
        struct sensor_cfg *cfg)
{
    int rc;

    if ((type != SENSOR_TYPE_ACCELEROMETER)   &&
        (type != SENSOR_TYPE_MAGNETIC_FIELD)  &&
        (type != SENSOR_TYPE_TEMPERATURE)     &&
        (type != SENSOR_TYPE_ROTATION_VECTOR) &&
        (type != SENSOR_TYPE_LINEAR_ACCEL)    &&
        (type != SENSOR_TYPE_GRAVITY)         &&
        (type != SENSOR_TYPE_EULER)) {
        rc = SYS_EINVAL;
        goto err;
    }

    if (type != SENSOR_TYPE_TEMPERATURE) {
        cfg->sc_valtype = SENSOR_VALUE_TYPE_FLOAT_TRIPLET;
    } else {
        cfg->sc_valtype = SENSOR_VALUE_TYPE_INT32;
    }

    return 0;
err:
    return rc;
}
