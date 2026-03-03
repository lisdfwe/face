# ===============================
# 编译器
# ===============================

CXX = $(CROSS_COMPILE)g++

# ===============================
# ncnn 路径
# ===============================

NCNN_DIR = ../third_party/ncnn

# 头文件路径
CXXFLAGS = -I$(NCNN_DIR)/include

# ===============================
# 目标文件
# ===============================

SRCS = face_detect.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o