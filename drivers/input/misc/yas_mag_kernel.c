/*
 * Copyright (c) 2014 Yamaha Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/of_gpio.h>
#include <linux/sensors.h>
#include <linux/regulator/consumer.h>
#include <linux/sensors_ftm.h>

#include "yas.h"

#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532 \
		    || YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533
#define YAS_MSM_NAME		"compass"
#define YAS_MSM_VENDOR		"Yamaha"
#define YAS_MSM_VERSION		(1)
#define YAS_MSM_HANDLE		(1)
#define YAS_MSM_TYPE		(2)
#define YAS_MSM_MIN_DELAY	(10000)
#define YAS_MSM_MAX_RANGE	(1200)
#define YAS_MSM_RESOLUTION	"1"
#define YAS_MSM_SENSOR_POWER	"0.40"
#endif
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS537
#define YAS_MSM_NAME		"compass"
#define YAS_MSM_VENDOR		"Yamaha"
#define YAS_MSM_VERSION		(1)
#define YAS_MSM_HANDLE		(1)
#define YAS_MSM_TYPE		(2)
#define YAS_MSM_MIN_DELAY	(10000)
#define YAS_MSM_MAX_RANGE	(2000)
#define YAS_MSM_RESOLUTION	"1"
#define YAS_MSM_SENSOR_POWER	"0.28"
#endif

#define YAS533_VDD_MIN_UV  2000000
#define YAS533_VDD_MAX_UV  3300000
#define YAS533_VIO_MIN_UV  1750000
#define YAS533_VIO_MAX_UV  1950000

struct yas533_platform_data {
	int dir;
	int (*init)(void);
	void (*exit)(void);
	int (*power_on)(bool);
};

static struct i2c_client *this_client;

struct yas_state {
	struct mutex lock;
	struct yas_mag_driver mag;
	struct input_dev *input_dev;
	struct sensors_classdev cdev;
	struct delayed_work work;
	int32_t poll_delay;
	atomic_t enable;
	int32_t compass_data[3];
	struct device *dev;
	struct class *class;
	bool power_on;
	struct regulator *vdd;
	struct regulator *vio;
	struct yas533_platform_data *platform_data;
	struct i2c_client *client;
};
static struct yas_state *pdev_data;
static struct sensors_classdev sensors_cdev = {
	.name = "yas533-mag",
	.vendor = "Yamaha",
	.version = 1,
	.handle = SENSORS_MAGNETIC_FIELD_HANDLE,
	.type = SENSOR_TYPE_MAGNETIC_FIELD,
	.max_range = "1200",
	.resolution = "1",
	.sensor_power = "0.40",
	.min_delay = 10000,
	.fifo_reserved_event_count = 0,
	.fifo_max_event_count = 0,
	.enabled = 0,
	.delay_msec = 10000,
	.sensors_enable = NULL,
	.sensors_poll_delay = NULL,
};

static int yas_device_open(int32_t type)
{
	return 0;
}

static int yas_device_close(int32_t type)
{
	return 0;
}

static int yas_device_write(int32_t type, uint8_t addr, const uint8_t *buf,
		int len)
{
	uint8_t tmp[2];
	int error;

	if (sizeof(tmp) - 1 < len)
		return -EPERM;
	tmp[0] = addr;
	memcpy(&tmp[1], buf, len);
	error = i2c_master_send(this_client, tmp, len + 1);

	if (unlikely(error < 0)) {
		dev_err(&this_client->dev, "I2C send error: %d\n", error);
		return error;
	}
	return 0;
}

static int yas_device_read(int32_t type, uint8_t addr, uint8_t *buf, int len)
{
	struct i2c_msg msg[2];
	int err;
	msg[0].addr = this_client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &addr;
	msg[1].addr = this_client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = buf;
	err = i2c_transfer(this_client->adapter, msg, 2);
	if (err != 2) {
		dev_err(&this_client->dev,
				"i2c_transfer() read error: "
				"slave_addr=%02x, reg_addr=%02x, err=%d\n",
				this_client->addr, addr, err);
		return err;
	}
	return 0;
}

static void yas_usleep(int us)
{
	usleep_range(us, us + 1000);
}

static uint32_t yas_current_time(void)
{
	return jiffies_to_msecs(jiffies);
}

static int yas_enable(struct yas_state *st)
{
	struct yas533_platform_data *pdata;
	pdata = st->platform_data;
	if (pdata->power_on)
		pdata->power_on(true);

	if (!atomic_cmpxchg(&st->enable, 0, 1)) {
		mutex_lock(&st->lock);
		st->mag.set_enable(1);
		mutex_unlock(&st->lock);
		schedule_delayed_work(&st->work, 0);
	}
	return 0;
}

static int yas_disable(struct yas_state *st)
{
	struct yas533_platform_data *pdata;
	pdata = st->platform_data;
	if (atomic_cmpxchg(&st->enable, 1, 0)) {
		cancel_delayed_work_sync(&st->work);
		mutex_lock(&st->lock);
		st->mag.set_enable(0);
		mutex_unlock(&st->lock);
	}

	if (pdata->power_on)
		pdata->power_on(false);

	return 0;
}

/* Sysfs interface */

