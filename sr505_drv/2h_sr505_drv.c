/*
 * sr505_drv.c
 *
 * HC-SR505 PIR driver for RK3568 GPIO1_B2.
 *
 * GPIO1_B2 = 1 * 32 + 1 * 8 + 2 = 42
 *
 * 说明：
 * 1. 不依赖设备树，驱动中直接固定 GPIO42。
 * 2. 监听 SR505 OUT 引脚的上升沿和下降沿。
 * 3. 通过 /dev/sr505 向应用层返回当前 OUT 状态：
 *      0 = LOW，无新的运动触发
 *      1 = HIGH，检测到人体运动或处于硬件保持时间内
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/string.h>

#define DRIVER_NAME "sr505"
#define DEVICE_NAME "sr505"

/* RK3568 GPIO1_B2 */
#define SR505_GPIO  42

struct sr505_device {
    int gpio;
    int irq;

    /*
     * SR505 当前 OUT 状态：
     * 0 = 低电平
     * 1 = 高电平
     */
    atomic_t state;

    /*
     * 是否有新事件等待应用读取：
     * 0 = 没有新事件
     * 1 = 有新事件
     */
    atomic_t pending;

    /* read()/poll() 阻塞等待队列 */
    wait_queue_head_t wait;

    /* 自动创建 /dev/sr505 */
    struct miscdevice miscdev;
};

static struct sr505_device g_sr505;


/*
 * GPIO 中断线程
 *
 * SR505 OUT 发生变化时进入这里：
 * 0 -> 1：检测到人体运动
 * 1 -> 0：SR505 硬件保持时间结束
 */
static irqreturn_t sr505_irq_thread(int irq, void *dev_id)
{
    struct sr505_device *dev = dev_id;
    int state = !!gpio_get_value(dev->gpio);
    int old_state = atomic_read(&dev->state);

    /* 状态没变，直接忽略 */
    if (state == old_state)
        return IRQ_HANDLED;

    atomic_set(&dev->state, state);
    atomic_set(&dev->pending, 1);

    /* 唤醒阻塞在 read()/poll() 的应用程序 */
    wake_up_interruptible(&dev->wait);

    pr_info(DRIVER_NAME ": OUT %d -> %d, %s\n",
            old_state, state,
            state ? "motion detected" : "motion ended");

    return IRQ_HANDLED;
}


/*
 * 应用层 read(/dev/sr505) 时进入这里。
 *
 * 返回 1 个字节：
 * 0x00 = SR505 OUT 低电平
 * 0x01 = SR505 OUT 高电平
 */
static ssize_t sr505_read(struct file *file, char __user *buf,
                          size_t count, loff_t *ppos)
{
    struct miscdevice *mdev = file->private_data;
    struct sr505_device *dev =
        container_of(mdev, struct sr505_device, miscdev);

    unsigned char state;
    int ret;

    if (count < 1)
        return -EINVAL;

    /*
     * 没有新事件：
     * - 非阻塞模式直接返回 -EAGAIN
     * - 阻塞模式等待中断唤醒
     */
    if (!atomic_read(&dev->pending)) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        ret = wait_event_interruptible(
            dev->wait,
            atomic_read(&dev->pending));

        if (ret)
            return ret;
    }

    /*
     * 清除 pending 标记。
     * 注意：这里返回的是当前最新 OUT 状态，不做事件队列缓存。
     */
    atomic_set(&dev->pending, 0);

    state = atomic_read(&dev->state) ? 1 : 0;

    if (copy_to_user(buf, &state, 1))
        return -EFAULT;

    return 1;
}


/*
 * poll/select/epoll 支持。
 *
 * 应用层可以阻塞等待 /dev/sr505 可读，
 * 避免 while 循环一直轮询 GPIO。
 */
static __poll_t sr505_poll(struct file *file, poll_table *wait)
{
    struct miscdevice *mdev = file->private_data;
    struct sr505_device *dev =
        container_of(mdev, struct sr505_device, miscdev);

    poll_wait(file, &dev->wait, wait);

    if (atomic_read(&dev->pending))
        return POLLIN | POLLRDNORM;

    return 0;
}


