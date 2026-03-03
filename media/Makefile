SRCS = v4l2_camera.c
OBJS = $(SRCS:.c=.o)

all: $(OBJS)

%.o: %.c
	$(CROSS_COMPILE)gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o