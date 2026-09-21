# ============================================================
# 机房环境多指标远程监测与分析系统 —— 服务器端 Makefile
#   目标：编译 src/ 下全部 .c（用 wildcard 自动收录，S1~S6 新增模块无需改本文件）
#   依赖：gcc + libmysqlclient-dev（链接 -lmysqlclient -lpthread）
#   用法：make          编译，产出 ./server
#         make clean    清理
# ============================================================

CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -g -Isrc
LDLIBS   := -lmysqlclient -lpthread

SRC_DIR  := src
OBJ_DIR  := build
TARGET   := server

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)
	@echo "==> 构建完成：./$(TARGET)"

# 编译（-MMD -MP 生成头文件依赖，改了 .h 会自动重编）
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

clean:
	@rm -rf $(OBJ_DIR) $(TARGET)
	@echo "==> 已清理"

-include $(DEPS)

.PHONY: all clean
