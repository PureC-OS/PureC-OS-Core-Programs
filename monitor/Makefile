ROOT_DIR ?= $(abspath ../../..)
include $(ROOT_DIR)/mk/toolchain.mk

NAME := monitor
OUT := $(BIN_DIR)/programs
OBJ := $(BIN_DIR)/obj/monitor
LIB_DIR := $(BIN_DIR)/lib
LIBGUI_DIR ?= $(ROOT_DIR)/PureC-OS-LibGui
VB_DIR ?= $(ROOT_DIR)/video-backend
LIBPG := $(BIN_DIR)/libgui/libpg.so
LIBVB := $(BIN_DIR)/video-backend/libvb.so
LIBPUREC := $(LIB_DIR)/libpurec.so
CRT0 := $(DYN_CRT0)
LINKER_SCRIPT := $(ROOT_DIR)/linker-userspace-pie-interp.ld

CFLAGS := $(CPPFLAGS) -I$(LIBC_DIR)/include/hosted -I$(LIBGUI_DIR)/include -I$(VB_DIR)/include \
	$(DYN_CFLAGS) -O2 -Wall -Wextra $(DEPFLAGS)

OBJECTS := $(patsubst src/%.c,$(OBJ)/%.o,$(wildcard src/*.c))
TARGET := $(OUT)/$(NAME)

.PHONY: all clean
all: $(TARGET)

$(OBJ)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS) $(LIBPG) $(LIBVB) $(CRT0) $(LIBPUREC) $(LINKER_SCRIPT)
	@mkdir -p $(@D)
	$(LD) $(DYN_EXE_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) -L$(dir $(LIBPG)) -L$(dir $(LIBVB)) -lpg -lvb $(DYN_LIBPUREC)

$(LIBPG):
	$(MAKE) -C $(LIBGUI_DIR) ROOT_DIR=$(ROOT_DIR)

$(LIBVB):
	$(MAKE) -C $(VB_DIR) ROOT_DIR=$(ROOT_DIR)

$(CRT0) $(LIBPUREC):
	$(MAKE) -C $(LIBC_DIR)

clean:
	rm -rf $(OBJ) $(TARGET)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
