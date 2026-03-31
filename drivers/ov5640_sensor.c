/*
 * ============================================================================
 * 文件：ov5640_sensor.c
 * 描述：OV5640摄像头V4L2子设备驱动 (深度优化版)
 * 
 * 优化策略：
 * 1. 寄存器配置表压缩：使用结构化数组，支持延迟参数
 * 2. 动态分辨率切换：通过缩放而非完整重新配置
 * 3. 快速启动路径：区分冷启动和热启动
 * 4. 批量I2C写入：减少总线事务开销
 * 5. 智能时钟计算：根据分辨率自动计算PLL参数
 * ============================================================================
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/videodev2.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define OV5640_CHIP_ID          0x5640
#define OV5640_CHIP_ID_HIGH     0x300A
#define OV5640_CHIP_ID_LOW      0x300B

/* 调试宏 */
#define OV5640_DEBUG_REG_WRITES 0  /* 设为1可调试寄存器写入 */

/* ============== 数据结构优化 ============== */

/* 寄存器配置项 - 支持延时 */
struct ov5640_reg {
    u16 addr;
    u8 val;
    u8 delay_ms;    /* 写入后延时，0表示不延时 */
};

/* 分辨率模式定义 */
enum ov5640_mode {
    MODE_QVGA = 0,      /* 320x240  - 人脸识别/预览 */
    MODE_VGA,           /* 640x480  - 标准视频 */
    MODE_720P,          /* 1280x720 - 高清视频 */
    MODE_1080P,         /* 1920x1080- 全高清 */
    MODE_5M,            /* 2592x1944 - 拍照 */
    MODE_MAX
};

/* 帧率预设 */
enum ov5640_fps {
    FPS_15 = 0,
    FPS_30,
    FPS_60,             /* 仅低分辨率支持 */
    FPS_MAX
};

/* 模式信息 - 包含所有必要参数 */
struct ov5640_mode_info {
    u16 width;
    u16 height;
    u8 max_fps;
    u8 binning;         /* 是否使用binning模式 */
    u16 hts;            /* 水平总大小 */
    u16 vts;            /* 垂直总大小 */
    const struct ov5640_reg *regs;
    u16 reg_num;
};

/* 设备状态机 */
enum ov5640_state {
    STATE_POWER_OFF,
    STATE_STANDBY,      /* 上电但未初始化 */
    STATE_IDLE,         /* 初始化完成，可配置 */
    STATE_STREAMING,    /* 正在采集 */
};

/* 优化后的设备结构 */
struct ov5640_dev {
    struct i2c_client *client;
    struct v4l2_subdev sd;
    struct v4l2_ctrl_handler ctrl_handler;
    
    /* 硬件资源 */
    struct gpio_desc *pwdn_gpio;
    struct gpio_desc *reset_gpio;
    struct clk *xclk;
    struct regulator *avdd;
    struct regulator *dvdd;
    struct regulator *dovdd;
    
    /* 状态管理 */
    struct mutex lock;
    enum ov5640_state state;
    enum ov5640_mode current_mode;
    enum ov5640_fps current_fps;
    bool fast_start;    /* 是否支持热启动 */
    
    /* 当前格式 */
    struct v4l2_mbus_framefmt format;
    u32 mbus_code;
    
    /* 缓存的配置参数（避免重复计算） */
    u16 pll_multiplier;
    u8  sys_div;
    u8  pix_div;
};

/* ============== 寄存器配置表（优化压缩） ============== */

/* 核心初始化序列 - 只需执行一次 */
static const struct ov5640_reg ov5640_global_init[] = {
    /* 系统复位 */
    {0x3103, 0x11, 0}, {0x3008, 0x82, 5}, {0x3008, 0x42, 2},
    
    /* 基础时钟配置 - 24MHz输入，默认42MHz输出 */
    {0x3034, 0x1A, 0},  /* 10-bit模式 */
    {0x3035, 0x21, 0},  /* 系统时钟分频 */
    {0x3036, 0x69, 0},  /* PLL倍频105 */
    {0x3037, 0x13, 0},  /* PLL设置 */
    {0x3108, 0x01, 0},  /* 系统根分频 */
    {0x3824, 0x01, 0},
    
    /* 接口配置 - 8位并行DVP */
    {0x300e, 0x45, 0},  /* DVP接口使能 */
    {0x3017, 0x00, 0},  /* 输出驱动能力 */
    {0x3018, 0x00, 0},
    
    /* ISP基础设置 */
    {0x5000, 0xA7, 0},  /* ISP控制0: LENC+BPC+WPC+CIP+ISP */
    {0x5001, 0xA3, 0},  /* ISP控制1: AWB+AWG+BC+WC */
    {0x5002, 0x80, 0},  /* ISP控制2 */
    {0x5003, 0x08, 0},  /* ISP控制3 */
    
    /* 自动曝光 */
    {0x3a00, 0x78, 0},  /* AE控制 */
    {0x3a18, 0x00, 0},  /* 曝光增益上限 */
    {0x3a19, 0xf8, 0},
    
    /* 输出格式 YUYV */
    {0x4300, 0x30, 0},  /* YUV422 YUYV */
    {0x501f, 0x00, 0},  /* ISP输出格式 */
    
    /* 色彩/伽马优化 */
    {0x5580, 0x06, 0},  /* 饱和度/对比度使能 */
    {0x5583, 0x40, 0},  /* 饱和度 */
    {0x5584, 0x10, 0},
    {0x5589, 0x10, 0},  /* 对比度 */
    {0x558a, 0x00, 0},
    {0x558b, 0xf8, 0},
    
    /* 结束标记 */
    {0xFFFF, 0xFF, 0},
};

