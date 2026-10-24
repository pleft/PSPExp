# pspexp_runtime.prx: user-mode runtime over the PSPExpUSB driver, GE executor.
TARGET = pspexp_runtime
OBJS = main.o pspexp_audio_psp.o pspexp_fs_psp.o pspexp_capture_psp.o ge_pspexp_ge.o tcp_pspexp_tcp_psp.o \
       rt_pspexp_runtime.o rt_pspexp_cache.o rt_pspexp_mixer.o rt_pspexp_bmp.o \
       pr_pspexp_batch.o pr_pspexp_texture.o pr_pspexp_names.o pr_pspexp_mp3.o \
       ser_pspexp_writer.o ser_pspexp_reader.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -MMD -MP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
LIBDIR = ../transport-usb
LDFLAGS =
LIBS = -lpspexpusb -lpsprtc -lpsppower -lpspgu -lpspge -lpspaudio -lpspaudiocodec -lpspnet_inet -lpsputility

BUILD_PRX = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

tcp_%.o: ../transport-tcp/%.c
	$(CC) $(CFLAGS) -c $< -o $@
ge_%.o: ../executor-ge/%.c
	$(CC) $(CFLAGS) -c $< -o $@
rt_%.o: ../../runtime/%.c
	$(CC) $(CFLAGS) -c $< -o $@
pr_%.o: ../../protocol/%.c
	$(CC) $(CFLAGS) -c $< -o $@
ser_%.o: ../../protocol/serialization/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# header dependencies (the SDK makefiles track none: a changed header must rebuild its users)
-include $(OBJS:.o=.d)
