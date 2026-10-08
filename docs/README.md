# PSPExp documents

## For players

- [User manual](manual/README.md): preparing the PSP, setting up each board,
  and everything the launcher does. Start here.
- [Cheatsheet](cheatsheet.md): commands to type on the board, for those who
  use its shell.

## For developers

- [Developing PSPExp](Developing.md): building, trying it from a Mac (with or
  without a PSP), the demos, and where the code lives. Start here.
- [Technical architecture](PSPExp_Technical_Architecture.md): the design: the
  protocol, graphics, sound, input, pacing, OpenGL, measurements, and what
  is next.
- [The SDL layer](../compatibility/sdl-pspexp/README.md): how unmodified SDL2
  and OpenGL games reach the PSP, and every `PSPEXP_*` setting it reads.
- [The launcher and boards](Launcher_and_Boards.md): the launcher, the
  emulators, the terminal and file manager, the apps, Bluetooth sound, the
  GameShell's own system image, and the tests on a real PSP.
- [Packages](Package_Manager.md): recipes, the apt repository, and how games
  are packaged for each board.
- [Scripts](../scripts/README.md): what each script does and where it runs.
- [v0.1 USB bring-up](v01-usb-bringup.md): the first measurements of the USB link.

The technical documents are also a record of the work: they say what was
tried, what was measured and when, so some sections describe the state at
a given date.
