#include <linux/module.h>        // 内核模块基础头文件
#include <linux/platform_device.h> // platform 驱动框架
#include <linux/gpio/consumer.h> // GPIO 描述符接口（推荐新API）
#include <linux/fs.h>            // 字符设备相关接口
#include <linux/device.h>        // 设备模型（class/device）
#include <linux/uaccess.h>       // 用户空间数据访问
#include <linux/kthread.h>       // 内核线程
#include <linux/wait.h>          // 等待队列
#include <linux/poll.h>          // poll/select 机制
#include <linux/timer.h>         // 内核定时器
#include <linux/slab.h>          // 内核内存管理
#include <linux/mutex.h>         // 互斥锁
#include <linux/delay.h>         // 延时函数 msleep

#define MOTOR_GPIO_NUM 4         // 步进电机控制GPIO数量（4相）

/* IOCTL 命令定义 */
#define MOTOR_MAGIC 'M'          // ioctl 类型标识
#define MOTOR_OPEN   _IO(MOTOR_MAGIC, 1)   // 开门命令
#define MOTOR_CLOSE  _IO(MOTOR_MAGIC, 2)   // 关门命令

/* 电机设备结构体：保存驱动的所有资源 */
struct motor_dev {
    struct gpio_desc *gpios[MOTOR_GPIO_NUM]; // 4个GPIO描述符（控制电机线圈）

    wait_queue_head_t wq;   // 等待队列（用于 poll 或线程同步）
    int event_flag;         // 事件标志位（通知用户空间或线程）

    struct timer_list close_timer;  // 定时器：用于30秒自动关门

    struct task_struct *thread;     // 内核线程（后台任务）

    struct mutex lock;      // 互斥锁：防止 ioctl 与 timer 同时操作电机

    int index_pos;          // 当前步进相位（0~7）

    int major;              // 字符设备主设备号
    struct class *class;    // 设备类
    struct device *device;  // 设备对象（生成 /dev/motor）
};

static struct motor_dev *g_motor; // 全局电机设备

/* 步进电机8拍驱动序列（半步模式）
   每个值的二进制对应4个GPIO电平 */
static int seq[8] = {0x2, 0x3, 0x1, 0x9, 0x8, 0xc, 0x4, 0x6};


/**
 * motor_set_pins - 设置GPIO电平
 * val 的二进制每一位对应一个GPIO输出
 */
static void motor_set_pins(int val)
{
    int i;

    // 依次设置4个GPIO
    for (i = 0; i < MOTOR_GPIO_NUM; i++)
        gpiod_set_value(g_motor->gpios[i], (val >> i) & 1);
}


/**
 * motor_rotate - 控制电机旋转
 * step：旋转步数
 * delay_ms：每一步延时
 */
static void motor_rotate(int step, int delay_ms)
{
    int i;

    int steps_to_move = (step > 0) ? step : -step; // 取绝对值

    // 根据step决定旋转方向
    int direction = (step > 0) ? -1 : 1;

    // 加锁，防止timer和ioctl同时操作电机
    mutex_lock(&g_motor->lock);

    for (i = 0; i < steps_to_move; i++) {

        // 根据当前相位输出GPIO
        motor_set_pins(seq[g_motor->index_pos]);

        // 延时控制转速
        msleep(delay_ms);

        // 更新相位
        g_motor->index_pos += direction;

        // 保证相位在 0~7 循环
        if (g_motor->index_pos < 0)
            g_motor->index_pos = 7;

        if (g_motor->index_pos > 7)
            g_motor->index_pos = 0;
    }

    // 停止电机，释放线圈电流（防止发热）
    motor_set_pins(0);

    mutex_unlock(&g_motor->lock);
}


/* 定时器回调函数
   4.9内核 timer 回调参数为 unsigned long */
static void close_timer_func(unsigned long data)
{
    struct motor_dev *dev = (struct motor_dev *)data;

    pr_info("Motor: Auto-closing door via timer\n");

    // 反转200步（关门）
    motor_rotate(-200, 2);

    // 设置事件标志
    dev->event_flag = 1;

    // 唤醒等待队列（通知用户空间或线程）
    wake_up_interruptible(&dev->wq);
}


/* 内核线程：处理后台任务 */
static int motor_thread(void *data)
{
    while (!kthread_should_stop()) {

        // 等待事件发生
        if (wait_event_interruptible(
                g_motor->wq,
                g_motor->event_flag || kthread_should_stop()))
            continue;

        if (kthread_should_stop())
            break;

        // 清除事件标志
        g_motor->event_flag = 0;

        pr_debug("Motor: Background thread processed event\n");
    }

    return 0;
}