static ssize_t yas_position_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	int ret;
	mutex_lock(&st->lock);
	ret = st->mag.get_position();
	printk("%s, mag.get_position = %d\n", __func__, ret);
	mutex_unlock(&st->lock);
	if (ret < 0)
		return -EFAULT;
	return snprintf(buf, PAGE_SIZE, "%d\n", ret);
}

static ssize_t yas_position_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	int ret, position;
	sscanf(buf, "%d\n", &position);
	mutex_lock(&st->lock);
	ret = st->mag.set_position(position);
	printk("%s, mag.set_position = %d\n", __func__, ret);
	mutex_unlock(&st->lock);
	if (ret < 0)
		return -EFAULT;
	return count;
}
static int yas_enable_set(struct sensors_classdev *sensors_cdev,
		unsigned int enable)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	if (enable)
		yas_enable(st);
	else
		yas_disable(st);
	return 0;
}

static int yas_poll_delay_set(struct sensors_classdev *sensors_cdev,
		unsigned int delay_ms)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	if (delay_ms <= 0)
		delay_ms = 10;
	mutex_lock(&st->lock);
	if (st->mag.set_delay(delay_ms) == YAS_NO_ERROR)
		st->poll_delay = delay_ms;
	mutex_unlock(&st->lock);

	return 0;

}


#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532 \
		    || YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533 \
	|| YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS537
static ssize_t yas_hard_offset_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	int8_t hard_offset[3];
	int ret;
	mutex_lock(&st->lock);
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532 \
	|| YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533
	ret = st->mag.ext(YAS532_GET_HW_OFFSET, hard_offset);
#endif
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS537
	ret = st->mag.ext(YAS537_GET_HW_OFFSET, hard_offset);
#endif
	mutex_unlock(&st->lock);
	if (ret < 0)
		return -EFAULT;
	return snprintf(buf, PAGE_SIZE, "%d %d %d\n",
			hard_offset[0], hard_offset[1], hard_offset[2]);
}
#endif

#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532 \
	|| YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533
static ssize_t yas_hard_offset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	int32_t tmp[3];
	int8_t hard_offset[3];
	int ret, i;
	sscanf(buf, "%d %d %d\n", &tmp[0], &tmp[1], &tmp[2]);
	for (i = 0; i < 3; i++)
		hard_offset[i] = (int8_t)tmp[i];
	mutex_lock(&st->lock);
	ret = st->mag.ext(YAS532_SET_HW_OFFSET, hard_offset);
	mutex_unlock(&st->lock);
	if (ret < 0)
		return -EFAULT;
	return count;
}
#endif

#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS537


static ssize_t yas_mag_average_sample_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	int8_t mag_average_sample;
	int ret;
	mutex_lock(&st->lock);
	ret = st->mag.ext(YAS537_GET_AVERAGE_SAMPLE, &mag_average_sample);
	mutex_unlock(&st->lock);
	if (ret < 0)
		return -EFAULT;
	return snprintf(buf, PAGE_SIZE, "%d\n", mag_average_sample);
}

