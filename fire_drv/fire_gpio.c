// SPDX-License-Identifier: GPL-2.0
/*
 * RK3568 GPIO1_B2 (GPIO 42) read-only character device.
 * Device node: /dev/fire_gpio
 * Input with internal pull-down; no DTS edits are required.
 *
 * Linux 4.19 Rockchip BSP: requires GPIO/pinctrl drivers already present,
 * and GPIO1_B2 must not be reserved by another driver.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/version.h>

#define FIRE_GPIO_NUM       (1 * 32 + 4) /* GPIO1_A4 = GPIO 36 */
#define FIRE_DEVICE_NAME   "fire_gpio"

static dev_t fire_devt;
static struct cdev fire_cdev;
static struct class *fire_class;
static struct device *fire_device;

/* Each new open/read sequence gets the current level as ASCII: "0\n"/"1\n". */
static ssize_t fire_gpio_read(struct file *file, char __user *user_buf,
                              size_t count, loff_t *ppos)
{
    char text[3];
    int level;
    int len;

    level = gpio_get_value(FIRE_GPIO_NUM);
    if (level < 0)
        return level;

    len = scnprintf(text, sizeof(text), "%d\n", !!level);
    return simple_read_from_buffer(user_buf, count, ppos, text, len);
}

static const struct file_operations fire_gpio_fops = {
    .owner  = THIS_MODULE,
    .read   = fire_gpio_read,
    .llseek = default_llseek,
};

static int __init fire_gpio_init(void)
{
    unsigned long pull_cfg;
    int ret;

    if (!gpio_is_valid(FIRE_GPIO_NUM)) {
        pr_err("fire_gpio: GPIO %d is invalid\n", FIRE_GPIO_NUM);
        return -EINVAL;
    }

    /* Existing Rockchip pinctrl driver switches IOMUX to GPIO on request. */
    ret = gpio_request(FIRE_GPIO_NUM, FIRE_DEVICE_NAME);
    if (ret) {
        pr_err("fire_gpio: gpio_request(%d) failed: %d\n",
               FIRE_GPIO_NUM, ret);
        return ret;
    }

    ret = gpio_direction_input(FIRE_GPIO_NUM);
    if (ret) {
        pr_err("fire_gpio: gpio_direction_input failed: %d\n", ret);
        goto err_gpio;
    }

    /* Input bias, not a driven output-low; do not silently ignore failure. */
    pull_cfg = pinconf_to_config_packed(PIN_CONFIG_BIAS_PULL_DOWN, 1);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    /* Newer kernels configure bias through the GPIO descriptor API. */
    ret = gpiod_set_config(gpio_to_desc(FIRE_GPIO_NUM), pull_cfg);
#else
    /* Linux 4.19 pinctrl API takes the global GPIO number. */
    ret = pinctrl_gpio_set_config(FIRE_GPIO_NUM, pull_cfg);
#endif
    if (ret) {
        pr_err("fire_gpio: internal pull-down setup failed: %d\n", ret);
        goto err_gpio;
    }

    ret = alloc_chrdev_region(&fire_devt, 0, 1, FIRE_DEVICE_NAME);
    if (ret) {
        pr_err("fire_gpio: alloc_chrdev_region failed: %d\n", ret);
        goto err_gpio;
    }

    cdev_init(&fire_cdev, &fire_gpio_fops);
    fire_cdev.owner = THIS_MODULE;
    ret = cdev_add(&fire_cdev, fire_devt, 1);
    if (ret) {
        pr_err("fire_gpio: cdev_add failed: %d\n", ret);
        goto err_chrdev;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    fire_class = class_create(FIRE_DEVICE_NAME);
#else
    fire_class = class_create(THIS_MODULE, FIRE_DEVICE_NAME);
#endif
    if (IS_ERR(fire_class)) {
        ret = PTR_ERR(fire_class);
        fire_class = NULL;
        pr_err("fire_gpio: class_create failed: %d\n", ret);
        goto err_cdev;
    }

    /* With devtmpfs (or udev), this appears as /dev/fire_gpio. */
    fire_device = device_create(fire_class, NULL, fire_devt, NULL,
                                FIRE_DEVICE_NAME);
    if (IS_ERR(fire_device)) {
        ret = PTR_ERR(fire_device);
        fire_device = NULL;
        pr_err("fire_gpio: device_create failed: %d\n", ret);
        goto err_class;
    }

    pr_info("fire_gpio: GPIO1_B2 (GPIO %d), input, pull-down enabled\n",
            FIRE_GPIO_NUM);
    pr_info("fire_gpio: device /dev/%s (major=%d minor=%d)\n",
            FIRE_DEVICE_NAME, MAJOR(fire_devt), MINOR(fire_devt));
    return 0;

err_class:
    class_destroy(fire_class);
err_cdev:
    cdev_del(&fire_cdev);
err_chrdev:
    unregister_chrdev_region(fire_devt, 1);
err_gpio:
    gpio_free(FIRE_GPIO_NUM);
    return ret;
}

static void __exit fire_gpio_exit(void)
{
    device_destroy(fire_class, fire_devt);
    class_destroy(fire_class);
    cdev_del(&fire_cdev);
    unregister_chrdev_region(fire_devt, 1);
    gpio_free(FIRE_GPIO_NUM);
    pr_info("fire_gpio: module removed\n");
}

module_init(fire_gpio_init);
module_exit(fire_gpio_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RK3568 GPIO1_B2 input, internal pull-down, /dev/fire_gpio");
MODULE_AUTHOR("OpenAI");