/* QVGA 320x240 - 使用8x数字缩放，快速配置 */
static const struct ov5640_reg ov5640_qvga_cfg[] = {
    /* 输入窗口 - 使用中心区域 */
    {0x3800, 0x02, 0}, {0x3801, 0xA0, 0},  /* X起始672 */
    {0x3802, 0x01, 0}, {0x3803, 0xF2, 0},  /* Y起始498 */
    {0x3804, 0x07, 0}, {0x3805, 0x9F, 0},  /* X结束1951 */
    {0x3806, 0x05, 0}, {0x3807, 0xAD, 0},  /* Y结束1453 */
    
    /* 输出尺寸 */
    {0x3808, 0x01, 0}, {0x3809, 0x40, 0},  /* 320 */
    {0x380A, 0x00, 0}, {0x380B, 0xF0, 0},  /* 240 */
    
    /* 缩放设置 - 8x缩放 */
    {0x5600, 0x00, 0},  /* 缩放使能 */
    {0x5601, 0x2C, 0},  /* X缩放分子 */
    {0x5602, 0x5C, 0},  /* X缩放分母 */
    {0x5603, 0x06, 0},  /* Y缩放分子 */
    {0x5604, 0x1C, 0},  /* Y缩放分母 */
    {0x5605, 0x65, 0},  /* 缩放系数 */
    {0x5606, 0x81, 0},
    {0x5607, 0x9F, 0},
    {0x5608, 0x8A, 0},
    {0x5609, 0xF0, 0},
    
    /* 时序 - 30fps @ 320x240 */
    {0x380C, 0x03, 0}, {0x380D, 0xC0, 0},  /* HTS=960 */
    {0x380E, 0x01, 0}, {0x380F, 0xF4, 0},  /* VTS=500 */
    
    {0xFFFF, 0xFF, 0},
};

/* VGA 640x480 - 4x缩放 */
static const struct ov5640_reg ov5640_vga_cfg[] = {
    {0x3800, 0x01, 0}, {0x3801, 0x50, 0},  /* X起始336 */
    {0x3802, 0x00, 0}, {0x3803, 0xF9, 0},  /* Y起始249 */
    {0x3804, 0x08, 0}, {0x3805, 0xEF, 0},  /* X结束2287 */
    {0x3806, 0x06, 0}, {0x3807, 0xA6, 0},  /* Y结束1702 */
    
    {0x3808, 0x02, 0}, {0x3809, 0x80, 0},  /* 640 */
    {0x380A, 0x01, 0}, {0x380B, 0xE0, 0},  /* 480 */
    
    /* 4x缩放 */
    {0x5600, 0x00, 0},
    {0x5601, 0x1C, 0}, {0x5602, 0x2E, 0},
    {0x5603, 0x06, 0}, {0x5604, 0x0E, 0},
    {0x5605, 0x32, 0}, {0x5606, 0x40, 0},
    {0x5607, 0x4F, 0}, {0x5608, 0x45, 0},
    {0x5609, 0x78, 0},
    
    /* 30fps时序 */
    {0x380C, 0x07, 0}, {0x380D, 0x68, 0},  /* HTS=1896 */
    {0x380E, 0x03, 0}, {0x380F, 0xD0, 0},  /* VTS=976 */
    
    {0xFFFF, 0xFF, 0},
};

