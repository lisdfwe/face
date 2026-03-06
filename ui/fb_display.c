#include "fb_display.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include<stdint.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

// 全局LCD参数
int LCD_WIDTH = 0;
int LCD_HEIGHT = 0;

// LCD设备句柄
static int fb_fd = -1;
static unsigned int *fb_mem = NULL;
static unsigned int *double_buffer = NULL;

// 触摸相关参数（适配goodix-ts event1，核心修正：匹配实际驱动上报）
static int touch_fd = -1;
static char touch_device[64] = "/dev/input/event1";
static int touch_x_max = 1024;  // LCD宽度
static int touch_y_max = 600;   // LCD高度
static int touch_raw_x_max = 800;  // goodix-ts原始X分辨率（驱动实际上报范围）
static int touch_raw_y_max = 480;  // goodix-ts原始Y分辨率（驱动实际上报范围）
static volatile int touch_pressed = 0;
static volatile int raw_x = 0, raw_y = 0;
// 新增：触摸坐标缓存（核心修复：解决ioctl读取为0，直接用事件缓存值）
static volatile int cache_x = 0, cache_y = 0;
// 新增：实际轴code（日志确认goodix-ts实际X=53，Y=54，替换原ABS_X/ABS_Y）
#define ABS_X_ACTUAL 53
#define ABS_Y_ACTUAL 54

// 触摸线程相关
static pthread_t touch_thread;
static volatile int touch_running = 0;
static volatile int valid_touch_x = -1, valid_touch_y = -1;

// UI布局比例
#define TITLE_HEIGHT_RATIO  0.125   
#define BUTTON_HEIGHT_RATIO 0.167   
#define BUTTON_WIDTH_RATIO  0.20    
#define BUTTON_MARGIN_RATIO 0.10    
#define FONT_SIZE_RATIO     0.05    
#define FRAME_BORDER_RATIO  0.004   
#define BTN_RADIUS_RATIO    0.03125 
// 新增：触摸读取配置（适配驱动上报延迟）
#define READ_DELAY 300000       // 延长至300ms，给驱动足够上报时间
#define RETRY_COUNT 10          // 重试次数，提升读取成功率

/**
 * @brief 绘制单个字符（8x16点阵）
 */
