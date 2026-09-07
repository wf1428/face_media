#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/uaccess.h>

#define DEVICE_NAME     "led"
#define LED_GPIO        81      /* GPIO2_C1 */

static ssize_t led_write(struct file *file,
                         const char __user *buf,
                         size_t count,
                         loff_t *ppos)
{
    char value;

    if (count < 1)
        return -EINVAL;

    if (copy_from_user(&value, buf, 1))
        return -EFAULT;

    switch (value) {
    case '1':
        gpio_set_value(LED_GPIO, 1);
        printk(KERN_INFO "led: ON\n");
        break;

    case '0':
        gpio_set_value(LED_GPIO, 0);
        printk(KERN_INFO "led: OFF\n");
        break;

    default:
        printk(KERN_WARNING "led: invalid value, use 0 or 1\n");
        return -EINVAL;
    }

    return count;
}

static ssize_t led_read(struct file *file,
                        char __user *buf,
                        size_t count,
                        loff_t *ppos)
{
    char value[2];

    if (*ppos != 0)
        return 0;

    value[0] = gpio_get_value(LED_GPIO) ? '1' : '0';
    value[1] = '\n';

    if (count < 2)
        return -EINVAL;

    if (copy_to_user(buf, value, 2))
        return -EFAULT;

    *ppos += 2;

    return 2;
}

static const struct file_operations led_fops = {
    .owner = THIS_MODULE,
    .write = led_write,
    .read  = led_read,
};

static struct miscdevice led_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEVICE_NAME,
    .fops  = &led_fops,
};

static int __init led_init(void)
{
    int ret;

    printk(KERN_INFO "led: driver init, GPIO=%d (GPIO2_C1)\n",
           LED_GPIO);

    /* 1. 申请 GPIO */
    ret = gpio_request(LED_GPIO, "led_gpio");
    if (ret) {
        printk(KERN_ERR "led: gpio_request(%d) failed: %d\n",
               LED_GPIO, ret);
        return ret;
    }

    /*
     * 2. 配置为输出
     * 默认输出低电平，即 LED 关闭
     */
    ret = gpio_direction_output(LED_GPIO, 0);
    if (ret) {
        printk(KERN_ERR "led: gpio_direction_output failed: %d\n",
               ret);
        gpio_free(LED_GPIO);
        return ret;
    }

    /* 3. 注册字符设备 /dev/led */
    ret = misc_register(&led_miscdev);
    if (ret) {
        printk(KERN_ERR "led: misc_register failed: %d\n", ret);
        gpio_free(LED_GPIO);
        return ret;
    }

    printk(KERN_INFO "led: registered /dev/%s\n", DEVICE_NAME);

    return 0;
}

static void __exit led_exit(void)
{
    /* 退出驱动前关闭 LED */
    gpio_set_value(LED_GPIO, 0);

    misc_deregister(&led_miscdev);
    gpio_free(LED_GPIO);

    printk(KERN_INFO "led: driver exit\n");
}

module_init(led_init);
module_exit(led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LED");
MODULE_DESCRIPTION("GPIO2_C1 LED Driver");
MODULE_VERSION("1.0");