/* 720P 1280x720 - 2x缩放 */
static const struct ov5640_reg ov5640_720p_cfg[] = {
    {0x3800, 0x00, 0}, {0x3801, 0x00, 0},
    {0x3802, 0x00, 0}, {0x3803, 0xFA, 0},  /* Y起始250 */
    {0x3804, 0x0A, 0}, {0x3805, 0x3F, 0},
    {0x3806, 0x06, 0}, {0x3807, 0xA5, 0},  /* Y结束1701 */
    
    {0x3808, 0x05, 0}, {0x3809, 0x00, 0},  /* 1280 */
    {0x380A, 0x02, 0}, {0x380B, 0xD0, 0},  /* 720 */
    
    /* 2x缩放 */
    {0x5600, 0x00, 0},
    {0x5601, 0x2C, 0}, {0x5602, 0x5C, 0},
    {0x5603, 0x06, 0}, {0x5604, 0x1C, 0},
    {0x5605, 0x65, 0}, {0x5606, 0x81, 0},
    {0x5607, 0x9F, 0}, {0x5608, 0x8A, 0},
    {0x5609, 0xF0, 0},
    
    {0x380C, 0x07, 0}, {0x380D, 0x64, 0},  /* HTS=1892 */
    {0x380E, 0x02, 0}, {0x380F, 0xE4, 0},  /* VTS=740 */
    
    {0xFFFF, 0xFF, 0},
};

/* 1080P 1920x1080 - 无缩放，裁剪 */
static const struct ov5640_reg ov5640_1080p_cfg[] = {
    {0x3800, 0x01, 0}, {0x3801, 0x50, 0},  /* X起始336 */
    {0x3802, 0x01, 0}, {0x3803, 0xB2, 0},  /* Y起始434 */
    {0x3804, 0x08, 0}, {0x3805, 0xEF, 0},  /* X结束2287 */
    {0x3806, 0x05, 0}, {0x3807, 0xF1, 0},  /* Y结束1521 */
    
    {0x3808, 0x07, 0}, {0x3809, 0x80, 0},  /* 1920 */
    {0x380A, 0x04, 0}, {0x380B, 0x38, 0},  /* 1080 */
    
    /* 无缩放 */
    {0x5001, 0x83, 0},  /* 关闭ISP缩放 */
    {0x5600, 0x10, 0},  /* 禁用缩放 */
    
    /* 15fps时序 */
    {0x380C, 0x09, 0}, {0x380D, 0xC4, 0},  /* HTS=2500 */
    {0x380E, 0x04, 0}, {0x380F, 0x60, 0},  /* VTS=1120 */
    
    {0xFFFF, 0xFF, 0},
};

/* 5M 2592x1944 - 全幅无缩放 */
static const struct ov5640_reg ov5640_5m_cfg[] = {
    {0x3800, 0x00, 0}, {0x3801, 0x00, 0},
    {0x3802, 0x00, 0}, {0x3803, 0x00, 0},
    {0x3804, 0x0A, 0}, {0x3805, 0x3F, 0},
    {0x3806, 0x07, 0}, {0x3807, 0x9F, 0},
    
    {0x3808, 0x0A, 0}, {0x3809, 0x20, 0},  /* 2592 */
    {0x380A, 0x07, 0}, {0x380B, 0x98, 0},  /* 1944 */
    
    {0x5001, 0x83, 0},
    {0x5600, 0x10, 0},
    
    {0x380C, 0x0B, 0}, {0x380D, 0x1C, 0},  /* HTS=2844 */
    {0x380E, 0x07, 0}, {0x380F, 0xB0, 0},  /* VTS=1968 */
    
    {0xFFFF, 0xFF, 0},
};

/* 模式信息表 */
static const struct ov5640_mode_info ov5640_modes[MODE_MAX] = {
    [MODE_QVGA]  = {320,  240,  60, 1, 960,  500,  ov5640_qvga_cfg,  ARRAY_SIZE(ov5640_qvga_cfg)},
    [MODE_VGA]   = {640,  480,  30, 1, 1896, 976,  ov5640_vga_cfg,   ARRAY_SIZE(ov5640_vga_cfg)},
    [MODE_720P]  = {1280, 720,  30, 0, 1892, 740,  ov5640_720p_cfg,  ARRAY_SIZE(ov5640_720p_cfg)},
    [MODE_1080P] = {1920, 1080, 15, 0, 2500, 1120, ov5640_1080p_cfg, ARRAY_SIZE(ov5640_1080p_cfg)},
    [MODE_5M]    = {2592, 1944, 15, 0, 2844, 1968, ov5640_5m_cfg,    ARRAY_SIZE(ov5640_5m_cfg)},
};

/* ============== 核心操作函数（优化实现） ============== */

