# ui/Makefile
SRCS = fb_display.c
OBJS = $(SRCS:.c=.o)

# 如果不需要 DRM，可以注释掉
# CFLAGS += -I$(SYSROOT)/usr/include/libdrm

all: $(OBJS)

# 每个 .o 只编译对应的 .c 文件
%.o: %.c
	$(CROSS_COMPILE)gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o

.PHONY: all clean