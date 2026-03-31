/*
 * ============================================================================
 * 文件：imx6ull_csi.c
 * 描述：i.MX6ULL CSI 主机驱动 (优化版)
 * 
 * 优化点：
 * 1. 简化buffer管理，使用双缓冲+备用队列模式
 * 2. 优化中断处理，减少锁竞争
 * 3. 添加VB2流式API完整支持
 * 4. 优化启动流程，减少首帧延迟
 * ============================================================================
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/of_device.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>

#include <media/v4l2-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#define DRIVER_NAME "imx6ull-csi"
#define DRIVER_VERSION "1.0.0"

/* CSI寄存器定义 */
#define CSI_CSICR1              0x00
#define CSI_CSICR2              0x04
#define CSI_CSICR3              0x08
#define CSI_CSISR               0x18
#define CSI_CSIDMASA_FB1        0x28
#define CSI_CSIDMASA_FB2        0x2C
#define CSI_CSIFBUF_PARA        0x30
#define CSI_CSIIMAG_PARA        0x34
#define CSI_CSICR18             0x48

/* CR1位定义 */
#define BIT_CSI_ENABLE          (1 << 31)   /* CR18中的使能位 */
#define BIT_SWAP16_EN           (1 << 31)
#define BIT_EOF_INT_EN          (1 << 29)
#define BIT_PRP_IF_EN           (1 << 28)
#define BIT_SOF_POL             (1 << 17)
#define BIT_SOF_INTEN           (1 << 16)
#define BIT_HSYNC_POL           (1 << 11)
#define BIT_CCIR_EN             (1 << 10)
#define BIT_FCC                 (1 << 8)
#define BIT_PACK_DIR            (1 << 7)
#define BIT_GCLK_MODE           (1 << 4)
#define BIT_REDGE               (1 << 1)
#define BIT_PIXEL_BIT           (1 << 0)

/* CR3位定义 */
#define BIT_DMA_REFLASH_RFF     (1 << 14)
#define BIT_DMA_REQ_EN_RFF      (1 << 12)
#define BIT_RXFF_LEVEL          (0x7 << 4)
#define BIT_TWO_8BIT_SENSOR     (1 << 3)

/* ISR位定义 */
#define BIT_DMA_TSF_DONE_FB1    (1 << 19)
#define BIT_DMA_TSF_DONE_FB2    (1 << 20)
#define BIT_SOF_INT             (1 << 16)
#define BIT_RFF_OR_INT          (1 << 25)
#define BIT_HRESP_ERR_INT       (1 << 7)

#define SHIFT_RXFIFO_LEVEL      4

/* 支持的格式 */
struct csi_format {
    char name[32];
    u32 fourcc;
    u32 mbus_code;
    u8 bpp;
};

static const struct csi_format csi_formats[] = {
    {
        .name = "YUYV 4:2:2",
        .fourcc = V4L2_PIX_FMT_YUYV,
        .mbus_code = MEDIA_BUS_FMT_YUYV8_2X8,
        .bpp = 2,
    },
    {
        .name = "UYVY 4:2:2",
        .fourcc = V4L2_PIX_FMT_UYVY,
        .mbus_code = MEDIA_BUS_FMT_UYVY8_2X8,
        .bpp = 2,
    },
};

/* Buffer管理结构 - 优化版：使用双缓冲+环形队列 */
struct csi_buffer {
    struct vb2_v4l2_buffer vb;
    struct list_head queue;
    int bufnum;         /* 0=FB1, 1=FB2 */
};

struct csi_dev {
    struct v4l2_device v4l2_dev;
    struct video_device vdev;
    struct vb2_queue vb2_q;
    
    /* 硬件资源 */
    void __iomem *reg_base;
    int irq;
    struct clk *clk_disp_axi;
    struct clk *clk_disp_dcic;
    struct clk *clk_csi_mclk;
    struct device *dev;
    
    /* 锁 */
    struct mutex lock;          /* V4L2操作锁 */
    spinlock_t slock;           /* 中断和buffer队列锁 */
    
    /* 格式状态 */
    const struct csi_format *fmt;
    u32 width, height;
    u32 bytesperline;
    u32 sizeimage;
    u32 field;
    
    /* Buffer管理 - 核心优化 */
    struct list_head buf_queue;     /* 等待入队的VB2 buffer */
    struct csi_buffer *active_buf[2]; /* 当前在硬件使用的两个buffer */
    struct csi_buffer *discard_buf;   /* 丢弃用buffer（无可用buffer时使用） */
    dma_addr_t discard_dma;
    
