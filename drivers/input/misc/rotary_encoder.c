/*
 * rotary_encoder.c
 *
 * (c) 2009 Daniel Mack <daniel@caiaq.de>
 * Copyright (C) 2011 Johan Hovold <jhovold@gmail.com>
 *
 * state machine code inspired by code from Tim Ruetz
 *
 * A generic driver for rotary encoders connected to GPIO lines.
 * See file:Documentation/input/rotary-encoder.txt for more information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/input-polldev.h>

#define DRV_NAME "rotary-encoder"

struct rotary_encoder {
	struct input_dev *input;
#ifdef CONFIG_INPUT_GPIO_ROTARY_ENCODER_POLL_MODE_SUPPORT
	struct input_polled_dev *poll_dev;
#endif
	struct mutex access_mutex;

	u32 steps;
	u32 axis;
	bool relative_axis;
	bool rollover;
	bool absolute_encoder;

	unsigned int pos;

	struct gpio_descs *gpios;
	struct device *dev;

	int *irq;

	bool armed;
	signed char dir;	/* 1 - clockwise, -1 - CCW */

	unsigned int last_stable;
};

static unsigned int rotary_encoder_get_state(struct rotary_encoder *encoder)
{
	int i;
	unsigned int ret = 0;

	for (i = 0; i < encoder->gpios->ndescs; ++i) {
		int val = gpiod_get_value_cansleep(encoder->gpios->desc[i]);
		/* convert from gray encoding to normal */
		if (ret & 1)
			val = !val;

		ret = ret << 1 | val;
	}

	return ret & 3;
}

static unsigned int rotary_encoder_get_gpios_state(struct rotary_encoder
						   *encoder)
{
	int i;
	unsigned int ret = 0;

	for (i = 0; i < encoder->gpios->ndescs; ++i) {
		int val = gpiod_get_value_cansleep(encoder->gpios->desc[i]);

		ret = ret << 1 | val;
	}

	return ret;
}

static void rotary_encoder_report_event(struct rotary_encoder *encoder)
{
	if (encoder->relative_axis) {
		input_report_rel(encoder->input,
				 encoder->axis, encoder->dir);
	} else {
		unsigned int pos = encoder->pos;

		if (encoder->dir < 0) {
			/* turning counter-clockwise */
			if (encoder->rollover)
				pos += encoder->steps;
			if (pos)
				pos--;
		} else {
			/* turning clockwise */
			if (encoder->rollover || pos < encoder->steps)
				pos++;
		}

		if (encoder->rollover)
			pos %= encoder->steps;

		encoder->pos = pos;
		input_report_abs(encoder->input, encoder->axis, encoder->pos);
	}

	input_sync(encoder->input);
}

static irqreturn_t rotary_encoder_irq(int irq, void *dev_id)
{
	struct rotary_encoder *encoder = dev_id;
	unsigned int state;

	mutex_lock(&encoder->access_mutex);

	state = rotary_encoder_get_state(encoder);

	switch (state) {
	case 0x0:
		if (encoder->armed) {
			rotary_encoder_report_event(encoder);
			encoder->armed = false;
		}
		break;

	case 0x1:
	case 0x3:
		if (encoder->armed)
			encoder->dir = 2 - state;
		break;

	case 0x2:
		encoder->armed = true;
		break;
	}

	mutex_unlock(&encoder->access_mutex);

	return IRQ_HANDLED;
}

static irqreturn_t rotary_encoder_half_period_irq(int irq, void *dev_id)
{
	struct rotary_encoder *encoder = dev_id;
	unsigned int state;

	mutex_lock(&encoder->access_mutex);

	state = rotary_encoder_get_state(encoder);

	if (state & 1) {
		encoder->dir = ((encoder->last_stable - state + 1) % 4) - 1;
	} else {
		if (state != encoder->last_stable) {
			rotary_encoder_report_event(encoder);
			encoder->last_stable = state;
		}
	}

	mutex_unlock(&encoder->access_mutex);

	return IRQ_HANDLED;
}

