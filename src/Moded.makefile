PROBE_HOST ?= stlinkv3
NO_BOOTLOADER ?= 1
PLATFORM_DIR = platforms/$(PROBE_HOST)
VPATH += $(PLATFORM_DIR) target
ENABLE_DEBUG ?=

ifneq ($(V), 1)
MAKEFLAGS += --no-print-dir
Q := @
endif

CFLAGS += -Wall -Wextra -Werror -Wno-char-subscripts \
	-std=gnu99 -g3 -MD -I./target \
	-I. -Iinclude -I$(PLATFORM_DIR)

ifeq ($(ENABLE_DEBUG), 1)
CFLAGS += -DENABLE_DEBUG
endif

SRC =			\
	main.c		\
	platform.c	\
 	usbuart.c	\
	cdcacm.c	\
	serialno.c	\
        stm32-slcan.c   \
	timing.c	\
	timing_stm32.c	\

include platforms/stlinkv3/Makefile.inc

OPT_FLAGS ?= -Os
CFLAGS += $(OPT_FLAGS)
LDFLAGS += $(OPT_FLAGS)

TARGET = blackmagic

OBJ = $(patsubst %.S,%.o,$(patsubst %.c,%.o,$(SRC)))

all:	blackmagic.elf

%.bin: %.elf
       @echo "  OBJCOPY $@"
       $(Q)$(OBJCOPY) -O binary $^ $@

%.hex: %.elf
       @echo "  OBJCOPY $@"

blackmagic.elf:  usb_f723.o \
		usb_dwc_common.o usb_control.o
	@echo "  LD      $@"
	$(Q)$(CC) $^ -o $@ $(LDFLAGS_BOOT)


$(TARGET): include/version.h $(OBJ)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o:	%.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

%.o:	%.S
	@echo "  AS      $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

.PHONY:	clean host_clean all_platforms FORCE

clean:	host_clean
	$(Q)echo "  CLEAN"
	-$(Q)$(RM) *.o *.d *.elf *~ $(TARGET) $(HOSTFILES)
	-$(Q)$(RM) platforms/*/*.o platforms/*/*.d mapfile include/version.h

all_platforms:
	$(Q)set -e ;\
	mkdir -p artifacts/$(shell git describe --always --dirty --tags) ;\
	echo "<html><body><ul>" > artifacts/index.html ;\
	for i in platforms/*/Makefile.inc ; do \
		export DIRNAME=`dirname $$i` ;\
		export PROBE_HOST=`basename $$DIRNAME` ;\
		export CFLAGS=-Werror ;\
		echo "Building for hardware platform: $$PROBE_HOST" ;\
		$(MAKE) clean ;\
		$(MAKE);\
		if [ -f blackmagic.bin ]; then \
			mv blackmagic.bin artifacts/blackmagic-$$PROBE_HOST.bin ;\
			echo "<li><a href='blackmagic-$$PROBE_HOST.bin'>$$PROBE_HOST</a></li>"\
				>> artifacts/index.html ;\
		fi ;\
		if [ -f blackmagic_dfu.bin ]; then \
			mv blackmagic_dfu.bin artifacts/blackmagic_dfu-$$PROBE_HOST.bin ;\
			echo "<li><a href='blackmagic_dfu-$$PROBE_HOST.bin'>$$PROBE_HOST DFU</a></li>"\
				>> artifacts/index.html ;\
		fi ;\
	done ;\
	echo "</ul></body></html>" >> artifacts/index.html ;\
	cp artifacts/*.bin artifacts/$(shell git describe --always --dirty --tags) \
    mv blackmagic.bin slv3app1.bin
command.c: include/version.h

include/version.h: FORCE
	$(Q)echo " GIT include/version.h"
	$(Q)echo "#define FIRMWARE_VERSION \"$(shell git describe --always --dirty --tags)\"" > $@
-include *.d