    bool streaming;
    u32 frame_count;
    u32 buf_toggle;     /* 0或1，指示下一个应使用的buffer槽位 */
    
    /* 子设备 */
    struct v4l2_subdev *sd;
    struct v4l2_async_notifier notifier;
};

#define CSI_REG_READ(csi, reg) readl((csi)->reg_base + (reg))
#define CSI_REG_WRITE(csi, val, reg) writel((val), (csi)->reg_base + (reg))

/* ============== 硬件操作函数 ============== */

static inline void csi_enable(struct csi_dev *csi)
{
    u32 cr18 = CSI_REG_READ(csi, CSI_CSICR18);
    cr18 |= BIT_CSI_ENABLE;
    CSI_REG_WRITE(csi, cr18, CSI_CSICR18);
}

static inline void csi_disable(struct csi_dev *csi)
{
    u32 cr18 = CSI_REG_READ(csi, CSI_CSICR18);
    cr18 &= ~BIT_CSI_ENABLE;
    CSI_REG_WRITE(csi, cr18, CSI_CSICR18);
}

static void csi_reset(struct csi_dev *csi)
{
    /* 禁用CSI */
    csi_disable(csi);
    
    /* 清除中断状态 */
    CSI_REG_WRITE(csi, 0xFFFFFFFF, CSI_CSISR);
    
    /* 复位FIFO */
    u32 cr1 = CSI_REG_READ(csi, CSI_CSICR1);
    CSI_REG_WRITE(csi, cr1 & ~BIT_FCC, CSI_CSICR1);
    CSI_REG_WRITE(csi, cr1 | BIT_FCC, CSI_CSICR1);
    
    /* DMA reflash */
    u32 cr3 = CSI_REG_READ(csi, CSI_CSICR3);
    CSI_REG_WRITE(csi, cr3 | BIT_DMA_REFLASH_RFF, CSI_CSICR3);
    udelay(10);
}

static void csi_init_interface(struct csi_dev *csi)
{
    u32 cr1, cr2, cr3;
    
    /* 基础配置：GCLK模式，上升沿采样，SOF低电平有效 */
    cr1 = BIT_GCLK_MODE | BIT_REDGE | BIT_HSYNC_POL | BIT_FCC | BIT_PACK_DIR;
    cr1 |= (1 << 12);  /* MCLKDIV = 1 */
    cr1 |= BIT_MCLK_EN;  /* 使能MCLK */
    
    if (csi->fmt->fourcc == V4L2_PIX_FMT_YUYV)
        cr1 |= BIT_SWAP16_EN;  /* YUYV需要交换 */
    
    CSI_REG_WRITE(csi, cr1, CSI_CSICR1);
    
    /* CR2: 设置突发传输模式 */
    cr2 = 0xC0000000;  /* INCR16 burst */
    CSI_REG_WRITE(csi, cr2, CSI_CSICR2);
    
    /* CR3: 使能DMA请求，设置FIFO阈值 */
    cr3 = BIT_DMA_REQ_EN_RFF | BIT_HRESP_ERR_INT;
    cr3 |= (0x2 << SHIFT_RXFIFO_LEVEL);  /* FIFO阈值 */
    CSI_REG_WRITE(csi, cr3, CSI_CSICR3);
    
    /* 设置图像参数 */
    u32 imag_para;
    if (csi->fmt->fourcc == V4L2_PIX_FMT_YUV32 ||
        csi->fmt->fourcc == V4L2_PIX_FMT_SBGGR8)
        imag_para = (csi->width << 16) | csi->height;
    else
        imag_para = (csi->width * 2 << 16) | csi->height;  /* YUYV: 每行2*width字节 */
    
    CSI_REG_WRITE(csi, imag_para, CSI_CSIIMAG_PARA);
    
    /* 设置行stride */
    CSI_REG_WRITE(csi, csi->bytesperline, CSI_CSIFBUF_PARA);
}

static void csi_update_buf_addr(struct csi_dev *csi, dma_addr_t addr, int bufnum)
{
    if (bufnum == 0)
        CSI_REG_WRITE(csi, addr, CSI_CSIDMASA_FB1);
    else
        CSI_REG_WRITE(csi, addr, CSI_CSIDMASA_FB2);
}

/* ============== 时钟管理 ============== */

static void csi_clk_enable(struct csi_dev *csi)
{
    clk_prepare_enable(csi->clk_disp_axi);
    clk_prepare_enable(csi->clk_disp_dcic);
    clk_prepare_enable(csi->clk_csi_mclk);
}