static ssize_t yas_mag_average_sample_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	int32_t tmp;
	int8_t mag_average_sample;
	int ret;
	sscanf(buf, "%d\n", &tmp);
	mag_average_sample = (int8_t)tmp;
	mutex_lock(&st->lock);
	ret = st->mag.ext(YAS537_SET_AVERAGE_SAMPLE, &mag_average_sample);
	mutex_unlock(&st->lock);
	if (ret < 0)
		return -EFAULT;
	return count;
}
#endif

static ssize_t yas_data_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	int32_t last[3], i;
	mutex_lock(&st->lock);
	for (i = 0; i < 3; i++)
		last[i] = st->compass_data[i];
	mutex_unlock(&st->lock);
	return snprintf(buf, PAGE_SIZE, "%d %d %d\n",
			last[0], last[1], last[2]);
}


static DEVICE_ATTR(data, S_IRUGO, yas_data_show, NULL);
static DEVICE_ATTR(position, S_IRUGO|S_IWUSR, yas_position_show,
		yas_position_store);
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532 \
		    || YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533
static DEVICE_ATTR(hard_offset, S_IRUGO|S_IWUSR, yas_hard_offset_show,
		yas_hard_offset_store);
#endif
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532 \
		    || YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533 \
|| YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS537

#endif
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS537
static DEVICE_ATTR(hard_offset, S_IRUGO|S_IWUSR, yas_hard_offset_show, NULL);
static DEVICE_ATTR(mag_average_sample, S_IRUGO|S_IWUSR,
		yas_mag_average_sample_show, yas_mag_average_sample_store);
#endif

static struct attribute *yas_attributes[] = {

	&dev_attr_data.attr,
	&dev_attr_position.attr,
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532 \
	|| YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533 \
	|| YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS537
	&dev_attr_hard_offset.attr,

#endif
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS537
	&dev_attr_mag_average_sample.attr,
#endif
	NULL
};
static struct attribute_group yas_attribute_group = {
	.attrs = yas_attributes
};

static void yas_work_func(struct work_struct *work)
{
	struct yas_state *st
		= container_of((struct delayed_work *)work,
				struct yas_state, work);
	struct yas_data mag[1];
	int32_t poll_delay;
	uint32_t time_before, time_after;
	int ret, i;

	if(atomic_read(&st->enable) == 0)
		return;

	time_before = yas_current_time();
	mutex_lock(&st->lock);
	ret = st->mag.measure(mag, 1);
	if (ret == 1) {
		for (i = 0; i < 3; i++)
			st->compass_data[i] = mag[0].xyz.v[i];
	}
	poll_delay = st->poll_delay;
	mutex_unlock(&st->lock);
	if (ret == 1) {
		/* report magnetic data in [nT] */
		input_report_abs(st->input_dev, ABS_X, mag[0].xyz.v[0]);
		input_report_abs(st->input_dev, ABS_Y, mag[0].xyz.v[1]);
		input_report_abs(st->input_dev, ABS_Z, mag[0].xyz.v[2]);
		input_sync(st->input_dev);
	}
	time_after = yas_current_time();
	poll_delay = poll_delay - (time_after - time_before);
	if (poll_delay <= 0)
		poll_delay = 1;
	schedule_delayed_work(&st->work, msecs_to_jiffies(poll_delay));
}

/*****************regulator configuration start**************/
static int sensor_regulator_configure(struct yas_state *data, bool on)
{
	int rc;

	if (!on) {

		if (regulator_count_voltages(data->vdd) > 0)
			regulator_set_voltage(data->vdd, 0,
					YAS533_VDD_MAX_UV);

		regulator_put(data->vdd);

		if (regulator_count_voltages(data->vio) > 0)
			regulator_set_voltage(data->vio, 0,
					YAS533_VIO_MAX_UV);

		regulator_put(data->vio);
	} else {
		data->vdd = regulator_get(&data->client->dev, "vdd");
		if (IS_ERR(data->vdd)) {
			rc = PTR_ERR(data->vdd);
			dev_err(&data->client->dev,
					"Regulator get failed vdd rc=%d\n", rc);
			return rc;
		}

		if (regulator_count_voltages(data->vdd) > 0) {
			rc = regulator_set_voltage(data->vdd,
					YAS533_VDD_MIN_UV, YAS533_VDD_MAX_UV);
			if (rc) {
				dev_err(&data->client->dev,
						"Regulator set failed vdd rc=%d\n",
						rc);
				goto reg_vdd_put;
			}
		}

		data->vio = regulator_get(&data->client->dev, "vio");
		if (IS_ERR(data->vio)) {
			rc = PTR_ERR(data->vio);
			dev_err(&data->client->dev,
					"Regulator get failed vio rc=%d\n", rc);
			goto reg_vdd_set;
		}

		if (regulator_count_voltages(data->vio) > 0) {
			rc = regulator_set_voltage(data->vio,
					YAS533_VIO_MIN_UV, YAS533_VIO_MAX_UV);
			if (rc) {
				dev_err(&data->client->dev,
						"Regulator set failed vio rc=%d\n", rc);
				goto reg_vio_put;
			}
		}
	}

	return 0;
reg_vio_put:
	regulator_put(data->vio);

reg_vdd_set:
	if (regulator_count_voltages(data->vdd) > 0)
		regulator_set_voltage(data->vdd, 0, YAS533_VDD_MAX_UV);
reg_vdd_put:
	regulator_put(data->vdd);
	return rc;
}