/* ioctl 控制接口
   用户空间通过 ioctl 控制电机 */
static long motor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {

    case MOTOR_OPEN:   // 开门命令

        pr_info("Motor: Opening door\n");

        // 电机正转200步
        motor_rotate(200, 2);

        /* 设置30秒后自动关门 */
        mod_timer(&g_motor->close_timer, jiffies + 30 * HZ);

        break;


    case MOTOR_CLOSE:  // 手动关门

        pr_info("Motor: Closing door manually\n");

        // 取消自动关门定时器
        del_timer(&g_motor->close_timer);

        // 电机反转
        motor_rotate(-200, 2);

        break;


    default:
        return -EINVAL;
    }

    return 0;
}


/* poll 接口：支持 select/poll/epoll */
static unsigned int motor_poll(struct file *file, poll_table *wait)
{
    unsigned int mask = 0;

    // 将当前进程加入等待队列
    poll_wait(file, &g_motor->wq, wait);

    // 如果有事件发生
    if (g_motor->event_flag)
        mask |= POLLIN | POLLRDNORM;

    return mask;
}


/* 字符设备操作函数 */
static const struct file_operations motor_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = motor_ioctl, // ioctl控制
    .poll = motor_poll,            // poll机制
};


/* probe函数：驱动初始化 */
static int motor_probe(struct platform_device *pdev)
{
    int i, ret;

    struct device *dev = &pdev->dev;

    // 分配设备结构体
    g_motor = devm_kzalloc(dev, sizeof(*g_motor), GFP_KERNEL);
    if (!g_motor)
        return -ENOMEM;

    // 初始化互斥锁
    mutex_init(&g_motor->lock);

    // 初始化等待队列
    init_waitqueue_head(&g_motor->wq);

    // 获取设备树中的GPIO
    for (i = 0; i < MOTOR_GPIO_NUM; i++) {

        g_motor->gpios[i] = devm_gpiod_get_index(dev, "motor", i, GPIOD_OUT_LOW);

        if (IS_ERR(g_motor->gpios[i])) {
            dev_err(dev, "Failed to get GPIO %d\n", i);
            return PTR_ERR(g_motor->gpios[i]);
        }
    }

    /* 初始化定时器（适配Linux4.9） */
    init_timer(&g_motor->close_timer);
    g_motor->close_timer.function = close_timer_func;
    g_motor->close_timer.data = (unsigned long)g_motor;

    // 创建内核线程
    g_motor->thread = kthread_run(motor_thread, NULL, "motor_kthread");

    // 注册字符设备
    g_motor->major = register_chrdev(0, "motor_dev", &motor_fops);
    if (g_motor->major < 0)
        return g_motor->major;

    // 创建设备类
    g_motor->class = class_create(THIS_MODULE, "motor_class");
    if (IS_ERR(g_motor->class)) {
        ret = PTR_ERR(g_motor->class);
        goto err_reg;
    }

    // 创建设备节点 /dev/motor
    g_motor->device = device_create(
        g_motor->class,
        NULL,
        MKDEV(g_motor->major, 0),
        NULL,
        "motor"
    );

    if (IS_ERR(g_motor->device)) {
        ret = PTR_ERR(g_motor->device);
        goto err_cls;
    }

    dev_info(dev, "Motor driver for Kernel 4.9 initialized\n");

    return 0;

err_cls:
    class_destroy(g_motor->class);

err_reg:
    unregister_chrdev(g_motor->major, "motor_dev");

    return ret;
}


/* 驱动卸载 */
static int motor_remove(struct platform_device *pdev)
{
    // 删除定时器
    del_timer_sync(&g_motor->close_timer);

    // 停止线程
    if (g_motor->thread)
        kthread_stop(g_motor->thread);

    // 删除设备
    device_destroy(g_motor->class, MKDEV(g_motor->major, 0));

    class_destroy(g_motor->class);

    // 注销字符设备
    unregister_chrdev(g_motor->major, "motor_dev");

    dev_info(&pdev->dev, "Motor driver removed\n");

    return 0;
}


/* 设备树匹配表 */
static const struct of_device_id motor_of_match[] = {
    { .compatible = "face,motor" },
    { }
};

MODULE_DEVICE_TABLE(of, motor_of_match);


/* platform 驱动结构体 */
static struct platform_driver motor_driver = {

    .probe = motor_probe,    // 驱动加载
    .remove = motor_remove,  // 驱动卸载

    .driver = {
        .name = "face_motor",
        .of_match_table = motor_of_match,
    },
};


/* 注册platform驱动 */
module_platform_driver(motor_driver);

MODULE_LICENSE("GPL");