/*
    mpu-dev.c - mpu3050 char device interface

    Copyright (C) 1995-97 Simon G. Vogl
    Copyright (C) 1998-99 Frodo Looijaard <frodol@dds.nl>
    Copyright (C) 2003 Greg Kroah-Hartman <greg@kroah.com>
    Copyright (C) 2010 InvenSense Corporation, All Rights Reserved.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/* Code inside mpudev_ioctl_rdrw is copied from i2c-dev.c
 */
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/signal.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/pm.h>
#include <linux/of_irq.h>
#include <linux/of_i2c.h>

#ifdef CONFIG_PM_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/io.h>

#include "mpuirq.h"
#include "slaveirq.h"
#include "mlsl.h"
#include "mpu-i2c.h"
#include "mldl_cfg.h"
#include "mpu.h"

#define MPU3050_EARLY_SUSPEND_IN_DRIVER 0

#if defined CONFIG_MPU_SENSORS_YAS529
#define COMPASS_AKM8975_SENSOR_ID  0x48
#define COMPASS_YAS529_SENSOR_ID   0x40
#define COMPASS_SLAVE_ID           0x0C

#define COMPASS_ADAPTER_NUMBER  4
#define COMPASS_RETRY_CNT       3

int compass_retry_num = 0;
signed char YAS529_orientation[9] = {
	 0, -1,  0,
	-1,  0,  0,
	 0,  0, -1
};

extern struct ext_slave_descr *yas529_get_slave_descr(void);
#endif

/* Platform data for the MPU */
struct mpu_private_data {
	struct mldl_cfg mldl_cfg;

#ifdef CONFIG_PM_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

static int pid;

static struct i2c_client *this_client;

static int mpu_open(struct inode *inode, struct file *file)
{
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *) i2c_get_clientdata(this_client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;

	dev_dbg(&this_client->adapter->dev, "mpu_open\n");
	dev_dbg(&this_client->adapter->dev, "current->pid %d\n",
		current->pid);
	pid = current->pid;
	file->private_data = this_client;
	/* we could do some checking on the flags supplied by "open" */
	/* i.e. O_NONBLOCK */
	/* -> set some flag to disable interruptible_sleep_on in mpu_read */

	/* Reset the sensors to the default */
	mldl_cfg->requested_sensors = ML_THREE_AXIS_GYRO;
	if (mldl_cfg->accel && mldl_cfg->accel->resume)
		mldl_cfg->requested_sensors |= ML_THREE_AXIS_ACCEL;

	if (mldl_cfg->compass && mldl_cfg->compass->resume)
		mldl_cfg->requested_sensors |= ML_THREE_AXIS_COMPASS;

	if (mldl_cfg->pressure && mldl_cfg->pressure->resume)
		mldl_cfg->requested_sensors |= ML_THREE_AXIS_PRESSURE;

	return 0;
}

/* close function - called when the "file" /dev/mpu is closed in userspace   */
static int mpu_release(struct inode *inode, struct file *file)
{
	struct i2c_client *client =
	    (struct i2c_client *) file->private_data;
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *) i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;
	int result = 0;

	pid = 0;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);
	result = mpu3050_suspend(mldl_cfg, client->adapter,
				 accel_adapter, compass_adapter,
				 pressure_adapter,
				 TRUE, TRUE, TRUE, TRUE);

	dev_dbg(&this_client->adapter->dev, "mpu_release\n");
	return result;
}

static noinline int mpudev_ioctl_rdrw(struct i2c_client *client,
				      unsigned long arg)
{
	struct i2c_rdwr_ioctl_data rdwr_arg;
	struct i2c_msg *rdwr_pa;
	u8 __user **data_ptrs;
	int i, res;

	if (copy_from_user(&rdwr_arg,
			   (struct i2c_rdwr_ioctl_data __user *) arg,
			   sizeof(rdwr_arg)))
		return -EFAULT;

	/* Put an arbitrary limit on the number of messages that can
	 * be sent at once */
	if (rdwr_arg.nmsgs > I2C_RDRW_IOCTL_MAX_MSGS)
		return -EINVAL;

	rdwr_pa = (struct i2c_msg *)
	    kmalloc(rdwr_arg.nmsgs * sizeof(struct i2c_msg), GFP_KERNEL);
	if (!rdwr_pa)
		return -ENOMEM;

	if (copy_from_user(rdwr_pa, rdwr_arg.msgs,
			   rdwr_arg.nmsgs * sizeof(struct i2c_msg))) {
		kfree(rdwr_pa);
		return -EFAULT;
	}

	data_ptrs =
	    kmalloc(rdwr_arg.nmsgs * sizeof(u8 __user *), GFP_KERNEL);
	if (data_ptrs == NULL) {
		kfree(rdwr_pa);
		return -ENOMEM;
	}

	res = 0;
	for (i = 0; i < rdwr_arg.nmsgs; i++) {
		/* Limit the size of the message to a sane amount;
		 * and don't let length change either. */
		if ((rdwr_pa[i].len > 8192) ||
		    (rdwr_pa[i].flags & I2C_M_RECV_LEN)) {
			res = -EINVAL;
			break;
		}
		data_ptrs[i] = (u8 __user *) rdwr_pa[i].buf;
		rdwr_pa[i].buf = kmalloc(rdwr_pa[i].len, GFP_KERNEL);
		if (rdwr_pa[i].buf == NULL) {
			res = -ENOMEM;
			break;
		}
		if (copy_from_user(rdwr_pa[i].buf, data_ptrs[i],
				   rdwr_pa[i].len)) {
			++i;	/* Needs to be kfreed too */
			res = -EFAULT;
			break;
		}
	}
	if (res < 0) {
		int j;
		for (j = 0; j < i; ++j)
			kfree(rdwr_pa[j].buf);
		kfree(data_ptrs);
		kfree(rdwr_pa);
		return res;
	}

	res = i2c_transfer(client->adapter, rdwr_pa, rdwr_arg.nmsgs);
	while (i-- > 0) {
		if (res >= 0 && (rdwr_pa[i].flags & I2C_M_RD)) {
			if (copy_to_user(data_ptrs[i], rdwr_pa[i].buf,
					 rdwr_pa[i].len))
				res = -EFAULT;
		}
		kfree(rdwr_pa[i].buf);
	}
	kfree(data_ptrs);
	kfree(rdwr_pa);
	return res;
}

