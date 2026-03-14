# 1. 内核源码路径 (请确保路径准确)
KERN_DIR = /home/book/100ask_imx6ull-sdk/Linux-4.9.88

# 2. 驱动模块文件名 (对应 motor_driver.c)
MODULE_NAME = motor_driver

# 3. 当前绝对路径
PWD = $(shell pwd)

# 核心：告诉内核构建系统要生成的模块目标
obj-m += $(MODULE_NAME).o

all:
	@echo "开始编译内核模块..."
	$(MAKE) -C $(KERN_DIR) M=$(PWD) modules

clean:
	@echo "清理编译生成的文件..."
	$(MAKE) -C $(KERN_DIR) M=$(PWD) clean
	rm -f modules.order Module.symvers