static const struct file_operations sr505_fops = {
    .owner = THIS_MODULE,
    .read  = sr505_read,
    .poll  = sr505_poll,
};


/*
 * 模块加载入口
 *
 * 主要流程：
 * 1. 申请 GPIO42
 * 2. 配置为输入
 * 3. 读取初始电平
 * 4. GPIO 转 IRQ
 * 5. 申请双边沿中断
 * 6. 注册 /dev/sr505
 */
static int __init sr505_init(void)
{
    struct sr505_device *dev = &g_sr505;
    int ret, state;

    memset(dev, 0, sizeof(*dev));

    dev->gpio = SR505_GPIO;

    pr_info(DRIVER_NAME
            ": loading, GPIO=%d GPIO1_B2\n",
            dev->gpio);

    if (!gpio_is_valid(dev->gpio)) {
        pr_err(DRIVER_NAME
               ": invalid GPIO %d\n",
               dev->gpio);

        return -EINVAL;
    }

    ret = gpio_request(dev->gpio, DRIVER_NAME "_gpio");
    if (ret) {
        pr_err(DRIVER_NAME
               ": gpio_request failed: %d\n",
               ret);

        return ret;
    }

    ret = gpio_direction_input(dev->gpio);
    if (ret) {
        pr_err(DRIVER_NAME
               ": gpio_direction_input failed: %d\n",
               ret);

        goto err_gpio;
    }

    /*
     * 读取初始状态。
     * pending 设置为 1，是为了让应用启动后
     * 能立即读到当前 OUT 初值。
     */
    state = !!gpio_get_value(dev->gpio);

    atomic_set(&dev->state, state);
    atomic_set(&dev->pending, 1);

    init_waitqueue_head(&dev->wait);

    dev->irq = gpio_to_irq(dev->gpio);
    if (dev->irq < 0) {
        ret = dev->irq;

        pr_err(DRIVER_NAME
               ": gpio_to_irq failed: %d\n",
               ret);

        goto err_gpio;
    }

    /*
     * 同时监听上升沿和下降沿：
     * 上升沿：OUT 0 -> 1，检测到人体运动
     * 下降沿：OUT 1 -> 0，硬件保持时间结束
     */
    ret = request_threaded_irq(
        dev->irq,
        NULL,
        sr505_irq_thread,
        IRQF_TRIGGER_RISING |
        IRQF_TRIGGER_FALLING |
        IRQF_ONESHOT,
        DRIVER_NAME,
        dev);

    if (ret) {
        pr_err(DRIVER_NAME
               ": request_threaded_irq failed: %d\n",
               ret);

        goto err_gpio;
    }

    dev->miscdev.minor = MISC_DYNAMIC_MINOR;
    dev->miscdev.name  = DEVICE_NAME;
    dev->miscdev.fops  = &sr505_fops;

    ret = misc_register(&dev->miscdev);
    if (ret) {
        pr_err(DRIVER_NAME
               ": misc_register failed: %d\n",
               ret);

        goto err_irq;
    }

    pr_info(DRIVER_NAME
            ": loaded, /dev/%s, GPIO%d (GPIO1_B2) -> IRQ%d, initial OUT=%d\n",
            DEVICE_NAME,
            dev->gpio,
            dev->irq,
            state);

    return 0;


err_irq:
    free_irq(dev->irq, dev);

err_gpio:
    gpio_free(dev->gpio);

    return ret;
}


/*
 * 模块卸载入口
 */
static void __exit sr505_exit(void)
{
    struct sr505_device *dev = &g_sr505;

    misc_deregister(&dev->miscdev);

    free_irq(dev->irq, dev);

    gpio_free(dev->gpio);

    pr_info(DRIVER_NAME ": unloaded\n");
}


module_init(sr505_init);
module_exit(sr505_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION(
    "HC-SR505 PIR driver for RK3568 GPIO1_B2");
MODULE_VERSION("1.1");



