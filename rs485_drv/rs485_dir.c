// SPDX-License-Identifier: GPL-2.0
/*
 * RK3566 RS485 DE/RE direction control character driver.
 *
 * Hardware used by this project:
 *   SoC pin     : GPIO2_B1_d
 *   package ball: A35
 *   legacy Linux GPIO number:
 *       bank 2 * 32 + port B * 8 + pin 1 = 2 * 32 + 1 * 8 + 1 = 73
 *
 * User-space ABI expected by MergedQtApp/Rs485DriverPort:
 *   device node : /dev/rs485_dir
 *   write 0x01  : transmit direction (DE asserted)
 *   write 0x00  : receive direction  (DE deasserted)
 *
 * For shell testing only, ASCII '1' and '0' are accepted as well.  The Qt
 * application writes binary bytes, so `echo 1` is not the application ABI.
 *
 * Out-of-tree Kbuild entry:
 *   obj-m += rs485_dir.o
 *
 * Example build:
 *   make -C /lib/modules/$(uname -r)/build M=$PWD modules
 *
 * Example load (default GPIO2_B1 / GPIO 73, active-high DE):
 *   insmod rs485_dir.ko
 *
 * If the transceiver DE is active-low:
 *   insmod rs485_dir.ko active_low=1
 *
 * GPIO2_B1 must not be claimed by another device-tree node.  Its pinmux must
 * be GPIO, not an alternate peripheral function.  The preferred RK3566 DTS
 * pin description is bank 2 / RK_PB1 / RK_FUNC_GPIO.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#define RS485_DIR_DEVICE_NAME "rs485_dir"
#define RK3566_GPIO2_B1        73

static int gpio_num = RK3566_GPIO2_B1;
module_param(gpio_num, int, 0444);
MODULE_PARM_DESC(gpio_num,
                 "Legacy Linux GPIO number for RS485 DE/RE (default: 73/GPIO2_B1)");

static bool active_low;
module_param(active_low, bool, 0444);
MODULE_PARM_DESC(active_low,
                 "Set to 1 when the RS485 DE signal is active-low (default: 0)");

static DEFINE_MUTEX(rs485_dir_lock);
static atomic_t rs485_dir_opened = ATOMIC_INIT(0);
static bool gpio_requested;

/* Qt semantics are fixed: logical 1 is TX and logical 0 is RX. */
static void rs485_dir_set(bool transmit)
{
    int physical_value = transmit ? 1 : 0;

    if (active_low)
        physical_value = !physical_value;

    gpio_set_value_cansleep(gpio_num, physical_value);
}

static int rs485_dir_open(struct inode *inode, struct file *file)
{
    int ret;

    if (atomic_cmpxchg(&rs485_dir_opened, 0, 1) != 0)
        return -EBUSY;

    ret = nonseekable_open(inode, file);
    if (ret) {
        atomic_set(&rs485_dir_opened, 0);
        return ret;
    }

    /* Always enter the safe receive state when Qt opens the device. */
    mutex_lock(&rs485_dir_lock);
    rs485_dir_set(false);
    mutex_unlock(&rs485_dir_lock);
    return 0;
}

static ssize_t rs485_dir_write(struct file *file,
                               const char __user *buffer,
                               size_t length,
                               loff_t *offset)
{
    unsigned char value;
    bool transmit;

    if (length == 0)
        return 0;

    if (copy_from_user(&value, buffer, sizeof(value)) != 0)
        return -EFAULT;

    switch (value) {
    case 0x00:
    case '0':
        transmit = false;
        break;
    case 0x01:
    case '1':
        transmit = true;
        break;
    default:
        return -EINVAL;
    }

    mutex_lock(&rs485_dir_lock);
    rs485_dir_set(transmit);
    mutex_unlock(&rs485_dir_lock);

    /*
     * This must be the consumed byte count, not zero.  Rs485DriverPort checks
     * that its one-byte QFile::write() returns exactly 1.
     */
    return length;
}

static int rs485_dir_release(struct inode *inode, struct file *file)
{
    /* Leave the transceiver listening when the application closes. */
    mutex_lock(&rs485_dir_lock);
    rs485_dir_set(false);
    mutex_unlock(&rs485_dir_lock);
    atomic_set(&rs485_dir_opened, 0);
    return 0;
}

static const struct file_operations rs485_dir_fops = {
    .owner   = THIS_MODULE,
    .open    = rs485_dir_open,
    .write   = rs485_dir_write,
    .release = rs485_dir_release,
    .llseek  = no_llseek,
};

static struct miscdevice rs485_dir_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = RS485_DIR_DEVICE_NAME,
    .fops  = &rs485_dir_fops,
};

static int __init rs485_dir_init(void)
{
    int ret;
    int receive_level;

    if (!gpio_is_valid(gpio_num)) {
        pr_err("%s: invalid GPIO number %d\n",
               RS485_DIR_DEVICE_NAME, gpio_num);
        return -EINVAL;
    }

    ret = gpio_request(gpio_num, RS485_DIR_DEVICE_NAME);
    if (ret) {
        pr_err("%s: gpio_request(%d) failed: %d; check pinmux/DT conflicts\n",
               RS485_DIR_DEVICE_NAME, gpio_num, ret);
        return ret;
    }
    gpio_requested = true;

    receive_level = active_low ? 1 : 0;
    ret = gpio_direction_output(gpio_num, receive_level);
    if (ret) {
        pr_err("%s: gpio_direction_output(%d) failed: %d\n",
               RS485_DIR_DEVICE_NAME, gpio_num, ret);
        goto err_free_gpio;
    }

    ret = misc_register(&rs485_dir_miscdev);
    if (ret) {
        pr_err("%s: misc_register failed: %d\n",
               RS485_DIR_DEVICE_NAME, ret);
        goto err_safe_rx;
    }

    pr_info("%s: registered /dev/%s on GPIO%d (GPIO2_B1/A35), active_%s\n",
            RS485_DIR_DEVICE_NAME,
            RS485_DIR_DEVICE_NAME,
            gpio_num,
            active_low ? "low" : "high");
    return 0;

err_safe_rx:
    rs485_dir_set(false);
err_free_gpio:
    gpio_free(gpio_num);
    gpio_requested = false;
    return ret;
}

static void __exit rs485_dir_exit(void)
{
    misc_deregister(&rs485_dir_miscdev);

    if (gpio_requested) {
        rs485_dir_set(false);
        gpio_free(gpio_num);
        gpio_requested = false;
    }

    pr_info("%s: unloaded\n", RS485_DIR_DEVICE_NAME);
}

module_init(rs485_dir_init);
module_exit(rs485_dir_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YCE");
MODULE_DESCRIPTION("RK3566 GPIO2_B1 RS485 DE/RE direction control driver");
MODULE_VERSION("2.0");