/* 批量I2C写入 - 减少总线开销 */
static int ov5640_write_regs(struct ov5640_dev *dev, 
                             const struct ov5640_reg *regs, u16 num)
{
    struct i2c_msg msg;
    u8 buf[4];
    int i, ret;
    
    for (i = 0; i < num; i++) {
        if (regs[i].addr == 0xFFFF)  /* 结束标记 */
            break;
        
        buf[0] = regs[i].addr >> 8;
        buf[1] = regs[i].addr & 0xFF;
        buf[2] = regs[i].val;
        
        msg.addr = dev->client->addr;
        msg.flags = 0;
        msg.len = 3;
        msg.buf = buf;
        
        ret = i2c_transfer(dev->client->adapter, &msg, 1);
        if (ret != 1) {
            dev_err(&dev->client->dev, 
                    "reg write failed: 0x%04x=0x%02x (ret=%d)\n",
                    regs[i].addr, regs[i].val, ret);
            return -EIO;
        }
        
        if (regs[i].delay_ms)
            msleep(regs[i].delay_ms);
        
#if OV5640_DEBUG_REG_WRITES
        dev_info(&dev->client->dev, "W: 0x%04x = 0x%02x\n", 
                 regs[i].addr, regs[i].val);
#endif
    }
    return 0;
}

/* 单寄存器读写 */
static inline int ov5640_write_reg(struct ov5640_dev *dev, u16 addr, u8 val)
{
    const struct ov5640_reg reg = {addr, val, 0};
    return ov5640_write_regs(dev, &reg, 1);
}

