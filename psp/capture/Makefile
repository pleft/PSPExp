# pspexp_capture.prx: PSPExp Capture, a kernel plugin for custom firmware's game.txt
# (main.c, systemctrl.S: the custom firmware's functions it uses; PSPExp's USB driver built in from ../transport-usb)
TARGET = pspexp_capture
OBJS = main.o systemctrl.o

USE_KERNEL_LIBS = 1
USE_KERNEL_LIBC = 1
INCDIR = ../transport-usb ../../protocol
CFLAGS = -O2 -mno-gpopt -Wall -fno-builtin-printf -MMD -MP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LIBDIR =
LIBS = -lpspusbbus_driver -lpspusb_driver -lpspctrl_driver -lpsprtc_driver -lpspreg_driver -lpspdisplay_driver -lpspdmac -lpsppower_driver -lpspge_driver

# make DEV=1: a developer build (the log also on the memory stick, new plugins and restarts over the cable)
ifeq ($(DEV),1)
CFLAGS += -DPSPEXP_CAPTURE_DEV
endif

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

-include $(OBJS:.o=.d)