static int sensor_regulator_power_on(struct yas_state *data, bool on)
{
	int rc = 0;

	if (!on) {
		rc = regulator_disable(data->vdd);
		if (rc) {
			dev_err(&data->client->dev,
					"Regulator vdd disable failed rc=%d\n", rc);
			return rc;
		}

		rc = regulator_disable(data->vio);
		if (rc) {
			dev_err(&data->client->dev,
					"Regulator vio disable failed rc=%d\n", rc);
			rc = regulator_enable(data->vdd);
			dev_err(&data->client->dev,
					"Regulator vio re-enabled rc=%d\n", rc);
			/*
			 * Successfully re-enable regulator.
			 * Enter poweron delay and returns error.
			 */
			if (!rc) {
				rc = -EBUSY;
				goto enable_delay;
			}
		}
		return rc;
	} else {
		rc = regulator_enable(data->vdd);
		if (rc) {
			dev_err(&data->client->dev,
					"Regulator vdd enable failed rc=%d\n", rc);
			return rc;
		}

		rc = regulator_enable(data->vio);
		if (rc) {
			dev_err(&data->client->dev,
					"Regulator vio enable failed rc=%d\n", rc);
			regulator_disable(data->vdd);
			return rc;
		}
	}

enable_delay:
	msleep(130);
	dev_dbg(&data->client->dev,
			"Sensor regulator power on =%d\n", on);
	return rc;
}

static int sensor_platform_hw_power_on(bool on)
{
	struct yas_state *data;
	int err = 0;

	/* pdev_data is global pointer to struct yas_state */
	if (pdev_data == NULL)
		return -ENODEV;
	data = pdev_data;

	if (data->power_on != on) {

		err = sensor_regulator_power_on(data, on);
		if (err)
			dev_err(&data->client->dev,
					"Can't configure regulator!\n");
		else
			data->power_on = on;
	}

	return err;
}

static int sensor_platform_hw_init(void)
{
	struct i2c_client *client;
	struct yas_state *data;
	int error;

	if (pdev_data == NULL)
		return -ENODEV;

	data = pdev_data;
	client = data->client;

	error = sensor_regulator_configure(data, true);
	if (error < 0) {
		dev_err(&client->dev, "unable to configure regulator\n");
		return error;
	}

	return 0;
}

static void sensor_platform_hw_exit(void)
{
	struct yas_state *data = pdev_data;

	if (data == NULL)
		return;

	sensor_regulator_configure(data, false);
}

static int sensor_parse_dt(struct device *dev,
		struct yas533_platform_data *pdata)
{
	int rc;
	u32 temp_val;

	struct device_node *np = dev->of_node;

	rc = of_property_read_u32(np, "yamaha,dir", &temp_val);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Unable to read yamaha dir\n");
		return rc;
	} else {
		pdata->dir = (u8)temp_val;
	}

	pdata->init = sensor_platform_hw_init;
	pdata->exit = sensor_platform_hw_exit;
	pdata->power_on = sensor_platform_hw_power_on;

	return 0;
}
/******************regulator ends***********************/