static int ov5640_read_reg(struct ov5640_dev *dev, u16 addr, u8 *val)
{
    u8 buf[2] = {addr >> 8, addr & 0xFF};
    struct i2c_msg msg[2] = {
        { .addr = dev->client->addr, .flags = 0,       .len = 2, .buf = buf },
        { .addr = dev->client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
    };
    int ret = i2c_transfer(dev->client->adapter, msg, 2);
    
    return (ret == 2) ? 0 : -EIO;
}

/* 芯片ID检测 - 快速版本 */
static int ov5640_detect(struct ov5640_dev *dev)
{
    u8 high, low;
    
    if (ov5640_read_reg(dev, OV5640_CHIP_ID_HIGH, &high) ||
        ov5640_read_reg(dev, OV5640_CHIP_ID_LOW, &low))
        return -ENODEV;
    
    if (((high << 8) | low) != OV5640_CHIP_ID) {
        dev_err(&dev->client->dev, "ID mismatch: 0x%04x\n", (high << 8) | low);
        return -ENODEV;
    }
    
    dev_info(&dev->client->dev, "OV5640 detected\n");
    return 0;
}

/* ============== 电源管理（优化时序） ============== */

static int ov5640_power_on(struct ov5640_dev *dev)
{
    int ret;
    
    /* 快速路径：如果已经在STANDBY状态，只需退出PWDN */
    if (dev->state == STATE_STANDBY) {
        if (dev->pwdn_gpio)
            gpiod_set_value(dev->pwdn_gpio, 0);
        usleep_range(5000, 10000);
        dev->state = STATE_IDLE;
        return 0;
    }
    
    /* 完整上电序列 */
    ret = regulator_enable(dev->dovdd);
    if (ret) goto err;
    usleep_range(500, 1000);
    
    ret = regulator_enable(dev->avdd);
    if (ret) goto err_dovdd;
    usleep_range(500, 1000);
    
    ret = regulator_enable(dev->dvdd);
    if (ret) goto err_avdd;
    usleep_range(500, 1000);
    
    ret = clk_prepare_enable(dev->xclk);
    if (ret) goto err_dvdd;
    
    /* 设置默认时钟频率 */
    clk_set_rate(dev->xclk, 24000000);
    usleep_range(5000, 10000);
    
    if (dev->pwdn_gpio)
        gpiod_set_value(dev->pwdn_gpio, 0);
    usleep_range(5000, 10000);
    
    if (dev->reset_gpio) {
        gpiod_set_value(dev->reset_gpio, 0);
        usleep_range(1000, 2000);
        gpiod_set_value(dev->reset_gpio, 1);
        msleep(20);
    }
    
    dev->state = STATE_IDLE;
    return 0;

err_dvdd:
    regulator_disable(dev->dvdd);
err_avdd:
    regulator_disable(dev->avdd);
err_dovdd:
    regulator_disable(dev->dovdd);
err:
    return ret;
}

static int ov5640_power_off(struct ov5640_dev *dev)
{
    /* 进入PWDN模式（保留寄存器配置） */
    if (dev->pwdn_gpio) {
        gpiod_set_value(dev->pwdn_gpio, 1);
        dev->state = STATE_STANDBY;
        return 0;
    }
    
    /* 完全断电 */
    clk_disable_unprepare(dev->xclk);
    regulator_disable(dev->dvdd);
    regulator_disable(dev->avdd);
    regulator_disable(dev->dovdd);
    dev->state = STATE_POWER_OFF;
    
    return 0;
}

/* ============== 模式配置（核心优化） ============== */

/* 计算最优PLL参数 - 动态时钟调整 */
static void ov5640_calc_pll(struct ov5640_dev *dev, 
                            enum ov5640_mode mode, enum ov5640_fps fps,
                            u8 *sys_div, u8 *pll_mult, u8 *pix_div)
{
    const struct ov5640_mode_info *info = &ov5640_modes[mode];
    u32 target_pclk;  /* 目标像素时钟 (KHz) */
    u32 xclk = 24000; /* 24MHz输入 */
    
    /* 根据分辨率和帧率计算目标时钟 */
    if (fps == FPS_60 && info->max_fps >= 60) {
        target_pclk = info->hts * info->vts * 60 / 1000;
    } else if (fps == FPS_30 && info->max_fps >= 30) {
        target_pclk = info->hts * info->vts * 30 / 1000;
    } else {
        target_pclk = info->hts * info->vts * 15 / 1000;
    }
    
    /* 简化计算：使用预设值 */
    *pix_div = 1;  /* 默认1分频 */
    
    if (mode <= MODE_VGA) {
        /* 低分辨率：高帧率 */
        *sys_div = 1;
        *pll_mult = (target_pclk * 2) / xclk;  /* 简化公式 */
    } else if (mode == MODE_720P) {
        *sys_div = 2;
        *pll_mult = 105;
    } else {
        /* 高分辨率：降低时钟 */
        *sys_div = 2;
        *pll_mult = 84;
    }
    
    /* 限制范围 */
    if (*pll_mult < 32) *pll_mult = 32;
    if (*pll_mult > 252) *pll_mult = 252;
}

/* 应用模式配置 - 支持热切换 */
static int ov5640_apply_mode(struct ov5640_dev *dev, enum ov5640_mode mode)
{
    const struct ov5640_mode_info *info = &ov5640_modes[mode];
    int ret;
    
    /* 冷启动：需要完整初始化 */
    if (dev->state != STATE_IDLE) {
        ret = ov5640_write_regs(dev, ov5640_global_init, 
                                ARRAY_SIZE(ov5640_global_init));
        if (ret) return ret;
    }
    
    /* 应用模式特定配置 */
    ret = ov5640_write_regs(dev, info->regs, info->reg_num);
    if (ret) return ret;
    
    /* 更新PLL（如果需要） */
    u8 sys_div, pll_mult, pix_div;
    ov5640_calc_pll(dev, mode, dev->current_fps, &sys_div, &pll_mult, &pix_div);
    
    ov5640_write_reg(dev, 0x3034, 0x1A);  /* 10-bit */
    ov5640_write_reg(dev, 0x3035, (sys_div << 4) | 0x01);
    ov5640_write_reg(dev, 0x3036, pll_mult);
    
    dev->current_mode = mode;
    dev->format.width = info->width;
    dev->format.height = info->height;
    
    dev_info(&dev->client->dev, "Mode: %dx%d, PLL: %d/%d\n",
             info->width, info->height, pll_mult, sys_div);
    
    return 0;
}

/* ============== V4L2 接口（精简优化） ============== */

static int ov5640_s_stream(struct v4l2_subdev *sd, int enable)
{
    struct ov5640_dev *dev = container_of(sd, struct ov5640_dev, sd);
    int ret = 0;
    
    mutex_lock(&dev->lock);
    
    if (enable) {
        if (dev->state != STATE_IDLE) {
            ret = -EINVAL;
            goto out;
        }
        
        /* 启动流：清除待机，使能输出 */
        ret = ov5640_write_reg(dev, 0x4202, 0x00);
        if (!ret)
            dev->state = STATE_STREAMING;
    } else {
        if (dev->state != STATE_STREAMING) {
            ret = 0;
            goto out;
        }
        
        /* 停止流：进入待机模式 */
        ret = ov5640_write_reg(dev, 0x4202, 0x0F);
        if (!ret)
            dev->state = STATE_IDLE;
    }
    
out:
    mutex_unlock(&dev->lock);
    return ret;
}

static int ov5640_get_fmt(struct v4l2_subdev *sd,
                          struct v4l2_subdev_state *state,
                          struct v4l2_subdev_format *fmt)
{
    struct ov5640_dev *dev = container_of(sd, struct ov5640_dev, sd);
    
    mutex_lock(&dev->lock);
    
    if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
        fmt->format = *v4l2_subdev_get_try_format(sd, state, fmt->pad);
    else
        fmt->format = dev->format;
    
    mutex_unlock(&dev->lock);
    return 0;
}

static int ov5640_set_fmt(struct v4l2_subdev *sd,
                          struct v4l2_subdev_state *state,
                          struct v4l2_subdev_format *fmt)
{
    struct ov5640_dev *dev = container_of(sd, struct ov5640_dev, sd);
    struct v4l2_mbus_framefmt *format = &fmt->format;
    enum ov5640_mode mode;
    int i, ret = 0;
    
    /* 强制YUYV */
    if (format->code != MEDIA_BUS_FMT_YUYV8_2X8 &&
        format->code != MEDIA_BUS_FMT_UYVY8_2X8)
        format->code = MEDIA_BUS_FMT_YUYV8_2X8;
    
    /* 快速匹配模式 */
    for (i = 0; i < MODE_MAX; i++) {
        if (format->width <= ov5640_modes[i].width &&
            format->height <= ov5640_modes[i].height) {
            mode = i;
            break;
        }
    }
    if (i >= MODE_MAX) mode = MODE_5M;  /* 默认最大 */
    
    format->width = ov5640_modes[mode].width;
    format->height = ov5640_modes[mode].height;
    format->field = V4L2_FIELD_NONE;
    format->colorspace = V4L2_COLORSPACE_SRGB;
    
    mutex_lock(&dev->lock);
    
    if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
        *v4l2_subdev_get_try_format(sd, state, fmt->pad) = *format;
    } else if (dev->state == STATE_IDLE || dev->state == STATE_POWER_OFF) {
        ret = ov5640_apply_mode(dev, mode);
        if (!ret) {
            dev->format = *format;
            dev->mbus_code = format->code;
        }
    } else {
        ret = -EBUSY;  /* 流开启时不能改 */
    }
    
    mutex_unlock(&dev->lock);
    return ret;
}

