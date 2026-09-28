# pspexp_usb.prx (kernel USB driver) and libpspexpusb.a (user-mode stubs)
TARGET = pspexp_usb
OBJS = main.o exports.o

USE_KERNEL_LIBS = 1
INCDIR =
CFLAGS = -Os -mno-gpopt -Wall -fno-builtin-printf -MMD -MP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LIBDIR =
LIBS = -lpspusbbus_driver -lpspusb_driver -lpspctrl_driver -lpsprtc_driver -lpspreg_driver

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

# Stub library for user modules. psp-build-exports -s writes PSPExpUSB.S with
# one #ifdef F_PSPExpUSB_NNNN block per exported function (0000: the library
# header, then one per PSP_EXPORT_FUNC of PSPExpUSB in exports.exp).
STUB_LIB = libpspexpusb.a
STUB_OBJS = $(foreach i,0000 0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012,PSPExpUSB_$(i).o)

all: $(STUB_LIB)

PSPExpUSB.S: exports.exp
	psp-build-exports -s $<

$(STUB_OBJS): PSPExpUSB.S
	$(CC) $(CFLAGS) -DF_$* $< -c -o $@

$(STUB_LIB): $(STUB_OBJS)
	rm -f $@
	psp-ar q $@ $(STUB_OBJS)
	psp-ranlib $@

EXTRA_CLEAN = clean_stubs
clean_stubs:
	rm -f $(STUB_OBJS) $(STUB_LIB) PSPExpUSB.S

# header dependencies (the SDK makefiles track none: a changed header must rebuild its users)
-include $(OBJS:.o=.d)
