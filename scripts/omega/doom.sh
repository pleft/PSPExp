#!/bin/sh
# Chocolate Doom on the PSP from the Omega2+ (sdl-pspexp built in).
# Controls: nub / d-pad move and turn, cross fire, circle use, square run,
# L / R strafe, triangle Enter, start Escape (menu), select map.
# The mixer runs at 11025 Hz, the sound effects' own rate: Doom's converters
# use floating point, which this FPU-less CPU emulates slowly; the SDL layer
# upsamples to the PSP's 44.1 kHz with integer maths. Music is off by default:
# the OPL (FM synth) emulation alone costs a third of this CPU and breaks
# the sound up; DOOM_MUSIC=1 turns it on.
cd "$(dirname "$0")"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export PSPEXP="${PSPEXP:-usb}"
export PSPEXP_KEYS="${PSPEXP_KEYS:-cross=lctrl,circle=space,square=lshift,triangle=return,l=comma,r=period,start=escape,select=tab,up=up,down=down,left=left,right=right}"
export PSPEXP_NUB="${PSPEXP_NUB:-keys}"
CFG=./doom-omega.cfg
[ -f "$CFG" ] || printf 'snd_samplerate 11025\nuse_libsamplerate 0\nsnd_pitchshift 0\n' > "$CFG"
MUSIC=-nomusic
[ "${DOOM_MUSIC:-0}" = 1 ] && MUSIC=
# from the PSP console, the console hands the PSP over for the game
EXEC=; [ -x ./pspexp-exec ] && EXEC=./pspexp-exec
exec $EXEC ./chocolate-doom $MUSIC -iwad "${DOOMWAD:-/mnt/mmcblk0p1/doom/freedoom1.wad}" -extraconfig "$CFG" -window -nomouse -nograbmouse "$@"