static irqreturn_t rotary_encoder_quarter_period_irq(int irq, void *dev_id)
{
	struct rotary_encoder *encoder = dev_id;
	unsigned int state;

	mutex_lock(&encoder->access_mutex);

	state = rotary_encoder_get_state(encoder);

	if ((encoder->last_stable + 1) % 4 == state)
		encoder->dir = 1;
	else if (encoder->last_stable == (state + 1) % 4)
		encoder->dir = -1;
	else
		goto out;

	rotary_encoder_report_event(encoder);

out:
	encoder->last_stable = state;
	mutex_unlock(&encoder->access_mutex);

	return IRQ_HANDLED;
}

static void rotary_encoder_setup_input_params(struct rotary_encoder  *encoder)
{
	struct input_dev *input = encoder->input;
	struct platform_device *pdev = to_platform_device(encoder->dev);

	input->name = pdev->name;
	input->id.bustype = BUS_HOST;
	input->dev.parent = encoder->dev;

	if (encoder->relative_axis)
		input_set_capability(input, EV_REL, encoder->axis);
	else
		input_set_abs_params(input,
				     encoder->axis, 0, encoder->steps, 0, 1);
}

static irqreturn_t rotary_absolute_encoder_irq(int irq, void *dev_id)
{
	struct rotary_encoder *encoder = dev_id;
	unsigned int state;

	mutex_lock(&encoder->access_mutex);

	state = rotary_encoder_get_gpios_state(encoder);
	if (state != encoder->last_stable) {
		input_report_abs(encoder->input, encoder->axis, state);
		input_sync(encoder->input);
		encoder->last_stable = state;
	}

	mutex_lock(&encoder->access_mutex);

	return IRQ_HANDLED;
}

#ifdef CONFIG_INPUT_GPIO_ROTARY_ENCODER_POLL_MODE_SUPPORT
static void rotary_encoder_poll_gpios(struct input_polled_dev *poll_dev)
{
	struct rotary_encoder *encoder = poll_dev->private;
	unsigned int state = rotary_encoder_get_gpios_state(encoder);

	if (state != encoder->last_stable) {
		input_report_abs(encoder->input, encoder->axis, state);
		input_sync(encoder->input);
		encoder->last_stable = state;
	}
}

static int rotary_encoder_register_poll_device(struct rotary_encoder
		*encoder)
{
	struct input_polled_dev *poll_dev =
		devm_input_allocate_polled_device(encoder->dev);

	if (!poll_dev)
		return -ENOMEM;
	poll_dev->private = encoder;
	poll_dev->poll = rotary_encoder_poll_gpios;
	encoder->input = poll_dev->input;
	rotary_encoder_setup_input_params(encoder);
	encoder->poll_dev = poll_dev;

	return input_register_polled_device(poll_dev);
}
#endif

