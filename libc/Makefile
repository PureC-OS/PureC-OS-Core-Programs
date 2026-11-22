ROOT_DIR ?= $(abspath ../../..)
include $(ROOT_DIR)/mk/toolchain.mk

OBJ_DIR := $(BIN_DIR)/obj/libc
LIB_DIR := $(BIN_DIR)/lib
OBJECTS := $(OBJ_DIR)/runtime.o $(OBJ_DIR)/stdlib.o $(OBJ_DIR)/string.o $(OBJ_DIR)/ctype.o $(OBJ_DIR)/stdio.o $(OBJ_DIR)/scanf.o $(OBJ_DIR)/unistd.o $(OBJ_DIR)/time.o $(OBJ_DIR)/setjmp.o $(OBJ_DIR)/dlfcn.o $(OBJ_DIR)/math.o $(OBJ_DIR)/libm.o $(OBJ_DIR)/signal.o $(OBJ_DIR)/locale.o $(OBJ_DIR)/process.o $(OBJ_DIR)/posix.o $(OBJ_DIR)/tty.o $(OBJ_DIR)/socket.o $(OBJ_DIR)/syslog.o $(OBJ_DIR)/event.o $(OBJ_DIR)/posix_extra.o $(OBJ_DIR)/compat.o $(OBJ_DIR)/regex.o $(OBJ_DIR)/iconv.o $(OBJ_DIR)/malloc.o $(OBJ_DIR)/fenv.o $(OBJ_DIR)/pthread.o $(OBJ_DIR)/wchar.o
SO_OBJ_DIR := $(OBJ_DIR)/pic
SO_OBJECTS := $(OBJECTS:$(OBJ_DIR)/%.o=$(SO_OBJ_DIR)/%.o)
TARGET := $(LIB_DIR)/libpurec.a
SO_REAL := $(LIB_DIR)/libpurec.so.$(LIBPUREC_SOVER)
SO_SONAME := libpurec.so.$(LIBPUREC_SOMAJOR)
SO_SONAME_LINK := $(LIB_DIR)/$(SO_SONAME)
SO_TARGET := $(LIB_DIR)/libpurec.so
CRT0_PIC := $(LIB_DIR)/crt0-pic.o
SO_LD_SCRIPT := $(LIBC_DIR)/userspace-so.ld
CRT0 := $(LIB_DIR)/crt0.o
LD_OBJ_DIR := $(OBJ_DIR)/ld
LD_STUB := $(LIB_DIR)/pc-ld.o
LD_STUB_OBJECTS := $(LD_OBJ_DIR)/ld.o $(LD_OBJ_DIR)/ld_entry.o $(LD_OBJ_DIR)/ld_tls.o
LD_STUB_CFLAGS := -g -O2 -ffreestanding -fno-builtin -fno-stack-protector -fPIE \
	-fvisibility=hidden -fno-plt -fno-tree-loop-distribute-patterns -Wa,--noexecstack -m64 -mno-red-zone -mcmodel=small

LD_INTERP := $(LIB_DIR)/ld-purec.so.1
LD_INTERP_OBJECTS := $(LD_OBJ_DIR)/ld_interp.o $(LD_OBJ_DIR)/ld_interp_entry.o $(LD_OBJ_DIR)/ld_tls.o

.PHONY: all
all: $(TARGET) $(SO_TARGET) $(CRT0) $(CRT0_PIC) $(LD_STUB) $(LD_INTERP)

-include $(OBJECTS:.o=.d)
-include $(SO_OBJECTS:.o=.d)
-include $(LD_STUB_OBJECTS:.o=.d)
-include $(LD_INTERP_OBJECTS:.o=.d)

$(TARGET): $(OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(SO_REAL): $(SO_OBJECTS) $(SO_LD_SCRIPT)
	@mkdir -p $(@D)
	$(LD) $(SHLIB_LDFLAGS) $(call SHLIB_SONAME,$(SO_SONAME)) -T $(SO_LD_SCRIPT) -o $@ $(SO_OBJECTS)

$(SO_SONAME_LINK): $(SO_REAL)
	ln -sf $(notdir $<) $@

$(SO_TARGET): $(SO_SONAME_LINK)
	ln -sf $(SO_SONAME) $@

$(CRT0_PIC): $(LIBC_DIR)/crt0.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(DYN_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(LIBC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(LIBC_DIR)/%.S
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(USER_CFLAGS) -c $< -o $@

$(SO_OBJ_DIR)/%.o: $(LIBC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(USER_SO_CFLAGS) -c $< -o $@

$(SO_OBJ_DIR)/%.o: $(LIBC_DIR)/%.S
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(USER_SO_CFLAGS) -c $< -o $@

# Hosted entry point, deliberately NOT archived into libpurec.a (native
# programs define their own _start). Ported programs link it explicitly.
$(CRT0): $(LIBC_DIR)/crt0.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(USER_CFLAGS) -c $< -o $@

$(LD_STUB): $(LD_STUB_OBJECTS)
	@mkdir -p $(@D)
	$(LD) -r -o $@ $^

$(LD_INTERP): $(LD_INTERP_OBJECTS)
	@mkdir -p $(@D)
	$(LD) $(INTERP_LINK_FLAGS) -o $@ $^

$(LD_OBJ_DIR)/ld_interp.o: $(LIBC_DIR)/ld/ld.c

$(LD_OBJ_DIR)/%.o: $(LIBC_DIR)/ld/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(LD_STUB_CFLAGS) -c $< -o $@

$(LD_OBJ_DIR)/%.o: $(LIBC_DIR)/ld/%.S
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(LD_STUB_CFLAGS) -c $< -o $@
