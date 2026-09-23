# pspexp_boot.prx packed as EBOOT.PBP. Kernel-style build (no crt0): the
# module supplies module_start itself, like PSPLink's bootstrap.
TARGET = pspexp_boot
OBJS = main.o

INCDIR =
CFLAGS = -O2 -mno-gpopt -Wall -MMD -MP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LIBDIR =
LDFLAGS =
LIBS =

BUILD_PRX = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

all: EBOOT.PBP

EBOOT.PBP: $(TARGET).prx
	mksfoex 'PSPExp' PARAM.SFO
	pack-pbp EBOOT.PBP PARAM.SFO NULL NULL NULL NULL NULL $(TARGET).prx NULL

EXTRA_CLEAN = clean_pbp
clean_pbp:
	rm -f PARAM.SFO EBOOT.PBP

# header dependencies (the SDK makefiles track none: a changed header must rebuild its users)
-include $(OBJS:.o=.d)
