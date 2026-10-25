# Files under their own licences

These keep the licence of the project they come from or change; PSPExp's MIT licence (LICENSE) does not apply to them:

- compatibility/etlegacy/pspexp.patch: changes to ET: Legacy, GPL-3.0.
- compatibility/war1gus/war1tool-optional-cutscenes.patch: a fix to War1gus's war1tool, GPL-2.0.
- compatibility/wargus/wargus-small-screen.patch: Wargus's scripts for the PSP's screen (its menus and the game laid out for 480x272), GPL-2.0.
- scripts/gameshell/base/uboot-cpi3.patch: changes to U-Boot, GPL-2.0.
- scripts/gameshell/base/brcmfmac43430a0-sdio.clockwork,clockworkpi-cpi3.txt: the
  GameShell's Wi-Fi settings (Ampak's NVRAM file), GPL-2.0.
- scripts/gameshell/base/sun8i-r16-clockworkpi-cpi3.dts: GPL-2.0+ or MIT.
- tools/pspexp-retro/libretro.h: the libretro API, MIT (notice in the file).
- tools/pspexp-term/stb/: stb_image and stb_truetype, public domain or MIT
  (notice in the files).
- tools/pspexp-term/assets/: the Inter, Lucide and Spleen fonts, each under
  the licence in the -LICENSE.txt file beside it; assets/systems/, the
  systems' icons, CC BY 4.0 (their LICENSE.txt).
- recipes/: each package's games, data and cover art belong to their
  authors; a recipe says where they come from and under which terms. The
  games themselves are not in this repository.

# Code from other projects in PSPExp's own files and builds

The rest of these files is PSPExp's, under its MIT licence; the parts and
libraries below keep theirs, whose texts are in licenses/ and go with every
copy of the programs (the PSP's zip, the pspexp-runtime package):

- psp/transport-usb/main.c, PSPExp's USB driver on the PSP (pspexp_usb.prx),
  also built into the PSPExp plugin (psp/plugin, pspexp_plugin.prx):
  its USB descriptors and the driver's start and table come from PSPLINK's
  usbhostfs, Copyright (c) 2006 James F, BSD (licenses/PSPLINK.txt).
- Every PSP program (EBOOT.PBP, pspexp_runtime.prx, pspexp_usb.prx,
  pspexp_plugin.prx) is built with PSPSDK and links its libraries, BSD
  (licenses/PSPSDK.txt).
- EBOOT.PBP and pspexp_runtime.prx link newlib's C library (pspdev's port),
  under the notices in licenses/newlib-COPYING.NEWLIB.txt. The kernel modules
  (pspexp_usb.prx, pspexp_plugin.prx) use the PSP's own instead.
