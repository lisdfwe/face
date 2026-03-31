#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/delay.h>

#define MOTOR_GPIO_NUM 4
#define DRV_NAME "motor"

/* 命令定义 */
#define MOTOR_MAGIC 'M'
#define CMD_OPEN  _IO(MOTOR_MAGIC, 1)
#define CMD_CLOSE _IO(MOTOR_MAGIC, 2)
#define CMD_FEED  _IO(MOTOR_MAGIC, 3) /* 心跳 */

struct motor_dev {
    struct gpio_desc *gpios[MOTOR_GPIO_NUM];
    struct task_struct *thread;
    struct timer_list timer;
    wait_queue_head_t wq;
    struct spinlock_t lock; 
    
    int steps;      /* 任务：步数 */
    int done;       /* 状态：完成标志 (用于 poll) */
    int phase;      /* 当前相位 */
};

static struct motor_dev *g_dev;
static const int seq[8] = {0x02, 0x03, 0x01, 0x09, 0x08, 0x0C, 0x04, 0x06};

/* 1. 执行转动 (运行在线程中，允许 msleep) */
static void run_motor(int steps, int delay)
{
    int i, dir = (steps > 0) ? 1 : -1;
    int count = (steps > 0) ? steps : -steps;

    for (i = 0; i < count; i++) {
        for (int j = 0; j < MOTOR_GPIO_NUM; j++)
            gpiod_set_value(g_dev->gpios[j], (seq[g_dev->phase] >> j) & 1);
        
        msleep(delay); /* 【考点】只有在这里才能睡眠 */
        
        g_dev->phase = (g_dev->phase + dir + 8) % 8;
    }
    /* 关闭引脚 */
    for (int j = 0; j < MOTOR_GPIO_NUM; j++) gpiod_set_value(g_dev->gpios[j], 0);
}

/* 2. 内核线程 (消费者) */
static int thread_fn(void *data)
{
    while (!kthread_should_stop()) {
        wait_event_interruptible(g_dev->wq, g_dev->steps != 0 || kthread_should_stop());
        if (kthread_should_stop()) break;

        mutex_lock(&g_dev->lock);
        if (g_dev->steps) {
            int s = g_dev->steps;
            g_dev->steps = 0; /* 取走任务 */
            mutex_unlock(&g_dev->lock);

            run_motor(s, 2); /* 执行耗时任务 */

            /* 任务完成，唤醒 poll */
            g_dev->done = 1;
            wake_up_interruptible(&g_dev->wq);
        } else {
            mutex_unlock(&g_dev->lock);
        }
    }
    return 0;
}

/* 3. 定时器回调 (软中断上下文，严禁睡眠) */
static void timer_cb(struct timer_list *t)
{
    /* 【考点】心跳超时，自动下发关门任务 (-200 步) */
    pr_warn("Motor: Heartbeat lost! Auto closing.\n");
    mutex_lock(&g_dev->lock);
    if (g_dev->steps == 0) { /* 仅当空闲时触发 */
        g_dev->steps = -200;
        wake_up_interruptible(&g_dev->wq);
    }
    mutex_unlock(&g_dev->lock);
    /* 注意：超时后不再自动重启，等待应用层重新喂狗 */
}

/* 4. IOCTL & Poll */
static long motor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case CMD_OPEN:
        mutex_lock(&g_dev->lock); g_dev->steps = 200; mutex_unlock(&g_dev->lock);
        wake_up_interruptible(&g_dev->wq);
        mod_timer(&g_dev->timer, jiffies + 5 * HZ); /* 喂狗 */
        break;
    case CMD_CLOSE:
        mutex_lock(&g_dev->lock); g_dev->steps = -200; mutex_unlock(&g_dev->lock);
        wake_up_interruptible(&g_dev->wq);
        del_timer_sync(&g_dev->timer); /* 手动关闭时停定时器 */
        break;
    case CMD_FEED:
        mod_timer(&g_dev->timer, jiffies + 5 * HZ); /* 应用层主动喂狗 */
        break;
    }
    return 0;
}

static __poll_t motor_poll(struct file *f, poll_table *wait)
{
    poll_wait(f, &g_dev->wq, wait);
    return g_dev->done ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = motor_ioctl,
    .poll = motor_poll,
};

/* 5. Probe / Remove */
static int probe(struct platform_device *pdev)
{
    int i;
    g_dev = devm_kzalloc(&pdev->dev, sizeof(*g_dev), GFP_KERNEL);
    if (!g_dev) return -ENOMEM;

    mutex_init(&g_dev->lock);
    init_waitqueue_head(&g_dev->wq);
    timer_setup(&g_dev->timer, timer_cb, 0);

    for (i = 0; i < MOTOR_GPIO_NUM; i++) {
        g_dev->gpios[i] = devm_gpiod_get_index(&pdev->dev, "motor", i, GPIOD_OUT_LOW);
        if (IS_ERR(g_dev->gpios[i])) return PTR_ERR(g_dev->gpios[i]);
    }

    g_dev->thread = kthread_run(thread_fn, NULL, "motor_thread");
    mod_timer(&g_dev->timer, jiffies + 5 * HZ); /* 启动看门狗 */

    g_dev->major = register_chrdev(0, DRV_NAME, &fops);
    g_dev->class = class_create(THIS_MODULE, "motor_cls");
    g_dev->device = device_create(g_dev->class, NULL, MKDEV(g_dev->major, 0), NULL, "motor");

    return 0;
}

static int remove(struct platform_device *pdev)
{
    del_timer_sync(&g_dev->timer);
    kthread_stop(g_dev->thread);
    device_destroy(g_dev->class, MKDEV(g_dev->major, 0));
    class_destroy(g_dev->class);
    unregister_chrdev(g_dev->major, DRV_NAME);
    return 0;
}

static const struct of_device_id match[] = { { .compatible = "face,motor" }, {} };
MODULE_DEVICE_TABLE(of, match);

static struct platform_driver driver = {
    .probe = probe,
    .remove = remove,
    .driver = { .name = DRV_NAME, .of_match_table = match },
};

module_platform_driver(driver);
MODULE_LICENSE("GPL");在这段代码中，我并没有看到中断是如何处理，也没有看到调用irq_handle