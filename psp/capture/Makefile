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
ifneq ($(PARTS),)
CFLAGS += -DPSPEXP_CAPTURE_PARTS='"$(PARTS)"'
endif
endif

# make LOCAL=1: the virtual disc served from an ISO on the stick (ms0:/seplugins/pxd-local.txt), no
# USB and no log on the stick: the device alone under Inferno (main.c, vd_local_open)
ifeq ($(LOCAL),1)
CFLAGS += -DPSPEXP_CAPTURE_LOCAL -DPSPEXP_CAPTURE_PARTS='"SBUXF"'
endif

# make CRASH=1: an unhandled crash shown on the PSP's screen, the PSP held there (main.c, crash_handler)
ifeq ($(CRASH),1)
CFLAGS += -DPSPEXP_CAPTURE_CRASH
LIBS := -lpspdebug $(LIBS)
endif

# make TRACE=1: a game from the board's disc traced step by step over the cable (main.c, TRACE)
ifeq ($(TRACE),1)
CFLAGS += -DPSPEXP_CAPTURE_TRACE
endif

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

-include $(OBJS:.o=.d)
