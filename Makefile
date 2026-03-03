# ===============================
# 1. 交叉编译相关配置
# ===============================

# ?= 表示“如果外部没传进来，就使用这个默认值”
# 好处：可以在命令行覆盖
# 例如： make ARCH=arm64
ARCH ?= arm

# 交叉编译器前缀
# 实际使用时会变成：
# arm-buildroot-linux-gnueabihf-gcc
CROSS_COMPILE ?= arm-buildroot-linux-gnueabihf-

# 内核源码目录
# 驱动编译时会用到
KERN_DIR ?= /home/book/100ask_imx6ull-sdk/Linux-4.9.88


# ===============================
# 2. 导出变量给子目录
# ===============================

# export 表示：子 Makefile 也能使用这些变量
# 非常重要！！！
export ARCH
export CROSS_COMPILE
export KERN_DIR


# ===============================
# 3. 定义子模块目录
# ===============================

# 所有需要编译的子目录
# 以后新增模块只需在这里加名字
SUBDIRS := drivers media ai ui app 

# ===============================
# 4. 声明伪目标
# ===============================

# .PHONY 表示这些不是文件名，而是“动作”
.PHONY: all clean $(SUBDIRS)


# ===============================
# 5. 默认目标
# ===============================

# 执行 make 时默认执行 all
# all 依赖 SUBDIRS 中的所有目录
# 这样可以支持 make -j 并行编译
all: $(SUBDIRS)


# ===============================
# 6. 子目录构建规则
# ===============================

# $@ 表示当前目标名
# 比如：
# make drivers
# $@ 就是 drivers
$(SUBDIRS):
	$(MAKE) -C $@

# 解释：
# -C 表示进入目录执行 make
# 等价于：
# cd drivers && make


# ===============================
# 7. 清理规则
# ===============================

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done

# 解释：
# $$dir 是 shell 变量
# 单个 $ 是 make 变量
# 所以这里要写 $$ 才能传给 shell