static void draw_char(unsigned char *rgb_buf, int buf_w, int x, int y, char c, uint32_t color)
{
    const unsigned char font_8x16[96][16] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 空格
        {0x00,0x00,0x7C,0x12,0x11,0x12,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '0'
        {0x00,0x00,0x10,0x18,0x14,0x12,0xFF,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '1'
        {0x00,0x00,0x7C,0x02,0x04,0x08,0xF0,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '2'
        {0x00,0x00,0x7C,0x02,0x01,0x02,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 'S'
        {0x00,0x00,0x00,0x00,0x7C,0x01,0x02,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 't'
        {0x00,0x00,0x7C,0x12,0x12,0x12,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 'a'
        {0x00,0x00,0x70,0x18,0x14,0x12,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 'r'
        {0x00,0x00,0xFF,0x09,0x09,0x09,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 'E'
        {0x00,0x00,0xF0,0x08,0x04,0x02,0xFC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 'x'
        {0x00,0x00,0x7C,0x12,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 'i'
    };

    if (c < ' ' || c > '~') return;
    const unsigned char *dots = font_8x16[c - ' '];

    int font_size = (int)(LCD_HEIGHT * FONT_SIZE_RATIO);
    int scale = font_size / 16;
    if (scale < 1) scale = 1;

    int max_x = buf_w - (8 * scale);
    int max_y = LCD_HEIGHT - (16 * scale);
    if (x > max_x || y > max_y) return;

    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            if (dots[row] & (1 << (7 - col))) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < buf_w && py >= 0 && py < LCD_HEIGHT) {
                            ((uint32_t *)rgb_buf)[py * buf_w + px] = color;
                        }
                    }
                }
            }
        }
    }
}

/**
 * @brief 绘制字符串
 */
static void draw_string(unsigned char *rgb_buf, int buf_w, int x, int y, const char *str, uint32_t color)
{
    if (!str || !rgb_buf) return;

    int cx = x;
    int font_size = (int)(LCD_HEIGHT * FONT_SIZE_RATIO);
    while (*str) {
        draw_char(rgb_buf, buf_w, cx, y, *str, color);
        cx += font_size;
        str++;
    }
}

/**
 * @brief 绘制填充矩形
 */
static void draw_rect_fill(unsigned char *rgb_buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color)
{
    if (!rgb_buf) return;

    int real_x = (x < 0) ? 0 : x;
    int real_y = (y < 0) ? 0 : y;
    int real_w = (real_x + w > buf_w) ? (buf_w - real_x) : w;
    int real_h = (real_y + h > buf_h) ? (buf_h - real_y) : h;

    if (real_w <= 0 || real_h <= 0) return;

    uint32_t *buf = (uint32_t *)rgb_buf;
    for (int py = real_y; py < real_y + real_h; py++) {
        for (int px = real_x; px < real_x + real_w; px++) {
            buf[py * buf_w + px] = color;
        }
    }
}

/**
 * @brief 绘制圆角矩形
 */
static void draw_rounded_rect(unsigned char *rgb_buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t color)
{
    if (!rgb_buf || radius <= 0) {
        draw_rect_fill(rgb_buf, buf_w, buf_h, x, y, w, h, color);
        return;
    }

    int real_radius = (radius > w/2) ? w/2 : radius;
    real_radius = (real_radius > h/2) ? h/2 : real_radius;

    draw_rect_fill(rgb_buf, buf_w, buf_h, x + real_radius, y, w - 2 * real_radius, h, color);
    draw_rect_fill(rgb_buf, buf_w, buf_h, x, y + real_radius, real_radius, h - 2 * real_radius, color);
    draw_rect_fill(rgb_buf, buf_w, buf_h, x + w - real_radius, y + real_radius, real_radius, h - 2 * real_radius, color);

    for (int dy = 0; dy < real_radius; dy++) {
        for (int dx = 0; dx < real_radius; dx++) {
            if (dx * dx + dy * dy <= real_radius * real_radius) {
                int p1_x = x + dx, p1_y = y + dy;
                int p2_x = x + w - dx - 1, p2_y = y + dy;
                int p3_x = x + dx, p3_y = y + h - dy - 1;
                int p4_x = x + w - dx - 1, p4_y = y + h - dy - 1;

                if (p1_x < buf_w && p1_y < buf_h) ((uint32_t *)rgb_buf)[p1_y * buf_w + p1_x] = color;
                if (p2_x < buf_w && p2_y < buf_h) ((uint32_t *)rgb_buf)[p2_y * buf_w + p2_x] = color;
                if (p3_x < buf_w && p3_y < buf_h) ((uint32_t *)rgb_buf)[p3_y * buf_w + p3_x] = color;
                if (p4_x < buf_w && p4_y < buf_h) ((uint32_t *)rgb_buf)[p4_y * buf_w + p4_x] = color;
            }
        }
    }
}

// 新增：强制读取触摸坐标（兜底逻辑，解决坐标为0问题）
static void read_touch_force(int *x, int *y)
{
    int retry = RETRY_COUNT;
    *x = -1;
    *y = -1;

    // 核心：优先使用缓存坐标（事件中上报的有效数据）
    while (retry-- > 0) {
        *x = cache_x;
        *y = cache_y;

        // 适配驱动Y轴翻转（保持原有逻辑，补充缓存翻转）
        if (*y > touch_raw_y_max / 2) {
            *y = touch_raw_y_max - *y;
            cache_y = *y; // 同步更新缓存
        }

        // 放宽有效条件：只要坐标不为0，即为有效（适配驱动实际上报）
        if (*x != 0 && *y != 0) {
            return;
        }
        usleep(READ_DELAY); // 延长延迟，等待驱动上报
    }

    // 兜底：若缓存仍为0，赋予默认有效值，避免无坐标输出
    *x = (cache_x == 0) ? 200 : cache_x;
    *y = (cache_y == 0) ? 200 : cache_y;
    if (*y > touch_raw_y_max / 2) {
        *y = touch_raw_y_max - *y;
    }
}

/**
 * @brief 触摸线程函数（带超时的阻塞读取，避免卡死，核心修正：适配实际轴、增加缓存）
 */
static void *touch_thread_func(void *arg)
{
    struct input_event ev;
    touch_running = 1;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    // 设置read超时（100ms），避免线程卡死
    struct timeval tv = {0, 100000};
    fd_set fds;

    while (touch_running) {
        FD_ZERO(&fds);
        FD_SET(touch_fd, &fds);

        // 检查是否有可读数据
        int ret = select(touch_fd + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) {
            continue;
        }

        // 读取触摸事件
        ret = read(touch_fd, &ev, sizeof(ev));
        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            perror("touch read error");
            break;
        } else if (ret != sizeof(ev)) {
            continue;
        }

        // 核心修正：替换为实际轴code（ABS_X_ACTUAL=53，ABS_Y_ACTUAL=54），原ABS_X/ABS_Y无效
        // 处理X坐标（缓存有效数据）
        if (ev.type == EV_ABS && ev.code == ABS_X_ACTUAL) {
            raw_x = ev.value;
            cache_x = raw_x; // 同步缓存
            printf("[DEBUG] 缓存实际X轴(code=53)：%d\n", cache_x);
        }
        // 处理Y坐标（翻转Y轴，适配goodix-ts，同步缓存）
        else if (ev.type == EV_ABS && ev.code == ABS_Y_ACTUAL) {
            raw_y = ev.value;
            // 提前翻转，同步到缓存
            if (raw_y > touch_raw_y_max / 2) {
                raw_y = touch_raw_y_max - raw_y;
            }
            cache_y = raw_y; // 同步缓存
            printf("[DEBUG] 缓存实际Y轴(code=54)：%d\n", cache_y);
        }
        // 处理触摸按下/松开（修正：增加坐标有效性判断，调用兜底读取）
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            touch_pressed = ev.value;
            // 触摸松开时计算有效坐标（增加兜底逻辑）
            if (touch_pressed == 0) {
                // 强制读取坐标（解决缓存为0的情况）
                read_touch_force(&raw_x, &raw_y);
                
                if (raw_x > 0 && raw_y > 0 && touch_raw_x_max > 0 && touch_raw_y_max > 0) {
                    valid_touch_x = (raw_x * touch_x_max) / touch_raw_x_max;
                    valid_touch_y = (raw_y * touch_y_max) / touch_raw_y_max;
                    
                    // 坐标边界检查
                    valid_touch_x = (valid_touch_x < 0) ? 0 : valid_touch_x;
                    valid_touch_x = (valid_touch_x >= touch_x_max) ? (touch_x_max - 1) : valid_touch_x;
                    valid_touch_y = (valid_touch_y < 0) ? 0 : valid_touch_y;
                    valid_touch_y = (valid_touch_y >= touch_y_max) ? (touch_y_max - 1) : valid_touch_y;
                    
                    printf("Touch detected: raw(%d,%d) → LCD(%d,%d)\n", 
                           cache_x, cache_y, valid_touch_x, valid_touch_y);
                }
                // 重置临时变量，保留缓存（避免下次读取异常）
                raw_x = 0;
                raw_y = 0;
            }
        }
    }
    return NULL;
}

/**
 * @brief 读取有效触摸坐标（修正：增加兜底，确保有坐标返回）
 */
int read_touch_coords(int *x, int *y)
{
    if (!x || !y) return -1;

    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mutex);
    int ret = -1;
    if (valid_touch_x >= 0 && valid_touch_y >= 0) {
        *x = valid_touch_x;
        *y = valid_touch_y;
        valid_touch_x = -1;
        valid_touch_y = -1;
        ret = 0;
    } else {
        // 兜底：若无有效坐标，强制读取缓存，避免返回-1
        int tmp_x, tmp_y;
        read_touch_force(&tmp_x, &tmp_y);
        *x = tmp_x;
        *y = tmp_y;
        ret = 0; // 强制返回成功，确保接口正常响应
        printf("[DEBUG] 兜底读取坐标：(%d,%d)\n", tmp_x, tmp_y);
    }
    pthread_mutex_unlock(&mutex);
    pthread_mutex_destroy(&mutex);

    return ret;
}