/* read function called when from /dev/mpu is read.  Read from the FIFO */
static ssize_t mpu_read(struct file *file,
			char __user *buf, size_t count, loff_t *offset)
{
	char *tmp;
	int ret;

	struct i2c_client *client =
	    (struct i2c_client *) file->private_data;

	if (count > 8192)
		count = 8192;

	tmp = kmalloc(count, GFP_KERNEL);
	if (tmp == NULL)
		return -ENOMEM;

	pr_debug("i2c-dev: i2c-%d reading %zu bytes.\n",
		 iminor(file->f_path.dentry->d_inode), count);

/* @todo fix this to do a i2c trasnfer from the FIFO */
	ret = i2c_master_recv(client, tmp, count);
	if (ret >= 0) {
		ret = copy_to_user(buf, tmp, count) ? -EFAULT : ret;
		if (ret)
			ret = -EFAULT;
	}
	kfree(tmp);
	return ret;
}

static int
mpu_ioctl_set_mpu_pdata(struct i2c_client *client, unsigned long arg)
{
	int ii;
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *) i2c_get_clientdata(client);
	struct mpu3050_platform_data *pdata = mpu->mldl_cfg.pdata;
	struct mpu3050_platform_data local_pdata;

	if (copy_from_user(&local_pdata, (unsigned char __user *) arg,
				sizeof(local_pdata)))
		return -EFAULT;

	pdata->int_config = local_pdata.int_config;
	for (ii = 0; ii < DIM(pdata->orientation); ii++)
		pdata->orientation[ii] = local_pdata.orientation[ii];
	pdata->level_shifter = local_pdata.level_shifter;

	pdata->accel.address = local_pdata.accel.address;
	for (ii = 0; ii < DIM(pdata->accel.orientation); ii++)
		pdata->accel.orientation[ii] =
			local_pdata.accel.orientation[ii];

	pdata->compass.address = local_pdata.compass.address;
	for (ii = 0; ii < DIM(pdata->compass.orientation); ii++)
		pdata->compass.orientation[ii] =
			local_pdata.compass.orientation[ii];

	pdata->pressure.address = local_pdata.pressure.address;
	for (ii = 0; ii < DIM(pdata->pressure.orientation); ii++)
		pdata->pressure.orientation[ii] =
			local_pdata.pressure.orientation[ii];

	dev_dbg(&client->adapter->dev, "%s\n", __func__);

	return ML_SUCCESS;
}

static int
mpu_ioctl_set_mpu_config(struct i2c_client *client, unsigned long arg)
{
	int ii;
	int result = ML_SUCCESS;
	struct mpu_private_data *mpu =
		(struct mpu_private_data *) i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct mldl_cfg *temp_mldl_cfg;

	dev_dbg(&this_client->adapter->dev, "%s\n", __func__);

	temp_mldl_cfg = kzalloc(sizeof(struct mldl_cfg), GFP_KERNEL);
	if (NULL == temp_mldl_cfg)
		return -ENOMEM;

	/*
	 * User space is not allowed to modify accel compass pressure or
	 * pdata structs, as well as silicon_revision product_id or trim
	 */
	if (copy_from_user(temp_mldl_cfg, (struct mldl_cfg __user *) arg,
				offsetof(struct mldl_cfg, silicon_revision))) {
		result = -EFAULT;
		goto out;
	}

	if (mldl_cfg->gyro_is_suspended) {
		if (mldl_cfg->addr != temp_mldl_cfg->addr)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->int_config != temp_mldl_cfg->int_config)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->ext_sync != temp_mldl_cfg->ext_sync)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->full_scale != temp_mldl_cfg->full_scale)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->lpf != temp_mldl_cfg->lpf)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->clk_src != temp_mldl_cfg->clk_src)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->divider != temp_mldl_cfg->divider)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->dmp_enable != temp_mldl_cfg->dmp_enable)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->fifo_enable != temp_mldl_cfg->fifo_enable)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->dmp_cfg1 != temp_mldl_cfg->dmp_cfg1)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->dmp_cfg2 != temp_mldl_cfg->dmp_cfg2)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->gyro_power != temp_mldl_cfg->gyro_power)
			mldl_cfg->gyro_needs_reset = TRUE;

		for (ii = 0; ii < MPU_NUM_AXES; ii++)
			if (mldl_cfg->offset_tc[ii] !=
			    temp_mldl_cfg->offset_tc[ii])
				mldl_cfg->gyro_needs_reset = TRUE;

		for (ii = 0; ii < MPU_NUM_AXES; ii++)
			if (mldl_cfg->offset[ii] != temp_mldl_cfg->offset[ii])
				mldl_cfg->gyro_needs_reset = TRUE;

		if (memcmp(mldl_cfg->ram, temp_mldl_cfg->ram,
				MPU_MEM_NUM_RAM_BANKS * MPU_MEM_BANK_SIZE *
				sizeof(unsigned char)))
			mldl_cfg->gyro_needs_reset = TRUE;
	}

	memcpy(mldl_cfg, temp_mldl_cfg,
		offsetof(struct mldl_cfg, silicon_revision));

out:
	kfree(temp_mldl_cfg);
	return result;
}

