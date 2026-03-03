#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <linux/fb.h>
#include "drm_display.h"

int drm_init(int w, int h, uint32_t **vaddr, int *line_length) {
    int fd;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    char *fbp = NULL;

    printf("DEBUG: Opening framebuffer device (/dev/fb0)...\n");

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("Error: cannot open framebuffer device");
        return -1;
    }

    // 获取屏幕固定参数 (Stride 在这里)
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("Error reading fixed information");
        close(fd);
        return -1;
    }
    
    // 获取屏幕可变参数 (分辨率在这里)
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("Error reading variable information");
        close(fd);
        return -1;
    }

    // 关键：将驱动的行跨距传出
    *line_length = finfo.line_length;
    
    printf("DEBUG: Framebuffer Resolution: %dx%d, %dbpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
    printf("DEBUG: Framebuffer Stride (line_length): %d\n", *line_length);

    // 映射显存，使用 finfo.smem_len 确保映射足够大
    fbp = (char *)mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if ((int)fbp == -1) {
        perror("Error: failed to map framebuffer device to memory");
        close(fd);
        return -1;
    }

    *vaddr = (uint32_t *)fbp;
    close(fd); // 文件描述符在 mmap 后可关闭
    return 0;
}