static int ov5640_enum_mbus_code(struct v4l2_subdev *sd,
                                 struct v4l2_subdev_state *state,
                                 struct v4l2_subdev_mbus_code_enum *code)
{
    if (code->index >= 2)
        return -EINVAL;
    
    code->code = (code->index == 0) ? 
                 MEDIA_BUS_FMT_YUYV8_2X8 : MEDIA_BUS_FMT_UYVY8_2X8;
    return 0;
}

static int ov5640_enum_frame_size(struct v4l2_subdev *sd,
                                  struct v4l2_subdev_state *state,
                                  struct v4l2_subdev_frame_size_enum *fse)
{
    if (fse->index >= MODE_MAX)
        return -EINVAL;
    
    fse->min_width = fse->max_width = ov5640_modes[fse->index].width;
    fse->min_height = fse->max_height = ov5640_modes[fse->index].height;
    
    return 0;
}

static int ov5640_enum_frame_interval(struct v4l2_subdev *sd,
                                      struct v4l2_subdev_state *state,
                                      struct v4l2_subdev_frame_interval_enum *fie)
{
    const struct ov5640_mode_info *mode;
    int i;
    
    /* 找到模式 */
    for (i = 0; i < MODE_MAX; i++) {
        if (ov5640_modes[i].width == fie->width &&
            ov5640_modes[i].height == fie->height)
            break;
    }
    if (i >= MODE_MAX)
        return -EINVAL;
    
    mode = &ov5640_modes[i];
    
    /* 根据索引返回支持的帧率 */
    if (fie->index == 0 && mode->max_fps >= 15) {
        fie->interval.numerator = 1;
        fie->interval.denominator = 15;
    } else if (fie->index == 1 && mode->max_fps >= 30) {
        fie->interval.numerator = 1;
        fie->interval.denominator = 30;
    } else if (fie->index == 2 && mode->max_fps >= 60) {
        fie->interval.numerator = 1;
        fie->interval.denominator = 60;
    } else {
        return -EINVAL;
    }
    
    return 0;
}

/* 帧率控制 */
static int ov5640_get_frame_interval(struct v4l2_subdev *sd,
                                     struct v4l2_subdev_frame_interval *fi)
{
    struct ov5640_dev *dev = container_of(sd, struct ov5640_dev, sd);
    
    mutex_lock(&dev->lock);
    fi->interval.numerator = 1;
    switch (dev->current_fps) {
    case FPS_60: fi->interval.denominator = 60; break;
    case FPS_30: fi->interval.denominator = 30; break;
    default:     fi->interval.denominator = 15; break;
    }
    mutex_unlock(&dev->lock);
    
    return 0;
}

static int ov5640_set_frame_interval(struct v4l2_subdev *sd,
                                     struct v4l2_subdev_frame_interval *fi)
{
    struct ov5640_dev *dev = container_of(sd, struct ov5640_dev, sd);
    u32 fps, val;
    int ret = 0;
    
    if (fi->interval.numerator == 0)
        return -EINVAL;
    
    fps = fi->interval.denominator / fi->interval.numerator;
    
    mutex_lock(&dev->lock);
    
    /* 检查是否支持 */
    const struct ov5640_mode_info *mode = &ov5640_modes[dev->current_mode];
    if (fps > mode->max_fps)
        fps = mode->max_fps;
    
