/*
 * Input Multitouch Library
 *
 * Copyright (c) 2008-2010 Henrik Rydberg
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/input/mt.h>
#include <linux/slab.h>

/**
 * input_mt_init_slots() - initialize MT input slots
 * @dev: input device supporting MT events and finger tracking
 * @num_slots: number of slots used by the device
 *
 * This function allocates all necessary memory for MT slot handling
 * in the input device, adds ABS_MT_SLOT to the device capabilities
 * and sets up appropriate event buffers. All slots are initially
 * marked as unused by setting ABS_MT_TRACKING_ID to -1. May be called
 * repeatedly. Returns -EINVAL if attempting to reinitialize with a
 * different number of slots.
 */
int input_mt_init_slots(struct input_dev *dev, unsigned int num_slots)
{
	int i;

	num_slots=2;
	if (!num_slots)
		return 0;
	if (dev->mt)
		return dev->mtsize != num_slots ? -EINVAL : 0;

	dev->mt = kcalloc(num_slots, sizeof(struct input_mt_slot), GFP_KERNEL);
	if (!dev->mt)
		return -ENOMEM;

	dev->mtsize = num_slots;
	input_set_abs_params(dev, ABS_MT_SLOT, 0, num_slots - 1, 0, 0);
	input_set_events_per_packet(dev, 6 * num_slots);

	/* Mark slots as 'unused' */
	for (i = 0; i < num_slots; i++)
		input_mt_set_value(&dev->mt[i], ABS_MT_TRACKING_ID, -1);

	return 0;
}
EXPORT_SYMBOL(input_mt_init_slots);

/**
 * input_mt_destroy_slots() - frees the MT slots of the input device
 * @dev: input device with allocated MT slots
 *
 * This function is only needed in error path as the input core will
 * automatically free the MT slots when the device is destroyed.
 */
void input_mt_destroy_slots(struct input_dev *dev)
{
	kfree(dev->mt);
	dev->mt = NULL;
	dev->mtsize = 0;
	dev->slot = 0;
}
EXPORT_SYMBOL(input_mt_destroy_slots);