static void csi_clk_disable(struct csi_dev *csi)
{
    clk_disable_unprepare(csi->clk_csi_mclk);
    clk_disable_unprepare(csi->clk_disp_dcic);
    clk_disable_unprepare(csi->clk_disp_axi);
}

/* ============== 中断处理 - 优化版 ============== */

static irqreturn_t csi_irq_handler(int irq, void *data)
{
    struct csi_dev *csi = data;
    u32 status, cr3;
    struct csi_buffer *buf;
    unsigned long flags;
    int completed_buf;
    
    status = CSI_REG_READ(csi, CSI_CSISR);
    
    /* 清除中断 */
    CSI_REG_WRITE(csi, status, CSI_CSISR);
    
    /* 快速路径：检查是否是我们关心的中断 */
    if (!(status & (BIT_DMA_TSF_DONE_FB1 | BIT_DMA_TSF_DONE_FB2 | 
                    BIT_RFF_OR_INT | BIT_HRESP_ERR_INT)))
        return IRQ_NONE;
    
    spin_lock_irqsave(&csi->slock, flags);
    
    /* 错误处理 */
    if (status & (BIT_RFF_OR_INT | BIT_HRESP_ERR_INT)) {
        dev_warn_ratelimited(csi->dev, "CSI error: status=0x%x\n", status);
        /* 错误恢复：reflash DMA */
        cr3 = CSI_REG_READ(csi, CSI_CSICR3);
        CSI_REG_WRITE(csi, cr3 | BIT_DMA_REFLASH_RFF, CSI_CSICR3);
    }
    
    /* 处理完成的buffer */
    completed_buf = -1;
    if (status & BIT_DMA_TSF_DONE_FB1)
        completed_buf = 0;
    else if (status & BIT_DMA_TSF_DONE_FB2)
        completed_buf = 1;
    
    if (completed_buf >= 0 && csi->active_buf[completed_buf]) {
        buf = csi->active_buf[completed_buf];
        csi->active_buf[completed_buf] = NULL;
        
        /* 标记完成 */
        buf->vb.vb2_buf.timestamp = ktime_get_ns();
        buf->vb.sequence = csi->frame_count++;
        vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
    }
    
    /* 填充下一个buffer */
    int next_buf = completed_buf >= 0 ? completed_buf : csi->buf_toggle;
    
    if (!csi->active_buf[next_buf]) {
        if (!list_empty(&csi->buf_queue)) {
            /* 使用用户buffer */
            buf = list_first_entry(&csi->buf_queue, struct csi_buffer, queue);
            list_del(&buf->queue);
            buf->bufnum = next_buf;
            csi->active_buf[next_buf] = buf;
            csi_update_buf_addr(csi, 
                vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0), next_buf);
        } else if (csi->discard_buf) {
            /* 使用丢弃buffer */
            csi_update_buf_addr(csi, csi->discard_dma, next_buf);
        }
    }
    
    /* 切换toggle */
    if (completed_buf >= 0)
        csi->buf_toggle = completed_buf;
    
    spin_unlock_irqrestore(&csi->slock, flags);
    
    return IRQ_HANDLED;
}

/* ============== VB2 操作 - 优化版 ============== */

static int csi_queue_setup(struct vb2_queue *vq,
                           unsigned int *nbuffers, unsigned int *nplanes,
                           unsigned int sizes[], struct device *alloc_devs[])
{
    struct csi_dev *csi = vb2_get_drv_priv(vq);
    
    if (*nplanes)
        return sizes[0] < csi->sizeimage ? -EINVAL : 0;
    
    *nplanes = 1;
    sizes[0] = csi->sizeimage;
    
    /* 最小buffer数：双缓冲+2个备用 */
    if (*nbuffers < 4)
        *nbuffers = 4;
    
    dev_dbg(csi->dev, "queue setup: %d buffers, size=%d\n", *nbuffers, sizes[0]);
    
    return 0;
}

static int csi_buf_init(struct vb2_buffer *vb)
{
    struct csi_dev *csi = vb2_get_drv_priv(vb->vb2_queue);
    struct csi_buffer *buf = container_of(vb, struct csi_buffer, vb.vb2_buf);
    
    buf->bufnum = -1;  /* 未分配 */
    INIT_LIST_HEAD(&buf->queue);
    
    return 0;
}