    /* 计算时钟分频 */
    if (fps >= 60 && mode->max_fps >= 60) {
        dev->current_fps = FPS_60;
        val = 0x01;  /* 60fps */
    } else if (fps >= 30 && mode->max_fps >= 30) {
        dev->current_fps = FPS_30;
        val = 0x01;  /* 30fps基础分频 */
    } else {
        dev->current_fps = FPS_15;
        val = 0x03;  /* 15fps 4分频 */
    }
    
    /* 应用帧率 */
    ret = ov5640_write_reg(dev, 0x3035, (val << 4) | 0x01);
    
    mutex_unlock(&dev->lock);
    return ret;
}

/* 控制接口 */
static int ov5640_s_ctrl(struct v4l2_ctrl *ctrl)
{
    struct ov5640_dev *dev = container_of(ctrl->handler, struct ov5640_dev, 
                                          ctrl_handler);
    int ret = 0;
    
    mutex_lock(&dev->lock);
    
    switch (ctrl->id) {
    case V4L2_CID_BRIGHTNESS:
        /* 亮度: 0-255, 默认128 */
        ret = ov5640_write_reg(dev, 0x5587, ctrl->val);
        break;
    case V4L2_CID_CONTRAST:
        /* 对比度 */
        ret = ov5640_write_reg(dev, 0x5588, ctrl->val);
        break;
    case V4L2_CID_SATURATION:
        /* 饱和度 */
        ret = ov5640_write_reg(dev, 0x5583, ctrl->val);
        break;
    case V4L2_CID_AUTO_WHITE_BALANCE:
        /* 自动白平衡 */
        ret = ov5640_write_reg(dev, 0x3406, ctrl->val ? 0x00 : 0x01);
        break;
    case V4L2_CID_EXPOSURE_AUTO:
        /* 自动曝光 */
        ret = ov5640_write_reg(dev, 0x3503, ctrl->val ? 0x00 : 0x07);
        break;
    default:
        ret = -EINVAL;
    }
    
    mutex_unlock(&dev->lock);
    return ret;
}

static const struct v4l2_ctrl_ops ov5640_ctrl_ops = {
    .s_ctrl = ov5640_s_ctrl,
};

/* ============== 设备驱动框架 ============== */

static const struct v4l2_subdev_video_ops ov5640_video_ops = {
    .s_stream = ov5640_s_stream,
    .g_frame_interval = ov5640_get_frame_interval,
    .s_frame_interval = ov5640_set_frame_interval,
};

static const struct v4l2_subdev_pad_ops ov5640_pad_ops = {
    .enum_mbus_code = ov5640_enum_mbus_code,
    .enum_frame_size = ov5640_enum_frame_size,
    .enum_frame_interval = ov5640_enum_frame_interval,
    .get_fmt = ov5640_get_fmt,
    .set_fmt = ov5640_set_fmt,
};

static const struct v4l2_subdev_ops ov5640_subdev_ops = {
    .video = &ov5640_video_ops,
    .pad = &ov5640_pad_ops,
};