static ssize_t yas_mag_selftest_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct yas_state *st = i2c_get_clientdata(this_client);
	struct yas532_self_test_result sample;
	int test_result;
	int32_t ret;

	mutex_lock(&st->lock);
	test_result = st->mag.ext(YAS532_SELF_TEST, &sample);

	printk(KERN_ERR"%s, id = %d, xy1y2[0] = %d, xy1y2[1] = %d,xy1y2[2] = %d,dir = %d,sx = %d,sy = %d,xyz[0] = %d,xyz[1] = %d,xyz[2] = %d\n",
			__func__, sample.id, sample.xy1y2[0],sample.xy1y2[1],sample.xy1y2[2],sample.dir,sample.sx,sample.sy,sample.xyz[0],sample.xyz[1],sample.xyz[2]);

	if(test_result < 0)
		ret = 0;    // test fail
	else
		ret = 1;    // test success

	mutex_unlock(&st->lock);

	printk("%s over. test_result = %d, ret = %d\n", __func__, test_result, ret);    

	return snprintf(buf, PAGE_SIZE, "%d\n", ret);
}

static struct kobj_attribute test = 
{
	.attr = {"test", 0444},
	.show = yas_mag_selftest_show,
};

static const struct attribute *yas_mag_ftm_attrs[] = 
{
	&test.attr,
	NULL
};

static struct dev_ftm yas_mag_ftm;