static int csi_buf_prepare(struct vb2_buffer *vb)
{
    struct csi_dev *csi = vb2_get_drv_priv(vb->vb2_queue);
    
    if (vb2_plane_size(vb, 0) < csi->sizeimage) {
        dev_err(csi->dev, "buffer too small (%lu < %u)\n",
                vb2_plane_size(vb, 0), csi->sizeimage);
        return -EINVAL;
    }
    
    vb2_set_plane_payload(vb, 0, csi->sizeimage);
    return 0;
}

static void csi_buf_queue(struct vb2_buffer *vb)
{
    struct csi_dev *csi = vb2_get_drv_priv(vb->vb2_queue);
    struct csi_buffer *buf = container_of(vb, struct csi_buffer, vb.vb2_buf);
    unsigned long flags;
    dma_addr_t addr;
    int empty_slot = -1;
    
    spin_lock_irqsave(&csi->slock, flags);
    
    if (!csi->streaming) {
        /* 未开始流，加入等待队列 */
        list_add_tail(&buf->queue, &csi->buf_queue);
        spin_unlock_irqrestore(&csi->slock, flags);
        return;
    }
    
    /* 寻找空的硬件槽位 */
    if (!csi->active_buf[0]) empty_slot = 0;
    else if (!csi->active_buf[1]) empty_slot = 1;
    
    if (empty_slot >= 0) {
        /* 直接送入硬件 */
        buf->bufnum = empty_slot;
        csi->active_buf[empty_slot] = buf;
        addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
        csi_update_buf_addr(csi, addr, empty_slot);
    } else {
        /* 硬件忙，加入队列等待 */
        list_add_tail(&buf->queue, &csi->buf_queue);
    }
    
    spin_unlock_irqrestore(&csi->slock, flags);
}

static int csi_start_streaming(struct vb2_queue *vq, unsigned int count)
{
    struct csi_dev *csi = vb2_get_drv_priv(vq);
    struct csi_buffer *buf;
    unsigned long flags;
    dma_addr_t addr;
    int ret, i;
    
    if (count < 2) {
        ret = -ENOBUFS;
        goto err_return_bufs;
    }
    
    mutex_lock(&csi->lock);
    
    /* 分配丢弃buffer */
    csi->discard_buf = kzalloc(sizeof(*csi->discard_buf), GFP_KERNEL);
    if (!csi->discard_buf) {
        ret = -ENOMEM;
        goto err_unlock;
    }
    
    csi->discard_dma = dma_alloc_coherent(csi->dev, PAGE_ALIGN(csi->sizeimage),
                                          &csi->discard_dma, GFP_DMA | GFP_KERNEL);
    if (!csi->discard_dma) {
        ret = -ENOMEM;
        goto err_free_discard_buf;
    }
    
    /* 初始化硬件 */
    csi_clk_enable(csi);
    csi_reset(csi);
    csi_init_interface(csi);
    
    /* 启动sensor */
    ret = v4l2_subdev_call(csi->sd, video, s_stream, 1);
    if (ret < 0) {
        dev_err(csi->dev, "sensor start failed: %d\n", ret);
        goto err_free_dma;
    }
    
    spin_lock_irqsave(&csi->slock, flags);
    
    csi->streaming = true;
    csi->frame_count = 0;
    csi->buf_toggle = 0;
    csi->active_buf[0] = NULL;
    csi->active_buf[1] = NULL;
    
    /* 预填充两个硬件buffer */
    for (i = 0; i < 2; i++) {
        if (list_empty(&csi->buf_queue))
            break;
        
        buf = list_first_entry(&csi->buf_queue, struct csi_buffer, queue);
        list_del(&buf->queue);
        
        buf->bufnum = i;
        csi->active_buf[i] = buf;
        addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
        csi_update_buf_addr(csi, addr, i);
    }
    
    spin_unlock_irqrestore(&csi->slock, flags);
    
    /* 使能CSI - 等待SOF后启动（避免首帧撕裂） */
    u32 cr1 = CSI_REG_READ(csi, CSI_CSICR1);
    CSI_REG_WRITE(csi, cr1 | BIT_SOF_INTEN, CSI_CSICR1);
    
    /* 使能CSI */
    csi_enable(csi);
    
    /* 轮询等待SOF（最多100ms） */
    int timeout;
    for (timeout = 100000; timeout > 0; timeout--) {
        if (CSI_REG_READ(csi, CSI_CSISR) & BIT_SOF_INT)
            break;
        udelay(1);
    }
    
    if (timeout <= 0)
        dev_warn(csi->dev, "SOF timeout, image may be corrupted\n");
    
    /* 清除SOF中断，使能DMA中断 */
    CSI_REG_WRITE(csi, BIT_SOF_INT, CSI_CSISR);
    cr1 = CSI_REG_READ(csi, CSI_CSICR1);
    cr1 &= ~BIT_SOF_INTEN;
    cr1 |= BIT_FB1_DMA_DONE_INTEN | BIT_FB2_DMA_DONE_INTEN;
    CSI_REG_WRITE(csi, cr1, CSI_CSICR1);
    
    mutex_unlock(&csi->lock);
    
    dev_dbg(csi->dev, "streaming started\n");
    return 0;

err_free_dma:
    dma_free_coherent(csi->dev, PAGE_ALIGN(csi->sizeimage), 
                      csi->discard_buf, csi->discard_dma);
err_free_discard_buf:
    kfree(csi->discard_buf);
    csi->discard_buf = NULL;
err_unlock:
    mutex_unlock(&csi->lock);
err_return_bufs:
    vb2_return_all_buffers(vq, VB2_BUF_STATE_QUEUED);
    return ret;
}