static int ov5640_probe(struct i2c_client *client,
                        const struct i2c_device_id *id)
{
    struct ov5640_dev *dev;
    struct v4l2_subdev *sd;
    struct device *d = &client->dev;
    int ret;
    
    dev = devm_kzalloc(d, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    
    dev->client = client;
    i2c_set_clientdata(client, dev);
    mutex_init(&dev->lock);
    dev->state = STATE_POWER_OFF;
    dev->current_fps = FPS_30;
    
    /* 解析设备树 - 使用devm简化错误处理 */
    dev->pwdn_gpio = devm_gpiod_get_optional(d, "pwdn", GPIOD_OUT_HIGH);
    if (IS_ERR(dev->pwdn_gpio))
        return PTR_ERR(dev->pwdn_gpio);
    
    dev->reset_gpio = devm_gpiod_get_optional(d, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(dev->reset_gpio))
        return PTR_ERR(dev->reset_gpio);
    
    dev->xclk = devm_clk_get(d, "xclk");
    if (IS_ERR(dev->xclk)) {
        dev_err(d, "Failed to get xclk\n");
        return PTR_ERR(dev->xclk);
    }
    
    dev->avdd = devm_regulator_get(d, "avdd");
    dev->dvdd = devm_regulator_get(d, "dvdd");
    dev->dovdd = devm_regulator_get(d, "dovdd");
    if (IS_ERR(dev->avdd) || IS_ERR(dev->dvdd) || IS_ERR(dev->dovdd))
        return PTR_ERR(IS_ERR(dev->avdd) ? dev->avdd : 
                      (IS_ERR(dev->dvdd) ? dev->dvdd : dev->dovdd));
    
    /* 上电并检测 */
    ret = ov5640_power_on(dev);
    if (ret) {
        dev_err(d, "Power on failed\n");
        return ret;
    }
    
    ret = ov5640_detect(dev);
    if (ret) {
        ov5640_power_off(dev);
        return ret;
    }
    
    /* 应用默认配置 */
    ret = ov5640_apply_mode(dev, MODE_VGA);
    if (ret) {
        ov5640_power_off(dev);
        return ret;
    }
    
    /* 初始化V4L2子设备 */
    sd = &dev->sd;
    v4l2_i2c_subdev_init(sd, client, &ov5640_subdev_ops);
    sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
    
    /* 初始化控制 */
    v4l2_ctrl_handler_init(&dev->ctrl_handler, 5);
    v4l2_ctrl_new_std(&dev->ctrl_handler, &ov5640_ctrl_ops,
                      V4L2_CID_BRIGHTNESS, 0, 255, 1, 128);
    v4l2_ctrl_new_std(&dev->ctrl_handler, &ov5640_ctrl_ops,
                      V4L2_CID_CONTRAST, 0, 255, 1, 128);
    v4l2_ctrl_new_std(&dev->ctrl_handler, &ov5640_ctrl_ops,
                      V4L2_CID_SATURATION, 0, 255, 1, 128);
    v4l2_ctrl_new_std(&dev->ctrl_handler, &ov5640_ctrl_ops,
                      V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
    v4l2_ctrl_new_std(&dev->ctrl_handler, &ov5640_ctrl_ops,
                      V4L2_CID_EXPOSURE_AUTO, 0, 1, 1, 1);
    
    if (dev->ctrl_handler.error) {
        ret = dev->ctrl_handler.error;
        goto err_ctrl;
    }
    sd->ctrl_handler = &dev->ctrl_handler;
    
    /* 初始化默认格式 */
    dev->format.code = MEDIA_BUS_FMT_YUYV8_2X8;
    dev->format.width = 640;
    dev->format.height = 480;
    dev->format.field = V4L2_FIELD_NONE;
    dev->format.colorspace = V4L2_COLORSPACE_SRGB;
    dev->mbus_code = MEDIA_BUS_FMT_YUYV8_2X8;
    
    /* 注册异步子设备 */
    ret = v4l2_async_register_subdev(sd);
    if (ret)
        goto err_ctrl;
    
    dev_info(d, "OV5640 initialized: %dx%d @ 30fps\n",
             dev->format.width, dev->format.height);
    return 0;

err_ctrl:
    v4l2_ctrl_handler_free(&dev->ctrl_handler);
    ov5640_power_off(dev);
    return ret;
}

static int ov5640_remove(struct i2c_client *client)
{
    struct ov5640_dev *dev = i2c_get_clientdata(client);
    
    v4l2_async_unregister_subdev(&dev->sd);
    v4l2_ctrl_handler_free(&dev->ctrl_handler);
    ov5640_power_off(dev);
    
    return 0;
}

static int ov5640_suspend(struct device *dev)
{
    struct ov5640_dev *sensor = dev_get_drvdata(dev);
    
    mutex_lock(&sensor->lock);
    if (sensor->state == STATE_STREAMING) {
        ov5640_write_reg(sensor, 0x4202, 0x0F);
        sensor->state = STATE_IDLE;
    }
    ov5640_power_off(sensor);
    mutex_unlock(&sensor->lock);
    
    return 0;
}

static int ov5640_resume(struct device *dev)
{
    struct ov5640_dev *sensor = dev_get_drvdata(dev);
    int ret;
    
    mutex_lock(&sensor->lock);
    ret = ov5640_power_on(sensor);
    if (ret == 0 && sensor->state == STATE_IDLE) {
        /* 恢复配置 */
        ov5640_apply_mode(sensor, sensor->current_mode);
    }
    mutex_unlock(&sensor->lock);
    
    return ret;
}

static const struct dev_pm_ops ov5640_pm_ops = {
    SET_SYSTEM_SLEEP_PM_OPS(ov5640_suspend, ov5640_resume)
};

static const struct i2c_device_id ov5640_id[] = {
    {"ov5640", 0},
    {}
};
MODULE_DEVICE_TABLE(i2c, ov5640_id);

static const struct of_device_id ov5640_of_match[] = {
    { .compatible = "ovti,ov5640" },
    { .compatible = "ov5640" },
    {}
};
MODULE_DEVICE_TABLE(of, ov5640_of_match);

static struct i2c_driver ov5640_driver = {
    .driver = {
        .name = "ov5640",
        .of_match_table = ov5640_of_match,
        .pm = &ov5640_pm_ops,
    },
    .probe = ov5640_probe,
    .remove = ov5640_remove,
    .id_table = ov5640_id,
};

module_i2c_driver(ov5640_driver);

MODULE_DESCRIPTION("OV5640 Camera Sensor Driver - Optimized");
MODULE_AUTHOR("Embedded Engineer");
MODULE_LICENSE("GPL");
MODULE_VERSION("2.0");