static int yas_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
	struct yas_state *st = NULL;
	struct input_dev *input_dev = NULL;
	int ret, i;
	struct yas533_platform_data *pdata;

	printk("%s start.", __func__);

	this_client = i2c;
	input_dev = input_allocate_device();
	if (input_dev == NULL) {
		ret = -ENOMEM;
		goto error_ret;
	}

	if (i2c->dev.of_node) {
		pdata = devm_kzalloc(&i2c->dev,
				sizeof(struct yas533_platform_data),
				GFP_KERNEL);
		if (!pdata) {
			dev_err(&i2c->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		i2c->dev.platform_data = pdata;
		ret = sensor_parse_dt(&i2c->dev, pdata);
		if (ret) {
			dev_err(&i2c->dev,
					"%s: sensor_parse_dt() err\n", __func__);
			return ret;
		}
	} else {
		pdata = i2c->dev.platform_data;
		if (!pdata) {
			dev_err(&i2c->dev, "No platform data\n");
			return -ENODEV;
		}
	}

	st = kzalloc(sizeof(struct yas_state), GFP_KERNEL);
	if (st == NULL) {
		ret = -ENOMEM;
		goto error_ret;
	}

	st->platform_data = pdata;
	st->client = i2c;
	pdev_data = st;

	i2c_set_clientdata(i2c, st);

	if (pdata->init)
	{   
		ret = pdata->init();
		if(ret)
		{
			dev_err(&i2c->dev,"pdata->init failed.\n");
			goto exit_kfree;
		}
	}

	if (pdata->power_on)
	{   
		ret = pdata->power_on(true);
		if(ret)
		{
			dev_err(&i2c->dev,"pdata->power_on failed.\n");
			goto exit_pdata_power_on;
		}        
	}

	input_dev->name = YAS_MSM_NAME;
	input_dev->dev.parent = &i2c->dev;
	input_dev->id.bustype = BUS_I2C;
	set_bit(EV_ABS, input_dev->evbit);
	input_set_abs_params(input_dev, ABS_X, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_Z, INT_MIN, INT_MAX, 0, 0);

	input_set_drvdata(input_dev, st);
	atomic_set(&st->enable, 0);
	st->input_dev = input_dev;
	st->poll_delay = YAS_DEFAULT_SENSOR_DELAY;
	st->mag.callback.device_open = yas_device_open;
	st->mag.callback.device_close = yas_device_close;
	st->mag.callback.device_write = yas_device_write;
	st->mag.callback.device_read = yas_device_read;
	st->mag.callback.usleep = yas_usleep;
	st->mag.callback.current_time = yas_current_time;
	INIT_DELAYED_WORK(&st->work, yas_work_func);
	mutex_init(&st->lock);

	for (i = 0; i < 3; i++)
		st->compass_data[i] = 0;

	ret = input_register_device(input_dev);
	if (ret)
		goto error_free_device;

	st->cdev = sensors_cdev;
	st->cdev.sensors_enable = yas_enable_set;
	st->cdev.sensors_poll_delay = yas_poll_delay_set;

	ret = sensors_classdev_register(&i2c->dev, &st->cdev);
	if (ret) {
		dev_err(&i2c->dev, "class device create failed: %d\n", ret);
		goto error_classdev_unregister;
	}

	st->dev = st->cdev.dev;

	ret = sysfs_create_group(&st->dev->kobj, &yas_attribute_group);
	if (ret)
		goto error_unregister_device;
	ret = yas_mag_driver_init(&st->mag);
	if (ret < 0) {
		ret = -EFAULT;
		goto error_remove_sysfs;
	}
	ret = st->mag.init();
	if (ret < 0) {
		ret = -EFAULT;
		goto error_remove_sysfs;
	}
	ret = st->mag.set_position(pdata->dir);
	if (ret < 0) {
		ret = -EFAULT;
		goto error_remove_sysfs;
	}

	if (pdata->power_on)
		pdata->power_on(false);

	yas_mag_ftm.name = "geomagnetic";
	yas_mag_ftm.i2c_client = st->client;
	yas_mag_ftm.priv_data = st;
	yas_mag_ftm.attrs = yas_mag_ftm_attrs;
	register_single_dev_ftm(&yas_mag_ftm);

	printk("%s ok.", __func__);

	return 0;

error_remove_sysfs:
	sysfs_remove_group(&st->dev->kobj, &yas_attribute_group);
error_unregister_device:
	sensors_classdev_unregister(&st->cdev);
error_classdev_unregister:
	input_unregister_device(input_dev);
error_free_device:
	if (pdata->power_on)
		pdata->power_on(false);
exit_pdata_power_on:
	if (pdata->exit)
		pdata->exit();
exit_kfree:
	kfree(st);
error_ret:
	//i2c_set_clientdata(i2c, NULL);
	this_client = NULL;
	return ret;
}

static int yas_remove(struct i2c_client *i2c)
{
	struct yas_state *st = i2c_get_clientdata(i2c);
	if (st != NULL) {
		yas_disable(st);
		st->mag.term();
		sysfs_remove_group(&st->dev->kobj,
				&yas_attribute_group);
		input_unregister_device(st->input_dev);
		input_free_device(st->input_dev);
		device_unregister(st->dev);
		class_destroy(st->class);
		kfree(st);
		this_client = NULL;
	}
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int yas_suspend(struct device *dev)
{
	struct yas533_platform_data *pdata;
	pdata = pdev_data->platform_data;
	if (atomic_read(&pdev_data->enable)) {
		cancel_delayed_work_sync(&pdev_data->work);
		pdev_data->mag.set_enable(0);
	}

	if (pdata->power_on)
		pdata->power_on(false);

	return 0;
}

static int yas_resume(struct device *dev)
{
	struct yas533_platform_data *pdata;
	pdata = pdev_data->platform_data;
	if (atomic_read(&pdev_data->enable)) {
		pdev_data->mag.set_enable(1);
		schedule_delayed_work(&pdev_data->work, 0);
	}

	if (pdata->power_on)
		pdata->power_on(true);

	return 0;
}

static SIMPLE_DEV_PM_OPS(yas_pm_ops, yas_suspend, yas_resume);
#define YAS_PM_OPS (&yas_pm_ops)
#else
#define YAS_PM_OPS NULL
#endif

static const struct i2c_device_id yas_id[] = {
	{YAS_MSM_NAME, 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, yas_id);


static struct of_device_id yas_match_table[] = {
	{ .compatible = "yamaha,yas533", },
	{ },
};

static struct i2c_driver yas_driver = {
	.driver = {
		.name	= YAS_MSM_NAME,
		.owner	= THIS_MODULE,
		.pm	= YAS_PM_OPS,
		.of_match_table = yas_match_table,
	},
	.probe		= yas_probe,
	.remove		= yas_remove,
	.id_table	= yas_id,
};
static int __init yas_driver_init(void)
{
	return i2c_add_driver(&yas_driver);
}

static void __exit yas_driver_exit(void)
{
	i2c_del_driver(&yas_driver);
}

module_init(yas_driver_init);
module_exit(yas_driver_exit);

MODULE_DESCRIPTION("Yamaha Magnetometer I2C driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.6.2.1020d");