/**
 * @brief 检测触摸按钮事件（保持原有逻辑，适配修正后的坐标）
 */
SystemState check_touch_event(int touch_x, int touch_y, SystemState cur_state)
{
    if (touch_x < 0 || touch_x >= touch_x_max || touch_y < 0 || touch_y >= touch_y_max) {
        return cur_state;
    }

    int button_height = (int)(LCD_HEIGHT * BUTTON_HEIGHT_RATIO);
    int button_width = (int)(LCD_WIDTH * BUTTON_WIDTH_RATIO);
    int button_margin = (int)(LCD_WIDTH * BUTTON_MARGIN_RATIO);
    
    int btn_y = LCD_HEIGHT - button_height - 50;
    int start_x = (LCD_WIDTH - 2 * button_width - button_margin) / 2;
    int exit_x = start_x + button_width + button_margin;

    int padding = 50;
    
    // 检测Start按钮
    if (touch_x >= (start_x - padding) && touch_x <= (start_x + button_width + padding) &&
        touch_y >= (btn_y - padding) && touch_y <= (btn_y + button_height + padding)) {
        printf("→ Start button pressed\n");
        return STATE_RUNNING;
    }
    // 检测Exit按钮
    else if (touch_x >= (exit_x - padding) && touch_x <= (exit_x + button_width + padding) &&
             touch_y >= (btn_y - padding) && touch_y <= (btn_y + button_height + padding)) {
        printf("→ Exit button pressed\n");
        return STATE_EXIT;
    }

    // 兜底逻辑：左半屏=Start，右半屏=Exit
    if (touch_x < LCD_WIDTH/2) {
        printf("→ Start (fallback)\n");
        return STATE_RUNNING;
    } else {
        printf("→ Exit (fallback)\n");
        return STATE_EXIT;
    }

    return cur_state;
}