static void csi_stop_streaming(struct vb2_queue *vq)
{
    struct csi_dev *csi = vb2_get_drv_priv(vq);
    struct csi_buffer *buf;
    unsigned long flags;
    int i;
    
    mutex_lock(&csi->lock);
    
    csi->streaming = false;
    
    /* 停止sensor */
    v4l2_subdev_call(csi->sd, video, s_stream, 0);
    
    /* 禁用CSI */
    csi_disable(csi);
    
    spin_lock_irqsave(&csi->slock, flags);
    
    /* 归还所有active buffer */
    for (i = 0; i < 2; i++) {
        if (csi->active_buf[i]) {
            vb2_buffer_done(&csi->active_buf[i]->vb.vb2_buf, VB2_BUF_STATE_ERROR);
            csi->active_buf[i] = NULL;
        }
    }
    
    /* 归还队列中的buffer */
    while (!list_empty(&csi->buf_queue)) {
        buf = list_first_entry(&csi->buf_queue, struct csi_buffer, queue);
        list_del(&buf->queue);
        vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
    }
    
    spin_unlock_irqrestore(&csi->slock, flags);
    
    /* 释放丢弃buffer */
    if (csi->discard_buf) {
        dma_free_coherent(csi->dev, PAGE_ALIGN(csi->sizeimage),
                          csi->discard_buf, csi->discard_dma);
        kfree(csi->discard_buf);
        csi->discard_buf = NULL;
    }
    
    csi_clk_disable(csi);
    
    mutex_unlock(&csi->lock);
    
    dev_dbg(csi->dev, "streaming stopped\n");
}

static const struct vb2_ops csi_vb2_ops = {
    .queue_setup = csi_queue_setup,
    .buf_init = csi_buf_init,
    .buf_prepare = csi_buf_prepare,
    .buf_queue = csi_buf_queue,
    .start_streaming = csi_start_streaming,
    .stop_streaming = csi_stop_streaming,
    .wait_prepare = vb2_ops_wait_prepare,
    .wait_finish = vb2_ops_wait_finish,
};

/* ============== V4L2 接口 ============== */

static int csi_querycap(struct file *file, void *priv,
                        struct v4l2_capability *cap)
{
    struct csi_dev *csi = video_drvdata(file);
    
    strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver));
    strscpy(cap->card, "i.MX6ULL CSI Camera", sizeof(cap->card));
    snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
             dev_name(csi->dev));
    
    cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
    
    return 0;
}

static int csi_enum_fmt(struct file *file, void *priv,
                        struct v4l2_fmtdesc *f)
{
    if (f->index >= ARRAY_SIZE(csi_formats))
        return -EINVAL;
    
    strscpy(f->description, csi_formats[f->index].name, sizeof(f->description));
    f->pixelformat = csi_formats[f->index].fourcc;
    
    return 0;
}

static int csi_try_fmt(struct file *file, void *priv,
                       struct v4l2_format *f)
{
    struct csi_dev *csi = video_drvdata(file);
    struct v4l2_subdev_format sd_fmt;
    const struct csi_format *fmt = NULL;
    int i, ret;
    
    /* 查找格式 */
    for (i = 0; i < ARRAY_SIZE(csi_formats); i++) {
        if (csi_formats[i].fourcc == f->fmt.pix.pixelformat) {
            fmt = &csi_formats[i];
            break;
        }
    }
    if (!fmt) {
        fmt = &csi_formats[0];  /* 默认YUYV */
        f->fmt.pix.pixelformat = fmt->fourcc;
    }
    
