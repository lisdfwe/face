#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

static int fb_fd;
static unsigned int *fb_mem;
static int screen_width;
static int screen_height;
static int screen_size;

int fb_init(void)
{
    struct fb_var_screeninfo vinfo;

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("open fb0");
        return -1;
    }

    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);

    screen_width  = vinfo.xres;
    screen_height = vinfo.yres;

    screen_size = screen_width * screen_height * 4;

    fb_mem = mmap(NULL, screen_size,
                  PROT_READ | PROT_WRITE,
                  MAP_SHARED,
                  fb_fd, 0);

    if (fb_mem == MAP_FAILED) {
        perror("mmap fb");
        return -1;
    }

    return 0;
}

void fb_display_fullscreen(void *rgb_data, int src_w, int src_h)
{
    int x, y;

    unsigned int *src = (unsigned int *)rgb_data;
    unsigned int *dst = fb_mem;

    for (y = 0; y < screen_height; y++) {

        int src_y = y * src_h / screen_height;

        for (x = 0; x < screen_width; x++) {

            int src_x = x * src_w / screen_width;

            dst[y * screen_width + x] =
                src[src_y * src_w + src_x];
        }
    }
}

void fb_deinit(void)
{
    munmap(fb_mem, screen_size);
    close(fb_fd);
}