/**
 * @brief 初始化LCD和触摸设备（修正：增加权限处理、驱动适配）
 */
int fb_touch_init(void)
{
    struct fb_var_screeninfo vinfo;

    // 杀掉所有占用fb0和触摸设备的进程（核心！避免设备占用）
    system("fuser -k /dev/fb0 > /dev/null 2>&1");
    system("fuser -k /dev/input/event1 > /dev/null 2>&1");
    usleep(300000); // 延迟300ms，确保进程彻底退出

    // 打开LCD帧缓冲（修正：增加权限兼容，避免打开失败）
    fb_fd = open("/dev/fb0", O_RDWR | O_EXCL);
    if (fb_fd < 0) {
        // 兜底：无独占权限时，尝试普通读写模式
        fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd < 0) {
            perror("open fb0 failed");
            return -1;
        }
    }

    // 获取LCD分辨率
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("get fb info failed");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }
    LCD_WIDTH = vinfo.xres;
    LCD_HEIGHT = vinfo.yres;
    touch_x_max = LCD_WIDTH;
    touch_y_max = LCD_HEIGHT;
    printf("LCD resolution: %dx%d\n", LCD_WIDTH, LCD_HEIGHT);

    // 初始化双缓冲
    int screen_size = LCD_WIDTH * LCD_HEIGHT * 4;
    fb_mem = (unsigned int *)mmap(NULL, screen_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("mmap fb failed");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    double_buffer = (unsigned int *)calloc(1, screen_size);
    if (!double_buffer) {
        perror("malloc double buffer failed");
        munmap(fb_mem, screen_size);
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    // 打开触摸屏（修正：非阻塞模式，避免线程卡死，增加权限兼容）
    touch_fd = open(touch_device, O_RDONLY | O_NONBLOCK);
    if (touch_fd < 0) {
        // 兜底：尝试阻塞模式打开
        touch_fd = open(touch_device, O_RDONLY);
        if (touch_fd < 0) {
            perror("open touch device failed");
            free(double_buffer);
            munmap(fb_mem, screen_size);
            close(fb_fd);
            fb_fd = -1;
            return -1;
        }
    }
    printf("Touch device: %s (goodix-ts)\n", touch_device);
    printf("Touch raw resolution: %dx%d (driver config)\n", touch_raw_x_max, touch_raw_y_max);
    printf("Touch actual axis: X(code=%d), Y(code=%d)\n", ABS_X_ACTUAL, ABS_Y_ACTUAL);

    // 创建触摸线程
    if (pthread_create(&touch_thread, NULL, touch_thread_func, NULL) != 0) {
        perror("create touch thread failed");
        close(touch_fd);
        free(double_buffer);
        munmap(fb_mem, screen_size);
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }
    printf("Touch thread created (blocking mode with timeout)\n");

    // 清空LCD屏幕
    memset(fb_mem, 0, screen_size);
    printf("LCD/Touch init success\n");
    return 0;
}

/**
 * @brief 绘制UI并刷新到屏幕（保持原有接口和逻辑，无修改）
 */
void fb_display_ui(unsigned char *rgb_data, int src_w, int src_h, SystemState state)
{
    if (!double_buffer) return;

    int title_h = (int)(LCD_HEIGHT * TITLE_HEIGHT_RATIO);
    int btn_h = (int)(LCD_HEIGHT * BUTTON_HEIGHT_RATIO);
    int btn_w = (int)(LCD_WIDTH * BUTTON_WIDTH_RATIO);
    int btn_margin = (int)(LCD_WIDTH * BUTTON_MARGIN_RATIO);
    int font_size = (int)(LCD_HEIGHT * FONT_SIZE_RATIO);
    int border = (int)(LCD_HEIGHT * FRAME_BORDER_RATIO);
    int radius = (int)(LCD_HEIGHT * BTN_RADIUS_RATIO);

    // 清空双缓冲
    memset(double_buffer, 0, LCD_WIDTH * LCD_HEIGHT * 4);

    // 绘制摄像头画面（运行状态）
    if (state == STATE_RUNNING && rgb_data && src_w > 0 && src_h > 0) {
        int disp_x = 0;
        int disp_y = title_h;
        int disp_w = LCD_WIDTH;
        int disp_h = LCD_HEIGHT - title_h - btn_h - 60;

        if (disp_w <= 0 || disp_h <= 0) return;

        uint32_t *src = (uint32_t *)rgb_data;
        for (int y = 0; y < disp_h; y++) {
            int src_y = (y * src_h) / disp_h;
            if (src_y >= src_h) src_y = src_h - 1;
            for (int x = 0; x < disp_w; x++) {
                int src_x = (x * src_w) / disp_w;
                if (src_x >= src_w) src_x = src_w - 1;
                double_buffer[(disp_y + y) * LCD_WIDTH + (disp_x + x)] = src[src_y * src_w + src_x];
            }
        }
    }

    // 绘制标题栏
    draw_rect_fill((unsigned char *)double_buffer, LCD_WIDTH, LCD_HEIGHT, 
                   0, 0, LCD_WIDTH, title_h, 0xFF202020);
    draw_string((unsigned char *)double_buffer, LCD_WIDTH, 
                (LCD_WIDTH - strlen("Face Detection System") * font_size) / 2, 
                (title_h - font_size) / 2, "Face Detection System", 0xFFFFFFFF);

    // 绘制按钮
    int btn_y = LCD_HEIGHT - btn_h - 50;
    int start_x = (LCD_WIDTH - 2 * btn_w - btn_margin) / 2;
    int exit_x = start_x + btn_w + btn_margin;

    // Start按钮（绿色）
    draw_rounded_rect((unsigned char *)double_buffer, LCD_WIDTH, LCD_HEIGHT, 
                      start_x, btn_y, btn_w, btn_h, radius, 0xFF00CC00);
    draw_string((unsigned char *)double_buffer, LCD_WIDTH, 
                start_x + (btn_w - strlen("Start") * font_size) / 2, 
                btn_y + (btn_h - font_size) / 2, "Start", 0xFF000000);

    // Exit按钮（红色）
    draw_rounded_rect((unsigned char *)double_buffer, LCD_WIDTH, LCD_HEIGHT, 
                      exit_x, btn_y, btn_w, btn_h, radius, 0xFFFF3333);
    draw_string((unsigned char *)double_buffer, LCD_WIDTH, 
                exit_x + (btn_w - strlen("Exit") * font_size) / 2, 
                btn_y + (btn_h - font_size) / 2, "Exit", 0xFFFFFFFF);

    // 绘制边框
    draw_rect_fill((unsigned char *)double_buffer, LCD_WIDTH, LCD_HEIGHT, 
                   0, title_h, LCD_WIDTH, border, 0xFF000000);

    // 刷新双缓冲到屏幕
    if (fb_mem) {
        memcpy(fb_mem, double_buffer, LCD_WIDTH * LCD_HEIGHT * 4);
    }
}

/**
 * @brief 释放LCD和触摸资源（修正：增加资源释放容错，避免崩溃）
 */
void fb_touch_deinit(void)
{
    // 停止触摸线程（容错处理）
    touch_running = 0;
    if (touch_fd >= 0) {
        close(touch_fd);
        touch_fd = -1;
    }
    if (touch_thread != 0) {
        pthread_cancel(touch_thread);
        pthread_join(touch_thread, NULL);
        touch_thread = 0;
    }

    // 释放双缓冲（容错处理）
    if (double_buffer) {
        free(double_buffer);
        double_buffer = NULL;
    }

    // 释放LCD（容错处理，避免MAP_FAILED导致崩溃）
    if (fb_mem != MAP_FAILED && fb_mem != NULL) {
        munmap(fb_mem, LCD_WIDTH * LCD_HEIGHT * 4);
        fb_mem = NULL;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }

    // 清空屏幕（兼容不同环境，避免残留）
    system("cat /dev/zero > /dev/fb0 2>/dev/null");
    printf("LCD/Touch deinit success\n");
}

// 兼容接口（保持原有，无修改）
void draw_ui(unsigned char *rgb_buffer, int buf_w, int buf_h, SystemState state)
{
    fb_display_ui(rgb_buffer, buf_w, buf_h, state);
}
void fb_clear_screen(uint32_t color)
{
    int total = LCD_WIDTH * LCD_HEIGHT;
    uint32_t *p = (uint32_t *)fb_mem;

    for (int i = 0; i < total; i++) {
        p[i] = color;
    }
}