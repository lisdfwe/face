TARGET = face_security_app

CXX = $(CROSS_COMPILE)g++
CC  = $(CROSS_COMPILE)gcc

SRCS = main.cpp
OBJS = $(SRCS:.cpp=.o)

# 确保其他模块的路径正确
ALL_OBJS = $(OBJS) ../media/v4l2_camera.o ../ui/fb_display.o ../ai/face_detect.o 

NCNN_DIR = ../third_party/ncnn

CXXFLAGS += -I$(NCNN_DIR)/include
LDFLAGS  += -L$(NCNN_DIR)/lib

LDLIBS += -lncnn -lpthread -ldl -lm -ldrm -ljpeg 

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CXX) $(ALL_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGET)