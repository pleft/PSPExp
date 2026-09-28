# pspexp_boot.prx packed as EBOOT.PBP. Kernel-style build (no crt0): the
# module supplies module_start itself, like PSPLink's bootstrap.
TARGET = pspexp_boot
OBJS = main.o pspexp_sha256.o

INCDIR =
CFLAGS = -O2 -mno-gpopt -Wall -MMD -MP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LIBDIR =
LDFLAGS =
LIBS = -lpspctrl

BUILD_PRX = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

all: EBOOT.PBP

# ICON0.PNG (144x80, the XMB tile) and PIC1.PNG (480x272, the XMB background
# while PSPExp is highlighted) come from scripts/dev/make-eboot-art.py.
EBOOT.PBP: $(TARGET).prx ICON0.PNG PIC1.PNG
	mksfoex 'PSPExp' PARAM.SFO
	pack-pbp EBOOT.PBP PARAM.SFO ICON0.PNG NULL NULL PIC1.PNG NULL $(TARGET).prx NULL

pspexp_sha256.o: ../../protocol/pspexp_sha256.c
	$(CC) $(CFLAGS) -c $< -o $@

EXTRA_CLEAN = clean_pbp
clean_pbp:
	rm -f PARAM.SFO EBOOT.PBP

# header dependencies (the SDK makefiles track none: a changed header must rebuild its users)
-include $(OBJS:.o=.d)