static int
mpu_ioctl_get_mpu_config(struct i2c_client *client, unsigned long arg)
{
	/* Have to be careful as there are 3 pointers in the mldl_cfg
	 * structure */
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *) i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct mldl_cfg *local_mldl_cfg;
	int retval = 0;

	local_mldl_cfg = kzalloc(sizeof(struct mldl_cfg), GFP_KERNEL);
	if (NULL == local_mldl_cfg)
		return -ENOMEM;

	retval =
	    copy_from_user(local_mldl_cfg, (struct mldl_cfg __user *) arg,
			   sizeof(struct mldl_cfg));
	if (retval) {
		dev_err(&this_client->adapter->dev,
			"%s|%s:%d: EFAULT on arg\n",
			__FILE__, __func__, __LINE__);
		retval = -EFAULT;
		goto out;
	}

	/* Fill in the accel, compass, pressure and pdata pointers */
	if (mldl_cfg->accel) {
		retval = copy_to_user((void __user *)local_mldl_cfg->accel,
				      mldl_cfg->accel,
				      sizeof(*mldl_cfg->accel));
		if (retval) {
			dev_err(&this_client->adapter->dev,
				"%s|%s:%d: EFAULT on accel\n",
				__FILE__, __func__, __LINE__);
			retval = -EFAULT;
			goto out;
		}
	}

	if (mldl_cfg->compass) {
		retval = copy_to_user((void __user *)local_mldl_cfg->compass,
				      mldl_cfg->compass,
				      sizeof(*mldl_cfg->compass));
		if (retval) {
			dev_err(&this_client->adapter->dev,
				"%s|%s:%d: EFAULT on compass\n",
				__FILE__, __func__, __LINE__);
			retval = -EFAULT;
			goto out;
		}
	}

	if (mldl_cfg->pressure) {
		retval = copy_to_user((void __user *)local_mldl_cfg->pressure,
				      mldl_cfg->pressure,
				      sizeof(*mldl_cfg->pressure));
		if (retval) {
			dev_err(&this_client->adapter->dev,
				"%s|%s:%d: EFAULT on pressure\n",
				__FILE__, __func__, __LINE__);
			retval = -EFAULT;
			goto out;
		}
	}

	if (mldl_cfg->pdata) {
		retval = copy_to_user((void __user *)local_mldl_cfg->pdata,
				      mldl_cfg->pdata,
				      sizeof(*mldl_cfg->pdata));
		if (retval) {
			dev_err(&this_client->adapter->dev,
				"%s|%s:%d: EFAULT on pdata\n",
				__FILE__, __func__, __LINE__);
			retval = -EFAULT;
			goto out;
		}
	}

	/* Do not modify the accel, compass, pressure and pdata pointers */
	retval = copy_to_user((struct mldl_cfg __user *) arg,
			      mldl_cfg, offsetof(struct mldl_cfg, accel));

	if (retval)
		retval = -EFAULT;
out:
	kfree(local_mldl_cfg);
	return retval;
}

/**
 * Pass a requested slave configuration to the slave sensor
 *
 * @param adapter the adaptor to use to communicate with the slave
 * @param mldl_cfg the mldl configuration structuer
 * @param slave pointer to the slave descriptor
 * @param usr_config The configuration to pass to the slave sensor
 *
 * @return 0 or non-zero error code
 */
static int slave_config(void *adapter,
			struct mldl_cfg *mldl_cfg,
			struct ext_slave_descr *slave,
			struct ext_slave_config __user *usr_config)
{
	int retval = ML_SUCCESS;
	if ((slave) && (slave->config)) {
		struct ext_slave_config config;
		retval = copy_from_user(
			&config,
			usr_config,
			sizeof(config));
		if (retval)
			return -EFAULT;

		if (config.len && config.data) {
			int *data;
			data = kzalloc(config.len, GFP_KERNEL);
			if (!data)
				return ML_ERROR_MEMORY_EXAUSTED;

			retval = copy_from_user(data,
						(void __user *)config.data,
						config.len);
			if (retval) {
				retval = -EFAULT;
				kfree(data);
				return retval;
			}
			config.data = data;
		}
		retval = slave->config(adapter,
				slave,
				&mldl_cfg->pdata->accel,
				&config);
		kfree(config.data);
	}
	return retval;
}

/**
 * Get a requested slave configuration from the slave sensor
 *
 * @param adapter the adaptor to use to communicate with the slave
 * @param mldl_cfg the mldl configuration structuer
 * @param slave pointer to the slave descriptor
 * @param usr_config The configuration for the slave to fill out
 *
 * @return 0 or non-zero error code
 */
static int slave_get_config(void *adapter,
			struct mldl_cfg *mldl_cfg,
			struct ext_slave_descr *slave,
			struct ext_slave_config __user *usr_config)
{
	int retval = ML_SUCCESS;
	if ((slave) && (slave->get_config)) {
		struct ext_slave_config config;
		void *user_data;
		retval = copy_from_user(
			&config,
			usr_config,
			sizeof(config));
		if (retval)
			return -EFAULT;

		user_data = config.data;
		if (config.len && config.data) {
			int *data;
			data = kzalloc(config.len, GFP_KERNEL);
			if (!data)
				return ML_ERROR_MEMORY_EXAUSTED;

			retval = copy_from_user(data,
						(void __user *)config.data,
						config.len);
			if (retval) {
				retval = -EFAULT;
				kfree(data);
				return retval;
			}
			config.data = data;
		}
		retval = slave->get_config(adapter,
				slave,
				&mldl_cfg->pdata->accel,
				&config);
		if (retval) {
			kfree(config.data);
			return retval;
		}
		retval = copy_to_user((unsigned char __user *) user_data,
				      config.data,
				      config.len);
		kfree(config.data);
	}
	return retval;
}

