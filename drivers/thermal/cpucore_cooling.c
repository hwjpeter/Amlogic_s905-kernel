/*
 *  linux/drivers/thermal/cpu_cooling.c
 *
 *  Copyright (C) 2012	Samsung Electronics Co., Ltd(http://www.samsung.com)
 *  Copyright (C) 2012  Amit Daniel <amit.kachhap@linaro.org>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpucore_cooling.h>
#include <linux/thermal_core.h>

/**
 * struct cpucore_cooling_device - data for cooling device with cpucore
 * @id: unique integer value corresponding to each cpucore_cooling_device
 *	registered.
 * @cool_dev: thermal_cooling_device pointer to keep track of the
 *	registered cooling device.
 * @cpucore_state: integer value representing the current state of cpucore
 *	cooling	devices.
 * @cpucore_val: integer value representing the absolute value of the clipped
 *	frequency.
 * @allowed_cpus: all the cpus involved for this cpucore_cooling_device.
 *
 * This structure is required for keeping information of each
 * cpucore_cooling_device registered. In order to prevent corruption of this a
 * mutex lock cooling_cpucore_lock is used.
 */

static DEFINE_IDR(cpucore_idr);
static DEFINE_MUTEX(cooling_cpucore_lock);

/* notify_table passes value to the cpucore_ADJUST callback function. */
#define NOTIFY_INVALID NULL

/**
 * get_idr - function to get a unique id.
 * @idr: struct idr * handle used to create a id.
 * @id: int * value generated by this function.
 *
 * This function will populate @id with an unique
 * id, using the idr API.
 *
 * Return: 0 on success, an error code on failure.
 */
static int get_idr(struct idr *idr, int *id)
{
	int ret;

	mutex_lock(&cooling_cpucore_lock);
	ret = idr_alloc(idr, NULL, 0, 0, GFP_KERNEL);
	mutex_unlock(&cooling_cpucore_lock);
	if (unlikely(ret < 0))
		return ret;
	*id = ret;

	return 0;
}

/**
 * release_idr - function to free the unique id.
 * @idr: struct idr * handle used for creating the id.
 * @id: int value representing the unique id.
 */
static void release_idr(struct idr *idr, int id)
{
	mutex_lock(&cooling_cpucore_lock);
	idr_remove(idr, id);
	mutex_unlock(&cooling_cpucore_lock);
}

/* cpucore cooling device callback functions are defined below */

/**
 * cpucore_get_max_state - callback function to get the max cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the max cooling state.
 *
 * Callback for the thermal cooling device to return the cpucore
 * max cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int cpucore_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpucore_cooling_device *cpucore_device = cdev->devdata;
	*state = cpucore_device->max_cpu_core_num;
	pr_debug("max cpu core=%ld\n", *state);
	return 0;
}

/**
 * cpucore_get_cur_state - callback function to get the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the current cooling state.
 *
 * Callback for the thermal cooling device to return the cpucore
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int cpucore_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpucore_cooling_device *cpucore_device = cdev->devdata;
	*state = cpucore_device->cpucore_state;
	pr_debug("current state=%ld\n", *state);
	return 0;
}

/**
 * cpucore_set_cur_state - callback function to set the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: set this variable to the current cooling state.
 *
 * Callback for the thermal cooling device to change the cpucore
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int cpucore_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct cpucore_cooling_device *cpucore_device = cdev->devdata;
	int set_max_num;
	mutex_lock(&cooling_cpucore_lock);
	if (cpucore_device->stop_flag) {
		mutex_unlock(&cooling_cpucore_lock);
		return 0;
	}
	if ((state & CPU_STOP) == CPU_STOP) {
		cpucore_device->stop_flag = 1;
		state = state&(~CPU_STOP);
	}
	mutex_unlock(&cooling_cpucore_lock);
	if (cpucore_device->max_cpu_core_num - state > 0) {
		cpucore_device->cpucore_state = state;
		set_max_num = cpucore_device->max_cpu_core_num - state;
		pr_debug("set max cpu num=%d,state=%ld\n", set_max_num, state);
		cpufreq_set_max_cpu_num(set_max_num);
	}

	return 0;
}

/*
 * Simple mathematics model for cpu core power:
 * just for ipa hook, nothing to do;
 */
