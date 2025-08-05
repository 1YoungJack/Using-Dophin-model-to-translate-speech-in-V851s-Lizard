# 编译器设置
CC = arm-openwrt-linux-gcc

# 包含路径
INCLUDES = -I./alsa/include \
           -I./libwebsockets/include \
           -I./openssl/include \
           -I./sherpa-onnx/include \

# 库路径
LIB_PATHS = -L./alsa/lib \
            -L./libwebsockets/lib \
            -L./openssl/lib \
            -L./sherpa-onnx/lib \

# 链接库
LIBS = -lasound -lsherpa-onnx-c-api  -lwebsockets -lssl -lcrypto

# 目标文件
TARGET = vad_capture

# 源文件
SRCS = vad_capture.c cJSON.c

# 编译选项
CFLAGS = $(INCLUDES)
LDFLAGS = $(LIB_PATHS) $(LIBS)

# 默认目标
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 清理
clean:
	rm -f $(TARGET)