    /* 与sensor协商 */
    sd_fmt.which = V4L2_SUBDEV_FORMAT_TRY;
    sd_fmt.pad = 0;
    v4l2_fill_mbus_format(&sd_fmt.format, &f->fmt.pix, fmt->mbus_code);
    
    ret = v4l2_subdev_call(csi->sd, pad, set_fmt, NULL, &sd_fmt);
    if (ret < 0)
        return ret;
    
    /* 使用sensor返回的实际参数 */
    v4l2_fill_pix_format(&f->fmt.pix, &sd_fmt.format);
    f->fmt.pix.pixelformat = fmt->fourcc;
    f->fmt.pix.bytesperline = f->fmt.pix.width * fmt->bpp;
    f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * f->fmt.pix.height;
    f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    
    if (f->fmt.pix.field != V4L2_FIELD_INTERLACED)
        f->fmt.pix.field = V4L2_FIELD_NONE;
    
    return 0;
}

static int csi_g_fmt(struct file *file, void *priv,
                     struct v4l2_format *f)
{
    struct csi_dev *csi = video_drvdata(file);
    
    f->fmt.pix.width = csi->width;
    f->fmt.pix.height = csi->height;
    f->fmt.pix.pixelformat = csi->fmt->fourcc;
    f->fmt.pix.field = csi->field;
    f->fmt.pix.bytesperline = csi->bytesperline;
    f->fmt.pix.sizeimage = csi->sizeimage;
    f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    
    return 0;
}

static int csi_s_fmt(struct file *file, void *priv,
                     struct v4l2_format *f)
{
    struct csi_dev *csi = video_drvdata(file);
    int ret;
    
    ret = csi_try_fmt(file, priv, f);
    if (ret < 0)
        return ret;
    
    mutex_lock(&csi->lock);
    
    /* 更新当前格式 */
    csi->width = f->fmt.pix.width;
    csi->height = f->fmt.pix.height;
    csi->field = f->fmt.pix.field;
    csi->bytesperline = f->fmt.pix.bytesperline;
    csi->sizeimage = f->fmt.pix.sizeimage;
    
    for (int i = 0; i < ARRAY_SIZE(csi_formats); i++) {
        if (csi_formats[i].fourcc == f->fmt.pix.pixelformat) {
            csi->fmt = &csi_formats[i];
            break;
        }
    }
    
    mutex_unlock(&csi->lock);
    
    return 0;
}

static int csi_enum_framesizes(struct file *file, void *priv,
                               struct v4l2_frmsizeenum *fsize)
{
    struct csi_dev *csi = video_drvdata(file);
    struct v4l2_subdev_frame_size_enum fse = {
        .index = fsize->index,
        .pad = 0,
    };
    int ret;
    
    /* 找到mbus_code */
    for (int i = 0; i < ARRAY_SIZE(csi_formats); i++) {
        if (csi_formats[i].fourcc == fsize->pixel_format) {
            fse.code = csi_formats[i].mbus_code;
            break;
        }
    }
    if (!fse.code)
        return -EINVAL;
    
    ret = v4l2_subdev_call(csi->sd, pad, enum_frame_size, NULL, &fse);
    if (ret < 0)
        return ret;
    
    fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
    fsize->discrete.width = fse.max_width;
    fsize->discrete.height = fse.max_height;
    
    return 0;
}

static int csi_enum_frameintervals(struct file *file, void *priv,
                                   struct v4l2_frmivalenum *fival)
{
    struct csi_dev *csi = video_drvdata(file);
    struct v4l2_subdev_frame_interval_enum fie = {
        .index = fival->index,
        .pad = 0,
        .width = fival->width,
        .height = fival->height,
    };
    int ret;
    
    for (int i = 0; i < ARRAY_SIZE(csi_formats); i++) {
        if (csi_formats[i].fourcc == fival->pixel_format) {
            fie.code = csi_formats[i].mbus_code;
            break;
        }
    }
    if (!fie.code)
        return -EINVAL;
    
    ret = v4l2_subdev_call(csi->sd, pad, enum_frame_interval, NULL, &fie);
    if (ret < 0)
        return ret;
    
    fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
    fival->discrete = fie.interval;
    
    return 0;
}

static int csi_g_parm(struct file *file, void *priv,
                      struct v4l2_streamparm *parm)
{
    struct csi_dev *csi = video_drvdata(file);
    
    if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
    
    return v4l2_subdev_call(csi->sd, video, g_parm, parm);
}