static int cpucore_get_requested_power(struct thermal_cooling_device *cdev,
				       struct thermal_zone_device *tz,
				       u32 *power)
{
	*power = 0;

	return 0;
}

static int cpucore_state2power(struct thermal_cooling_device *cdev,
			       struct thermal_zone_device *tz,
			       unsigned long state, u32 *power)
{
	*power = 0;

	return 0;
}

static int cpucore_power2state(struct thermal_cooling_device *cdev,
			       struct thermal_zone_device *tz, u32 power,
			       unsigned long *state)
{
	cdev->ops->get_cur_state(cdev, state);
	return 0;
}

static int cpucore_notify_state(struct thermal_cooling_device *cdev,
				struct thermal_zone_device *tz,
				enum thermal_trip_type type)
{
	unsigned long cur_state;
	long upper = -1;
	int i;
	struct thermal_instance *ins;

	switch (type) {
	case THERMAL_TRIP_HOT:
		if (tz->enter_hot) {
			for (i = 0; i < tz->trips; i++) {
				ins = get_thermal_instance(tz, cdev, i);
				if (ins && ins->upper > upper)
					upper = ins->upper;
			}
			cdev->ops->get_cur_state(cdev, &cur_state);
			cur_state += 1;
			/* do not exceed upper levels */
			if (upper != -1 && cur_state > upper)
				cur_state = upper;
			cdev->ops->set_cur_state(cdev, cur_state);
		} else {
			cur_state = 0;
			cdev->ops->set_cur_state(cdev, cur_state);
		}
		dev_info(&cdev->device, "cur_state:%ld\n", cur_state);
		break;
	default:
		break;
	}
	return 0;
}


/* Bind cpucore callbacks to thermal cooling device ops */
static struct thermal_cooling_device_ops const cpucore_cooling_ops = {
	.get_max_state = cpucore_get_max_state,
	.get_cur_state = cpucore_get_cur_state,
	.set_cur_state = cpucore_set_cur_state,
	.state2power   = cpucore_state2power,
	.power2state   = cpucore_power2state,
	.notify_state  = cpucore_notify_state,
	.get_requested_power = cpucore_get_requested_power,
};

/**
 * cpucore_cooling_register - function to create cpucore cooling device.
 *
 * This interface function registers the cpucore cooling device with the name
 * "thermal-cpucore-%x". This api can support multiple instances of cpucore
 * cooling devices.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
struct thermal_cooling_device *
cpucore_cooling_register(struct device_node *np)
{
	struct thermal_cooling_device *cool_dev;
	struct cpucore_cooling_device *cpucore_dev = NULL;
	char dev_name[THERMAL_NAME_LENGTH];
	int ret = 0;
	cpucore_dev = kzalloc(sizeof(struct cpucore_cooling_device),
			      GFP_KERNEL);
	if (!cpucore_dev)
		return ERR_PTR(-ENOMEM);

	ret = get_idr(&cpucore_idr, &cpucore_dev->id);
	if (ret) {
		kfree(cpucore_dev);
		return ERR_PTR(-EINVAL);
	}

	cpucore_dev->max_cpu_core_num = num_possible_cpus();
	snprintf(dev_name, sizeof(dev_name), "thermal-cpucore-%d",
		 cpucore_dev->id);

	cool_dev = thermal_of_cooling_device_register(np, dev_name, cpucore_dev,
						      &cpucore_cooling_ops);
	if (!cool_dev) {
		release_idr(&cpucore_idr, cpucore_dev->id);
		kfree(cpucore_dev);
		return ERR_PTR(-EINVAL);
	}
	cpucore_dev->cool_dev = cool_dev;
	cpucore_dev->cpucore_state = 0;
	return cool_dev;
}
EXPORT_SYMBOL_GPL(cpucore_cooling_register);

/**
 * cpucore_cooling_unregister - function to remove cpucore cooling device.
 * @cdev: thermal cooling device pointer.
 *
 * This interface function unregisters the "thermal-cpucore-%x" cooling device.
 */
void cpucore_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct cpucore_cooling_device *cpucore_dev;

	if (!cdev)
		return;

	cpucore_dev = cdev->devdata;

	thermal_cooling_device_unregister(cpucore_dev->cool_dev);
	release_idr(&cpucore_idr, cpucore_dev->id);
	kfree(cpucore_dev);
}
EXPORT_SYMBOL_GPL(cpucore_cooling_unregister);
