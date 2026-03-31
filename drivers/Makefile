# ============================================================================
# 文件：drivers/Makefile（修复版）
# ============================================================================

# 1. 内核源码路径
KERN_DIR = /home/book/100ask_imx6ull-sdk/Linux-4.9.88

# 2. 当前路径
PWD := $(shell pwd)

# 3. 内核模块声明（推荐：一行声明所有模块）
obj-m := motor_driver.o ov5640.o

# 4. 默认目标
all:
	@echo "========================================="
	@echo "  Building kernel modules..."
	@echo "  Kernel: $(KERN_DIR)"
	@echo "  Path: $(PWD)"
	@echo "  Modules: $(obj-m)"
	@echo "========================================="
	$(MAKE) -C $(KERN_DIR) M=$(PWD) modules
	@echo ""
	@echo "✓ Build complete"
	@echo "  Generated .ko files:"
	@ls -1 *.ko 2>/dev/null || echo "  None!"

# 5. 清理
clean:
	@echo "Cleaning kernel modules..."
	$(MAKE) -C $(KERN_DIR) M=$(PWD) clean
	rm -f *.ko *.o .*.cmd .*.flags
	rm -f *.mod.c *.mod.o
	rm -f modules.order Module.symvers
	rm -rf .tmp_versions
	@echo "✓ Clean complete"

.PHONY: all clean