static int rotary_encoder_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rotary_encoder *encoder;
	struct input_dev *input;
	irq_handler_t handler;
	u32 steps_per_period;
	unsigned int i;
	int err;

	encoder = devm_kzalloc(dev, sizeof(struct rotary_encoder), GFP_KERNEL);
	if (!encoder)
		return -ENOMEM;

	mutex_init(&encoder->access_mutex);

	device_property_read_u32(dev, "rotary-encoder,steps", &encoder->steps);

	err = device_property_read_u32(dev, "rotary-encoder,steps-per-period",
				       &steps_per_period);
	if (err) {
		/*
		 * The 'half-period' property has been deprecated, you must
		 * use 'steps-per-period' and set an appropriate value, but
		 * we still need to parse it to maintain compatibility. If
		 * neither property is present we fall back to the one step
		 * per period behavior.
		 */
		steps_per_period = device_property_read_bool(dev,
					"rotary-encoder,half-period") ? 2 : 1;
	}

	encoder->rollover =
		device_property_read_bool(dev, "rotary-encoder,rollover");

	device_property_read_u32(dev, "linux,axis", &encoder->axis);
	encoder->relative_axis =
		device_property_read_bool(dev, "rotary-encoder,relative-axis");

	encoder->gpios = devm_gpiod_get_array(dev, NULL, GPIOD_IN);
	if (IS_ERR(encoder->gpios)) {
		dev_err(dev, "unable to get gpios\n");
		return PTR_ERR(encoder->gpios);
	}
	if (encoder->gpios->ndescs < 2) {
		dev_err(dev, "not enough gpios found\n");
		return -EINVAL;
	}

	encoder->dev = dev;
	encoder->absolute_encoder =
		device_property_read_bool(dev,
					  "rotary-encoder,absolute-encoder");
	if (encoder->absolute_encoder) {
		handler = rotary_absolute_encoder_irq;
	} else {
		switch (steps_per_period >> (encoder->gpios->ndescs - 2)) {
		case 4:
			handler = &rotary_encoder_quarter_period_irq;
			encoder->last_stable =
				rotary_encoder_get_state(encoder);
			break;
		case 2:
			handler = &rotary_encoder_half_period_irq;
			encoder->last_stable =
				rotary_encoder_get_state(encoder);
			break;
		case 1:
			handler = &rotary_encoder_irq;
			break;
		default:
			dev_err(dev, "'%d' is not a valid steps-per-period value\n",
				steps_per_period);
			return -EINVAL;
		}
	}

	encoder->irq =
		devm_kzalloc(dev,
			     sizeof(*encoder->irq) * encoder->gpios->ndescs,
			     GFP_KERNEL);
	if (!encoder->irq)
		return -ENOMEM;

	for (i = 0; i < encoder->gpios->ndescs; ++i) {
		encoder->irq[i] = gpiod_to_irq(encoder->gpios->desc[i]);

#ifdef CONFIG_INPUT_GPIO_ROTARY_ENCODER_POLL_MODE_SUPPORT
		if (encoder->irq[i] < 0 && encoder->absolute_encoder) {
			dev_info(dev, "Using poll mode\n");
			err = rotary_encoder_register_poll_device(encoder);
			if (err) {
				dev_err(dev, "failed to register poll dev\n");
				return err;
			}
			platform_set_drvdata(pdev, encoder);
			return 0;
		}
#endif
		err = devm_request_threaded_irq(dev, encoder->irq[i],
				NULL, handler,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT,
				DRV_NAME, encoder);
		if (err) {
			dev_err(dev, "unable to request IRQ %d (gpio#%d)\n",
				encoder->irq[i], i);
			return err;
		}
	}

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	encoder->input = input;
	rotary_encoder_setup_input_params(encoder);
	err = input_register_device(input);
	if (err) {
		dev_err(dev, "failed to register input device\n");
		return err;
	}

	device_init_wakeup(dev,
			   device_property_read_bool(dev, "wakeup-source"));

	platform_set_drvdata(pdev, encoder);

	return 0;
}

static int __maybe_unused rotary_encoder_suspend(struct device *dev)
{
	struct rotary_encoder *encoder = dev_get_drvdata(dev);
	unsigned int i;

	if (device_may_wakeup(dev)) {
		for (i = 0; i < encoder->gpios->ndescs; ++i)
			enable_irq_wake(encoder->irq[i]);
	}

	return 0;
}

static int __maybe_unused rotary_encoder_resume(struct device *dev)
{
	struct rotary_encoder *encoder = dev_get_drvdata(dev);
	unsigned int i;

	if (device_may_wakeup(dev)) {
		for (i = 0; i < encoder->gpios->ndescs; ++i)
			disable_irq_wake(encoder->irq[i]);
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(rotary_encoder_pm_ops,
			 rotary_encoder_suspend, rotary_encoder_resume);

#ifdef CONFIG_OF
static const struct of_device_id rotary_encoder_of_match[] = {
	{ .compatible = "rotary-encoder", },
	{ },
};
MODULE_DEVICE_TABLE(of, rotary_encoder_of_match);
#endif

static struct platform_driver rotary_encoder_driver = {
	.probe		= rotary_encoder_probe,
	.driver		= {
		.name	= DRV_NAME,
		.pm	= &rotary_encoder_pm_ops,
		.of_match_table = of_match_ptr(rotary_encoder_of_match),
	}
};
module_platform_driver(rotary_encoder_driver);

MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DESCRIPTION("GPIO rotary encoder driver");
MODULE_AUTHOR("Daniel Mack <daniel@caiaq.de>, Johan Hovold");
MODULE_LICENSE("GPL v2");