/* ioctl - I/O control */
static long mpu_ioctl(struct file *file,
		      unsigned int cmd, unsigned long arg)
{
	struct i2c_client *client =
	    (struct i2c_client *) file->private_data;
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *) i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	int retval = 0;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	switch (cmd) {
	case I2C_RDWR:
		mpudev_ioctl_rdrw(client, arg);
		break;
	case I2C_SLAVE:
		if ((arg & 0x7E) != (client->addr & 0x7E)) {
			dev_err(&this_client->adapter->dev,
				"%s: Invalid I2C_SLAVE arg %lu\n",
				__func__, arg);
		}
		break;
	case MPU_SET_MPU_CONFIG:
		retval = mpu_ioctl_set_mpu_config(client, arg);
		break;
	case MPU_SET_INT_CONFIG:
		mldl_cfg->int_config = (unsigned char) arg;
		break;
	case MPU_SET_EXT_SYNC:
		mldl_cfg->ext_sync = (enum mpu_ext_sync) arg;
		break;
	case MPU_SET_FULL_SCALE:
		mldl_cfg->full_scale = (enum mpu_fullscale) arg;
		break;
	case MPU_SET_LPF:
		mldl_cfg->lpf = (enum mpu_filter) arg;
		break;
	case MPU_SET_CLK_SRC:
		mldl_cfg->clk_src = (enum mpu_clock_sel) arg;
		break;
	case MPU_SET_DIVIDER:
		mldl_cfg->divider = (unsigned char) arg;
		break;
	case MPU_SET_LEVEL_SHIFTER:
		mldl_cfg->pdata->level_shifter = (unsigned char) arg;
		break;
	case MPU_SET_DMP_ENABLE:
		mldl_cfg->dmp_enable = (unsigned char) arg;
		break;
	case MPU_SET_FIFO_ENABLE:
		mldl_cfg->fifo_enable = (unsigned char) arg;
		break;
	case MPU_SET_DMP_CFG1:
		mldl_cfg->dmp_cfg1 = (unsigned char) arg;
		break;
	case MPU_SET_DMP_CFG2:
		mldl_cfg->dmp_cfg2 = (unsigned char) arg;
		break;
	case MPU_SET_OFFSET_TC:
		retval = copy_from_user(mldl_cfg->offset_tc,
					(unsigned char __user *) arg,
					sizeof(mldl_cfg->offset_tc));
		if (retval)
			retval = -EFAULT;

		break;
	case MPU_SET_RAM:
		retval = copy_from_user(mldl_cfg->ram,
					(unsigned char __user *) arg,
					sizeof(mldl_cfg->ram));
		if (retval)
			retval = -EFAULT;
		break;
	case MPU_SET_PLATFORM_DATA:
		retval = mpu_ioctl_set_mpu_pdata(client, arg);
		break;
	case MPU_GET_MPU_CONFIG:
		retval = mpu_ioctl_get_mpu_config(client, arg);
		break;
	case MPU_GET_INT_CONFIG:
		retval = put_user(mldl_cfg->int_config,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_EXT_SYNC:
		retval = put_user(mldl_cfg->ext_sync,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_FULL_SCALE:
		retval = put_user(mldl_cfg->full_scale,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_LPF:
		retval = put_user(mldl_cfg->lpf,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_CLK_SRC:
		retval = put_user(mldl_cfg->clk_src,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_DIVIDER:
		retval = put_user(mldl_cfg->divider,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_LEVEL_SHIFTER:
		retval = put_user(mldl_cfg->pdata->level_shifter,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_DMP_ENABLE:
		retval = put_user(mldl_cfg->dmp_enable,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_FIFO_ENABLE:
		retval = put_user(mldl_cfg->fifo_enable,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_DMP_CFG1:
		retval = put_user(mldl_cfg->dmp_cfg1,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_DMP_CFG2:
		retval = put_user(mldl_cfg->dmp_cfg2,
				  (unsigned char __user *) arg);
		break;
	case MPU_GET_OFFSET_TC:
		retval = copy_to_user((unsigned char __user *) arg,
				      mldl_cfg->offset_tc,
				      sizeof(mldl_cfg->offset_tc));
		if (retval)
			retval = -EFAULT;
		break;
	case MPU_GET_RAM:
		retval = copy_to_user((unsigned char __user *) arg,
				      mldl_cfg->ram,
				      sizeof(mldl_cfg->ram));
		if (retval)
			retval = -EFAULT;
		break;
	case MPU_CONFIG_ACCEL:
		retval = slave_config(accel_adapter, mldl_cfg,
				mldl_cfg->accel,
				(struct ext_slave_config __user *) arg);
		break;
	case MPU_CONFIG_COMPASS:
		retval = slave_config(compass_adapter, mldl_cfg,
				mldl_cfg->compass,
				(struct ext_slave_config __user *) arg);
		break;
	case MPU_CONFIG_PRESSURE:
		retval = slave_config(pressure_adapter, mldl_cfg,
				mldl_cfg->pressure,
				(struct ext_slave_config __user *) arg);
		break;
	case MPU_GET_CONFIG_ACCEL:
		retval = slave_get_config(accel_adapter, mldl_cfg,
					mldl_cfg->accel,
					(struct ext_slave_config __user *) arg);
		break;
	case MPU_GET_CONFIG_COMPASS:
		retval = slave_get_config(compass_adapter, mldl_cfg,
					mldl_cfg->compass,
					(struct ext_slave_config __user *) arg);
		break;
	case MPU_GET_CONFIG_PRESSURE:
		retval = slave_get_config(pressure_adapter, mldl_cfg,
					mldl_cfg->pressure,
					(struct ext_slave_config __user *) arg);
		break;
	case MPU_SUSPEND:
	{
		unsigned long sensors;
		sensors = ~(mldl_cfg->requested_sensors);
		retval = mpu3050_suspend(mldl_cfg,
					client->adapter,
					accel_adapter,
					compass_adapter,
					pressure_adapter,
					((sensors & ML_THREE_AXIS_GYRO)
						== ML_THREE_AXIS_GYRO),
					((sensors & ML_THREE_AXIS_ACCEL)
						== ML_THREE_AXIS_ACCEL),
					((sensors & ML_THREE_AXIS_COMPASS)
						== ML_THREE_AXIS_COMPASS),
					((sensors & ML_THREE_AXIS_PRESSURE)
						== ML_THREE_AXIS_PRESSURE));
	}
	break;
	case MPU_RESUME:
	{
		unsigned long sensors;
		sensors = mldl_cfg->requested_sensors;
		retval = mpu3050_resume(mldl_cfg,
					client->adapter,
					accel_adapter,
					compass_adapter,
					pressure_adapter,
					sensors & ML_THREE_AXIS_GYRO,
					sensors & ML_THREE_AXIS_ACCEL,
					sensors & ML_THREE_AXIS_COMPASS,
					sensors & ML_THREE_AXIS_PRESSURE);
	}
	break;
	case MPU_READ_ACCEL:
	{
		unsigned char data[6];
		retval = mpu3050_read_accel(mldl_cfg, client->adapter,
					    data);
		if ((ML_SUCCESS == retval) &&
		    (copy_to_user((unsigned char __user *) arg,
			    data, sizeof(data))))
			retval = -EFAULT;
	}
	break;
	case MPU_READ_COMPASS:
	{
		unsigned char data[6];
		struct i2c_adapter *compass_adapt =
			i2c_get_adapter(mldl_cfg->pdata->compass.
					adapt_num);
		retval = mpu3050_read_compass(mldl_cfg, compass_adapt,
						 data);
		if ((ML_SUCCESS == retval) &&
			(copy_to_user((unsigned char *) arg,
				data, sizeof(data))))
			retval = -EFAULT;
	}
	break;
	case MPU_READ_PRESSURE:
	{
		unsigned char data[3];
		struct i2c_adapter *pressure_adapt =
			i2c_get_adapter(mldl_cfg->pdata->pressure.
					adapt_num);
		retval =
			mpu3050_read_pressure(mldl_cfg, pressure_adapt,
					data);
		if ((ML_SUCCESS == retval) &&
		    (copy_to_user((unsigned char __user *) arg,
			    data, sizeof(data))))
			retval = -EFAULT;
	}
	break;
	case MPU_READ_MEMORY:
	case MPU_WRITE_MEMORY:
	default:
		dev_err(&this_client->adapter->dev,
			"%s: Unknown cmd %d, arg %lu\n", __func__, cmd,
			arg);
		retval = -EINVAL;
	}

	return retval;
}

#ifdef CONFIG_PM_EARLYSUSPEND
void mpu3050_early_suspend(struct early_suspend *h)
{
	struct mpu_private_data *mpu = container_of(h,
						    struct
						    mpu_private_data,
						    early_suspend);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	dev_dbg(&this_client->adapter->dev, "%s: %d, %d\n", __func__,
		h->level, mpu->mldl_cfg.gyro_is_suspended);
	if (MPU3050_EARLY_SUSPEND_IN_DRIVER)
		(void) mpu3050_suspend(mldl_cfg, this_client->adapter,
				accel_adapter, compass_adapter,
				pressure_adapter, TRUE, TRUE, TRUE, TRUE);
}

void mpu3050_early_resume(struct early_suspend *h)
{
	struct mpu_private_data *mpu = container_of(h,
						    struct
						    mpu_private_data,
						    early_suspend);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	if (MPU3050_EARLY_SUSPEND_IN_DRIVER) {
		if (pid) {
			unsigned long sensors = mldl_cfg->requested_sensors;
			(void) mpu3050_resume(mldl_cfg,
					this_client->adapter,
					accel_adapter,
					compass_adapter,
					pressure_adapter,
					sensors & ML_THREE_AXIS_GYRO,
					sensors & ML_THREE_AXIS_ACCEL,
					sensors & ML_THREE_AXIS_COMPASS,
					sensors & ML_THREE_AXIS_PRESSURE);
			dev_dbg(&this_client->adapter->dev,
				"%s for pid %d\n", __func__, pid);
		}
	}
	dev_dbg(&this_client->adapter->dev, "%s: %d\n", __func__, h->level);
}
#endif

void mpu_shutdown(struct i2c_client *client)
{
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *) i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	(void) mpu3050_suspend(mldl_cfg, this_client->adapter,
			       accel_adapter, compass_adapter, pressure_adapter,
			       TRUE, TRUE, TRUE, TRUE);
	dev_dbg(&this_client->adapter->dev, "%s\n", __func__);
}

int mpu_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *) i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	if (!mpu->mldl_cfg.gyro_is_suspended) {
		dev_dbg(&this_client->adapter->dev,
			"%s: suspending on event %d\n", __func__,
			mesg.event);
		(void) mpu3050_suspend(mldl_cfg, this_client->adapter,
				       accel_adapter, compass_adapter,
				       pressure_adapter,
				       TRUE, TRUE, TRUE, TRUE);
	} else {
		dev_dbg(&this_client->adapter->dev,
			"%s: Already suspended %d\n", __func__,
			mesg.event);
	}
	return 0;
}

int mpu_resume(struct i2c_client *client)
{
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *) i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	if (pid) {
		unsigned long sensors = mldl_cfg->requested_sensors;
		(void) mpu3050_resume(mldl_cfg, this_client->adapter,
				      accel_adapter,
				      compass_adapter,
				      pressure_adapter,
				      sensors & ML_THREE_AXIS_GYRO,
				      sensors & ML_THREE_AXIS_ACCEL,
				      sensors & ML_THREE_AXIS_COMPASS,
				      sensors & ML_THREE_AXIS_PRESSURE);
		dev_dbg(&this_client->adapter->dev,
			"%s for pid %d\n", __func__, pid);
	}
	return 0;
}

/* define which file operations are supported */
static const struct file_operations mpu_fops = {
	.owner = THIS_MODULE,
	.read = mpu_read,
#if HAVE_COMPAT_IOCTL
	.compat_ioctl = mpu_ioctl,
#endif
#if HAVE_UNLOCKED_IOCTL
	.unlocked_ioctl = mpu_ioctl,
#endif
	.open = mpu_open,
	.release = mpu_release,
};

static unsigned short normal_i2c[] = { I2C_CLIENT_END };

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 32)
I2C_CLIENT_INSMOD;
#endif

static struct miscdevice i2c_mpu_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mpu", /* Same for both 3050 and 6000 */
	.fops = &mpu_fops,
};

#if defined CONFIG_MPU_SENSORS_YAS529
static int AKM_i2c_rxdata(struct i2c_adapter *i2c_adap, unsigned short saddr, unsigned char *rxdata, int length)
{
	struct i2c_msg msgs[] = {
		{
			.addr  = saddr,
			.flags = 0,
			.len   = 1,
			.buf   = rxdata,
		},
		{
			.addr  = saddr,
			.flags = I2C_M_RD,
			.len   = length,
			.buf   = rxdata,
		},
	};

	if (i2c_transfer(i2c_adap, &msgs[0], 1) < 0) {
		pr_err("failed, write slave address failed\n");
		return -EIO;
	}

	if (i2c_transfer(i2c_adap, &msgs[1], 1) < 0) {
		pr_err("failed, read data failed\n");
		return -EIO;
	}

	return 0;
}


static int32_t AKM_i2c_read_byte(struct i2c_adapter *i2c_adap, unsigned short saddr, unsigned char raddr, unsigned char *rdata)
{
	int32_t rc = 0;
	unsigned char buf[2];

	if (!rdata) {
		pr_err("buffer is NULL\n");
		return -EIO;
	}

	memset(buf, 0, sizeof(buf));
	buf[0] = raddr;

	rc = AKM_i2c_rxdata(i2c_adap, saddr, buf, 1);
	if (rc) {
		pr_err("failed, raddr = 0x%02x\n", raddr);
		return -EIO;
	}
	*rdata = buf[0];

	return rc;
}

static int mpu_sensor_i2c_write(struct i2c_adapter *i2c_adap,
				   unsigned char address,
				   unsigned int len, unsigned char *data)
{
	struct i2c_msg msgs[1];
	int res;

	if (NULL == data || NULL == i2c_adap)
		return -EINVAL;

	msgs[0].addr = address;
	msgs[0].flags = 0;	/* write */
	msgs[0].buf = (unsigned char *) data;
	msgs[0].len = len;

	res = i2c_transfer(i2c_adap, msgs, 1);
	if (res < 1)
		return res;
	else
		return 0;
}

static int mpu_sensor_i2c_read(struct i2c_adapter *i2c_adap,
				  unsigned char address,
				  unsigned char reg,
				  unsigned int len, unsigned char *data)
{
	struct i2c_msg msgs[2];
	int res;

	if (NULL == data || NULL == i2c_adap)
		return -EINVAL;

	msgs[0].addr = address;
	msgs[0].flags = I2C_M_RD;
	msgs[0].buf = data;
	msgs[0].len = len;

	res = i2c_transfer(i2c_adap, msgs, 1);
	if (res < 1)
		return res;
	else
		return 0;
}
#endif

static void mpu3050_dt_parse_slave_pdata(struct i2c_client *client,
					 struct device_node *of_node,
					 struct ext_slave_platform_data *slave_pdata)
{
	struct resource r_irq;
	u32 val;

	if (of_irq_to_resource(of_node, 0, &r_irq))
		slave_pdata->irq = r_irq.start;

	if (of_find_property(of_node, "bus-primary", NULL))
		slave_pdata->bus = EXT_SLAVE_BUS_PRIMARY;
	else if (of_find_property(of_node, "bus-secondary", NULL))
		slave_pdata->bus = EXT_SLAVE_BUS_SECONDARY;

	if (!of_property_read_u32(of_node, "address", &val))
		slave_pdata->address = val;

	of_property_read_u8_array(of_node, "orientation", slave_pdata->orientation, 9);
}

static struct mpu3050_platform_data *mpu3050_dt_parse_pdata(
		struct i2c_client *client)
{
	struct device_node *of_node = client->dev.of_node;
	struct device_node *accel_np, *compass_np, *pressure_np;
	struct mpu3050_platform_data *pdata;
	u32 val;

	if (!client->dev.of_node || client->dev.platform_data)
		return client->dev.platform_data;

	pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (!of_property_read_u32(of_node, "int-config", &val))
		pdata->int_config = val;

	if (!of_property_read_u32(of_node, "level-shifter", &val))
		pdata->level_shifter = val;

	of_property_read_u8_array(of_node, "orientation", pdata->orientation, 9);

	accel_np = of_find_node_by_name(of_node, "accelerometer");
	if (accel_np) {
		mpu3050_dt_parse_slave_pdata(client, accel_np, &pdata->accel);
		pdata->accel.get_slave_descr = get_accel_slave_descr;
	}

	compass_np = of_find_node_by_name(of_node, "compass");
	if (compass_np) {
		mpu3050_dt_parse_slave_pdata(client, compass_np, &pdata->compass);
		pdata->compass.get_slave_descr = get_compass_slave_descr;
	}

	pressure_np = of_find_node_by_name(of_node, "pressure");
	if (pressure_np) {
		mpu3050_dt_parse_slave_pdata(client, pressure_np, &pdata->pressure);
		pdata->pressure.get_slave_descr = get_pressure_slave_descr;
	}

	return pdata;
}

int mpu3050_probe(struct i2c_client *client,
		  const struct i2c_device_id *devid)
{
	struct mpu3050_platform_data *pdata = NULL;
	struct mpu_private_data *mpu;
	struct mldl_cfg *mldl_cfg;
	int res = 0;
	struct i2c_adapter *accel_adapter = NULL;
	struct i2c_adapter *compass_adapter = NULL;
	struct i2c_adapter *pressure_adapter = NULL;
	struct i2c_adapter *compass_yamaha_adapter = NULL;
	struct device_node *bus;

#if defined CONFIG_MPU_SENSORS_YAS529
	int i = 0;
	int compass_res = 0;
	unsigned char compass_rdata;
	unsigned char rawData[2];
	unsigned char dummyData[1] = { 0 };
#endif

	dev_dbg(&client->adapter->dev, "%s\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		res = -ENODEV;
		goto out_check_functionality_failed;
	}

	mpu = devm_kzalloc(&client->dev, sizeof(*mpu), GFP_KERNEL);
	if (!mpu) {
		res = -ENOMEM;
		goto out_alloc_data_failed;
	}

	i2c_set_clientdata(client, mpu);
	this_client = client;
	mldl_cfg = &mpu->mldl_cfg;
	pdata = mpu3050_dt_parse_pdata(client);
	if (!pdata) {
		dev_err(&this_client->adapter->dev,
			 "Warning no platform data for mpu3050\n");
		res = -ENOMEM;
		goto out_alloc_data_failed;
	} else {
		mldl_cfg->pdata = pdata;

		if (client->dev.of_node) {
			if (pdata->accel.get_slave_descr) {
				bus = of_parse_phandle(client->dev.of_node, "accel-i2c-bus", 0);
				if (!bus)
					accel_adapter = client->adapter;
				else {
					accel_adapter = of_find_i2c_adapter_by_node(bus);
					if (!accel_adapter)
						return -ENODEV;
				}
				pdata->accel.adapt_num = accel_adapter->nr;
			}

			if (pdata->compass.get_slave_descr) {
				bus = of_parse_phandle(client->dev.of_node, "compass-i2c-bus", 0);
				if (!bus)
					compass_adapter = client->adapter;
				else {
					compass_adapter = of_find_i2c_adapter_by_node(bus);
					if (!compass_adapter)
						return -ENODEV;
				}
				pdata->compass.adapt_num = compass_adapter->nr;
			}

			if (pdata->pressure.get_slave_descr) {
				bus = of_parse_phandle(client->dev.of_node, "pressure-i2c-bus", 0);
				if (!bus)
					pressure_adapter = client->adapter;
				else {
					pressure_adapter = of_find_i2c_adapter_by_node(bus);
					if (!pressure_adapter)
						return -ENODEV;
				}
				pdata->pressure.adapt_num = pressure_adapter->nr;
			}

#if defined CONFIG_MPU_SENSORS_YAS529
			bus = of_parse_phandle(client->dev.of_node, "compass-yamaha-i2c-bus", 0);
			if (!bus)
				compass_yamaha_adapter = client->adapter;
			else {
				compass_yamaha_adapter = of_find_i2c_adapter_by_node(bus);
				if (!compass_yamaha_adapter)
					return -ENODEV;
			}
#endif
		}

#if defined CONFIG_MPU_SENSORS_YAS529
		/* Read AKM8975 sensor ID */
		compass_res = AKM_i2c_read_byte(compass_yamaha_adapter, COMPASS_SLAVE_ID, 0, &compass_rdata);
		if (compass_res) {
			for (compass_retry_num = 0; compass_retry_num<COMPASS_RETRY_CNT; compass_retry_num++) {
				compass_res = AKM_i2c_read_byte(compass_yamaha_adapter, COMPASS_SLAVE_ID, 0, &compass_rdata);
				if (compass_res == 0)
					break;
				else
					pr_err("compass read AKM8975 sensor id error, retry(%d)\n", compass_retry_num);
			}
		}

		/* Check AKM8975 sensor ID */
		if (compass_rdata != COMPASS_AKM8975_SENSOR_ID) {

			/* YAS529 Change register(Config Register) to read mode */
			dummyData[0] = 0xC0 | 0x10;
			compass_res = mpu_sensor_i2c_write(compass_yamaha_adapter, 0x2E, 1, dummyData);
			if (compass_res) {
				for (compass_retry_num = 0; compass_retry_num < COMPASS_RETRY_CNT; compass_retry_num++) {
						compass_res = mpu_sensor_i2c_write(compass_yamaha_adapter, 0x2E, 1, dummyData);
					if (compass_res == 0)
						break;
					else
						pr_err("set YAS529 config register error, retry(%d)\n", compass_retry_num);
				}
				if (compass_retry_num == COMPASS_RETRY_CNT)
					goto out_alloc_data_failed;
			}

			/* Read YAS529 Sensor ID */
			compass_res = mpu_sensor_i2c_read(compass_yamaha_adapter,  0x2E, 4, 2, (unsigned char *) &rawData);
			if (compass_res) {
				for (compass_retry_num = 0; compass_retry_num < COMPASS_RETRY_CNT; compass_retry_num++) {
					compass_res = mpu_sensor_i2c_read(compass_yamaha_adapter,  0x2E, 4, 2, (unsigned char *) &rawData);
					if (compass_res == 0)
						break;
					else
						pr_err("compass read YAS592 sensor id error, retry(%d)\n", compass_retry_num);
				}
				if (compass_retry_num == COMPASS_RETRY_CNT)
					goto out_alloc_data_failed;
			}

			if (rawData[1] == COMPASS_YAS529_SENSOR_ID) {
				pdata->compass.get_slave_descr = yas529_get_slave_descr;
				pdata->compass.address = 0x2E;

				for (i = 0; i < 9; i++)
					pdata->compass.orientation[i] = YAS529_orientation[i];

				pr_info("compass sensor is YAS529 0x%02x\n",rawData[1]);
			}
		}
		else
			pr_info("compass sensor is AKM8975 0x%02x\n",compass_rdata);
#endif

		if (pdata->accel.get_slave_descr) {
			mldl_cfg->accel =
			    pdata->accel.get_slave_descr();
			dev_info(&this_client->adapter->dev,
				 "%s: +%s\n", MPU_NAME,
				 mldl_cfg->accel->name);
			if (!accel_adapter)
				accel_adapter =
					i2c_get_adapter(pdata->accel.adapt_num);
			if (pdata->accel.irq > 0) {
				dev_info(&this_client->adapter->dev,
					"Installing Accel irq using %d\n",
					pdata->accel.irq);
				res = slaveirq_init(accel_adapter,
						&pdata->accel,
						"accelirq");
				if (res)
					goto out_accelirq_failed;
			} else {
				dev_warn(&this_client->adapter->dev,
					"WARNING: Accel irq not assigned\n");
			}
		} else {
			dev_warn(&this_client->adapter->dev,
				 "%s: No Accel Present\n", MPU_NAME);
		}

		if (pdata->compass.get_slave_descr) {
			mldl_cfg->compass =
			    pdata->compass.get_slave_descr();
			dev_info(&this_client->adapter->dev,
				 "%s: +%s\n", MPU_NAME,
				 mldl_cfg->compass->name);
			if (!compass_adapter)
				compass_adapter =
					i2c_get_adapter(pdata->compass.adapt_num);
			if (pdata->compass.irq > 0) {
				dev_info(&this_client->adapter->dev,
					"Installing Compass irq using %d\n",
					pdata->compass.irq);
				res = slaveirq_init(compass_adapter,
						&pdata->compass,
						"compassirq");
				if (res)
					goto out_compassirq_failed;
			} else {
				dev_warn(&this_client->adapter->dev,
					"WARNING: Compass irq not assigned\n");
			}
		} else {
			dev_warn(&this_client->adapter->dev,
				 "%s: No Compass Present\n", MPU_NAME);
		}

		if (pdata->pressure.get_slave_descr) {
			mldl_cfg->pressure =
			    pdata->pressure.get_slave_descr();
			dev_info(&this_client->adapter->dev,
				 "%s: +%s\n", MPU_NAME,
				 mldl_cfg->pressure->name);
			if (!pressure_adapter)
				pressure_adapter =
					i2c_get_adapter(pdata->pressure.adapt_num);
			if (pdata->pressure.irq > 0) {
				dev_info(&this_client->adapter->dev,
					"Installing Pressure irq using %d\n",
					pdata->pressure.irq);
				res = slaveirq_init(pressure_adapter,
						&pdata->pressure,
						"pressureirq");
				if (res)
					goto out_pressureirq_failed;
			} else {
				dev_warn(&this_client->adapter->dev,
					"WARNING: Pressure irq not assigned\n");
			}
		} else {
			dev_warn(&this_client->adapter->dev,
				 "%s: No Pressure Present\n", MPU_NAME);
		}
	}

	mldl_cfg->addr = client->addr;
	res = mpu3050_open(&mpu->mldl_cfg, client->adapter,
			accel_adapter, compass_adapter, pressure_adapter);

	if (res) {
		dev_err(&this_client->adapter->dev,
			"Unable to open %s %d\n", MPU_NAME, res);
		res = -ENODEV;
		goto out_whoami_failed;
	}

	res = misc_register(&i2c_mpu_device);
	if (res < 0) {
		dev_err(&this_client->adapter->dev,
			"ERROR: misc_register returned %d\n", res);
		goto out_misc_register_failed;
	}

	if (this_client->irq > 0) {
		dev_info(&this_client->adapter->dev,
			 "Installing irq using %d\n", this_client->irq);
		res = mpuirq_init(this_client);
		if (res)
			goto out_mpuirq_failed;
	} else {
		dev_warn(&this_client->adapter->dev,
			 "WARNING: %s irq not assigned\n", MPU_NAME);
	}


#ifdef CONFIG_PM_EARLYSUSPEND
	mpu->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	mpu->early_suspend.suspend = mpu3050_early_suspend;
	mpu->early_suspend.resume = mpu3050_early_resume;
	register_early_suspend(&mpu->early_suspend);
#endif
	return res;

out_mpuirq_failed:
	misc_deregister(&i2c_mpu_device);
out_misc_register_failed:
	mpu3050_close(&mpu->mldl_cfg, client->adapter,
		accel_adapter, compass_adapter, pressure_adapter);
out_whoami_failed:
	if (pdata &&
	    pdata->pressure.get_slave_descr &&
	    pdata->pressure.irq)
		slaveirq_exit(&pdata->pressure);
out_pressureirq_failed:
	if (pdata &&
	    pdata->compass.get_slave_descr &&
	    pdata->compass.irq)
		slaveirq_exit(&pdata->compass);
out_compassirq_failed:
	if (pdata &&
	    pdata->accel.get_slave_descr &&
	    pdata->accel.irq)
		slaveirq_exit(&pdata->accel);
out_accelirq_failed:
out_alloc_data_failed:
out_check_functionality_failed:
	dev_err(&this_client->adapter->dev, "%s failed %d\n", __func__,
		res);
	return res;

}

static int mpu3050_remove(struct i2c_client *client)
{
	struct mpu_private_data *mpu = i2c_get_clientdata(client);
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct mpu3050_platform_data *pdata = mldl_cfg->pdata;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter =
	    i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	dev_dbg(&client->adapter->dev, "%s\n", __func__);

#ifdef CONFIG_PM_EARLYSUSPEND
	unregister_early_suspend(&mpu->early_suspend);
#endif
	mpu3050_close(mldl_cfg, client->adapter,
		accel_adapter, compass_adapter, pressure_adapter);

	if (client->irq)
		mpuirq_exit();

	if (pdata &&
	    pdata->pressure.get_slave_descr &&
	    pdata->pressure.irq)
		slaveirq_exit(&pdata->pressure);

	if (pdata &&
	    pdata->compass.get_slave_descr &&
	    pdata->compass.irq)
		slaveirq_exit(&pdata->compass);

	if (pdata &&
	    pdata->accel.get_slave_descr &&
	    pdata->accel.irq)
		slaveirq_exit(&pdata->accel);

	misc_deregister(&i2c_mpu_device);

	return 0;
}

static const struct i2c_device_id mpu3050_id[] = {
	{MPU_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, mpu3050_id);

static const struct of_device_id mpu3050_of_match[] = {
	{ .compatible = "invn,mpu3050", },
	{ },
};
MODULE_DEVICE_TABLE(of, mpu3050_of_match);

static struct i2c_driver mpu3050_driver = {
	.class = I2C_CLASS_HWMON,
	.probe = mpu3050_probe,
	.remove = mpu3050_remove,
	.id_table = mpu3050_id,
	.driver = {
		.owner = THIS_MODULE,
		.name = MPU_NAME,
		.of_match_table = mpu3050_of_match,
	},
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 32)
	.address_data = &addr_data,
#else
	.address_list = normal_i2c,
#endif

	.shutdown = mpu_shutdown,	/* optional */
	.suspend = mpu_suspend,	/* optional */
	.resume = mpu_resume,	/* optional */

};
module_i2c_driver(mpu3050_driver);

MODULE_AUTHOR("Invensense Corporation");
MODULE_DESCRIPTION("User space character device interface for MPU3050");
MODULE_LICENSE("GPL");
MODULE_ALIAS(MPU_NAME);
