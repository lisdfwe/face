#ifndef _IMX6_CSI_REGS_H
#define _IMX6_CSI_REGS_H

/* --- 寄存器偏移 (Offset from Base 0x021C4000) --- */
#define CSI_CSICR1      0x00    /* Control Register 1 */
#define CSI_CSICR2      0x04    /* Control Register 2 */
#define CSI_CSICR3      0x08    /* Control Register 3 */
#define CSI_CSISTATFIFO 0x0C    /* Status FIFO */
#define CSI_CSIRFIFO    0x10    /* RX FIFO Data */
#define CSI_CSIDMASA_FB1 0x24   /* DMA Buffer Address 1 */
#define CSI_CSIDMASA_FB2 0x28   /* DMA Buffer Address 2 */
#define CSI_CSIFBUF_PAR 0x2C    /* Frame Buffer Parameter */
#define CSI_CSICNT      0x30    /* Counter Register */

/* --- CSICR1 Bit Definitions --- */
#define BIT_CSI_EN          (1 << 31) /* Enable CSI */
#define BIT_SOF_INTEN       (1 << 7)  /* Start of Frame Interrupt Enable */
#define BIT_EOF_INTEN       (1 << 6)  /* End of Frame Interrupt Enable */
#define BIT_PRP_IF_EN       (1 << 5)  /* PRP Interface Enable */
#define BIT_CCIR_MODE       (1 << 4)  /* CCIR Mode (0: Progressive, 1: Interlaced) */
#define BIT_DATA_SWAP       (1 << 3)  /* Data Swap (YUYV vs UYVY) */
#define BIT_PACK_DIR        (1 << 2)  /* Packing Direction */
#define BIT_EXT_VSYNC_POL   (1 << 1)  /* External VSYNC Polarity (0: Active Low, 1: Active High) */
#define BIT_EXT_HSYNC_POL   (1 << 0)  /* External HSYNC Polarity */

/* --- CSICR2 Bit Definitions --- */
#define BIT_IMAGE_FMT_MASK  (0xF << 12) /* Image Format Mask */
#define VAL_YUYV_422        (0x0 << 12) /* YUYV 4:2:2 */
#define VAL_UYVY_422        (0x1 << 12) /* UYVY 4:2:2 */
#define VAL_RGB565          (0x4 << 12) /* RGB565 */
#define BIT_JPEG_EN         (1 << 8)  /* JPEG Mode Enable */

/* --- CSICR3 Bit Definitions (Clock & Polarity) --- */
#define BIT_PCLK_POL        (1 << 3)  /* Pixel Clock Polarity (0: Rising, 1: Falling) */
#define BIT_VSYNC_POL       (1 << 2)  /* VSYNC Polarity override */
#define BIT_HSYNC_POL       (1 << 1)  /* HSYNC Polarity override */
#define BIT_DATA_POL        (1 << 0)  /* Data Polarity */

/* --- Helper Macros --- */
#define CSI_REG_READ(base, off)      readl((base) + (off))
#define CSI_REG_WRITE(val, base, off) writel((val), (base) + (off))
#define CSI_REG_SET(base, off, bit)  CSI_REG_WRITE(CSI_REG_READ(base, off) | (bit), base, off)
#define CSI_REG_CLR(base, off, bit)  CSI_REG_WRITE(CSI_REG_READ(base, off) & ~(bit), base, off)

#endif