static int csi_s_parm(struct file *file, void *priv,
                      struct v4l2_streamparm *parm)
{
    struct csi_dev *csi = video_drvdata(file);
    
    if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
    
    return v4l2_subdev_call(csi->sd, video, s_parm, parm);
}

static int csi_enum_input(struct file *file, void *priv,
                          struct v4l2_input *inp)
{
    if (inp->index != 0)
        return -EINVAL;
    
    strscpy(inp->name, "Camera", sizeof(inp->name));
    inp->type = V4L2_INPUT_TYPE_CAMERA;
    
    return 0;
}

static int csi_g_input(struct file *file, void *priv, unsigned int *i)
{
    *i = 0;
    return 0;
}

static int csi_s_input(struct file *file, void *priv, unsigned int i)
{
    return (i == 0) ? 0 : -EINVAL;
}

static const struct v4l2_ioctl_ops csi_ioctl_ops = {
    .vidioc_querycap = csi_querycap,
    .vidioc_enum_fmt_vid_cap = csi_enum_fmt,
    .vidioc_try_fmt_vid_cap = csi_try_fmt,
    .vidioc_g_fmt_vid_cap = csi_g_fmt,
    .vidioc_s_fmt_vid_cap = csi_s_fmt,
    .vidioc_enum_framesizes = csi_enum_framesizes,
    .vidioc_enum_frameintervals = csi_enum_frameintervals,
    .vidioc_g_parm = csi_g_parm,
    .vidioc_s_parm = csi_s_parm,
    .vidioc_enum_input = csi_enum_input,
    .vidioc_g_input = csi_g_input,
    .vidioc_s_input = csi_s_input,
    .vidioc_reqbufs = vb2_ioctl_reqbufs,
    .vidioc_querybuf = vb2_ioctl_querybuf,
    .vidioc_qbuf = vb2_ioctl_qbuf,
    .vidioc_dqbuf = vb2_ioctl_dqbuf,
    .vidioc_streamon = vb2_ioctl_streamon,
    .vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations csi_fops = {
    .owner = THIS_MODULE,
    .open = v4l2_fh_open,
    .release = vb2_fop_release,
    .read = vb2_fop_read,
    .poll = vb2_fop_poll,
    .unlocked_ioctl = video_ioctl2,
    .mmap = vb2_fop_mmap,
};

/* ============== 异步子设备绑定 ============== */

static int csi_async_bound(struct v4l2_async_notifier *notifier,
                           struct v4l2_subdev *subdev,
                           struct v4l2_async_subdev *asd)
{
    struct csi_dev *csi = container_of(notifier, struct csi_dev, notifier);
    
    csi->sd = subdev;
    dev_info(csi->dev, "Bound subdevice: %s\n", subdev->name);
    
    return 0;
}

static void csi_async_unbind(struct v4l2_async_notifier *notifier,
                             struct v4l2_subdev *subdev,
                             struct v4l2_async_subdev *asd)
{
    struct csi_dev *csi = container_of(notifier, struct csi_dev, notifier);
    csi->sd = NULL;
}

static const struct v4l2_async_notifier_operations csi_async_ops = {
    .bound = csi_async_bound,
    .unbind = csi_async_unbind,
};

/* ============== 平台驱动 ============== */

static int csi_probe(struct platform_device *pdev)
{
    struct csi_dev *csi;
    struct resource *res;
    struct device_node *ep;
    int ret;
    
    csi = devm_kzalloc(&pdev->dev, sizeof(*csi), GFP_KERNEL);
    if (!csi)
        return -ENOMEM;
    
    csi->dev = &pdev->dev;
    
    /* 初始化锁和队列 */
    mutex_init(&csi->lock);
    spin_lock_init(&csi->slock);
    INIT_LIST_HEAD(&csi->buf_queue);
    
    /* 映射寄存器 */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    csi->reg_base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(csi->reg_base))
        return PTR_ERR(csi->reg_base);
    
    /* 获取中断 */
    csi->irq = platform_get_irq(pdev, 0);
    if (csi->irq < 0)
        return csi->irq;
    
    ret = devm_request_irq(&pdev->dev, csi->irq, csi_irq_handler, 0,
                          DRIVER_NAME, csi);
    if (ret)
        return ret;
    
    /* 获取时钟 */
    csi->clk_disp_axi = devm_clk_get(&pdev->dev, "disp-axi");
    if (IS_ERR(csi->clk_disp_axi))
        return PTR_ERR(csi->clk_disp_axi);
    
    csi->clk_disp_dcic = devm_clk_get(&pdev->dev, "disp_dcic");
    if (IS_ERR(csi->clk_disp_dcic))
        return PTR_ERR(csi->clk_disp_dcic);
    
    csi->clk_csi_mclk = devm_clk_get(&pdev->dev, "csi_mclk");
    if (IS_ERR(csi->clk_csi_mclk))
        return PTR_ERR(csi->clk_csi_mclk);
    
    /* 注册V4L2设备 */
    ret = v4l2_device_register(&pdev->dev, &csi->v4l2_dev);
    if (ret)
        return ret;
    
    /* 初始化VB2队列 */
    csi->vb2_q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    csi->vb2_q.io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ | VB2_DMABUF;
    csi->vb2_q.drv_priv = csi;
    csi->vb2_q.ops = &csi_vb2_ops;
    csi->vb2_q.mem_ops = &vb2_dma_contig_memops;
    csi->vb2_q.buf_struct_size = sizeof(struct csi_buffer);
    csi->vb2_q.lock = &csi->lock;
    csi->vb2_q.dev = &pdev->dev;
    csi->vb2_q.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    
    ret = vb2_queue_init(&csi->vb2_q);
    if (ret)
        goto err_v4l2;
    
    /* 初始化video设备 */
    csi->vdev.queue = &csi->vb2_q;
    csi->vdev.fops = &csi_fops;
    csi->vdev.ioctl_ops = &csi_ioctl_ops;
    csi->vdev.release = video_device_release_empty;
    csi->vdev.v4l2_dev = &csi->v4l2_dev;
    csi->vdev.lock = &csi->lock;
    csi->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    strscpy(csi->vdev.name, "imx6ull-csi", sizeof(csi->vdev.name));
    video_set_drvdata(&csi->vdev, csi);
    
    /* 设置默认格式 */
    csi->fmt = &csi_formats[0];
    csi->width = 640;
    csi->height = 480;
    csi->field = V4L2_FIELD_NONE;
    csi->bytesperline = 640 * 2;
    csi->sizeimage = 640 * 480 * 2;
    
    /* 设置异步通知器 */
    v4l2_async_notifier_init(&csi->notifier);
    
    ep = of_graph_get_next_endpoint(pdev->dev.of_node, NULL);
    if (ep) {
        v4l2_async_notifier_add_fwnode_remote_subdev(&csi->notifier,
            of_fwnode_handle(ep), sizeof(struct v4l2_async_subdev));
        of_node_put(ep);
    }
    
    csi->notifier.ops = &csi_async_ops;
    ret = v4l2_async_notifier_register(&csi->v4l2_dev, &csi->notifier);
    if (ret)
        goto err_v4l2;
    
    /* 注册video设备 */
    ret = video_register_device(&csi->vdev, VFL_TYPE_VIDEO, -1);
    if (ret)
        goto err_notifier;
    
    platform_set_drvdata(pdev, csi);
    pm_runtime_enable(&pdev->dev);
    
    dev_info(&pdev->dev, "i.MX6ULL CSI driver loaded\n");
    return 0;

err_notifier:
    v4l2_async_notifier_unregister(&csi->notifier);
    v4l2_async_notifier_cleanup(&csi->notifier);
err_v4l2:
    v4l2_device_unregister(&csi->v4l2_dev);
    return ret;
}

static int csi_remove(struct platform_device *pdev)
{
    struct csi_dev *csi = platform_get_drvdata(pdev);
    
    pm_runtime_disable(&pdev->dev);
    
    video_unregister_device(&csi->vdev);
    v4l2_async_notifier_unregister(&csi->notifier);
    v4l2_async_notifier_cleanup(&csi->notifier);
    v4l2_device_unregister(&csi->v4l2_dev);
    
    return 0;
}

static int csi_runtime_suspend(struct device *dev)
{
    return 0;
}

static int csi_runtime_resume(struct device *dev)
{
    return 0;
}

static const struct dev_pm_ops csi_pm_ops = {
    SET_RUNTIME_PM_OPS(csi_runtime_suspend, csi_runtime_resume, NULL)
};

static const struct of_device_id csi_of_match[] = {
    { .compatible = "fsl,imx6ull-csi" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, csi_of_match);

static struct platform_driver csi_driver = {
    .probe = csi_probe,
    .remove = csi_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = csi_of_match,
        .pm = &csi_pm_ops,
    },
};

module_platform_driver(csi_driver);

MODULE_DESCRIPTION("i.MX6ULL CSI Host Driver - Optimized");
MODULE_AUTHOR("Embedded Engineer");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);