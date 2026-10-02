# Writes OUT: #define PSPEXP_VERSION "2026.10.02 (244e4bf)", the source's
# version: its last commit's date and short hash (", modified" when tracked
# files differ from it), from git; in a copy of the source without git (the
# Pi's: scripts/pi/install.sh), the .pspexp-version file it carries; else
# "unknown". Run at every build (cmake -DSRC=... -DOUT=... -P this); OUT is
# rewritten only when the version changes, so nothing rebuilds otherwise.
set(v "")
execute_process(COMMAND git -C "${SRC}" log -1 --format=%cd --date=format:%Y.%m.%d
                OUTPUT_VARIABLE d OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE r ERROR_QUIET)
if(r EQUAL 0 AND d)
  execute_process(COMMAND git -C "${SRC}" rev-parse --short HEAD OUTPUT_VARIABLE h OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no OUTPUT_VARIABLE dirty ERROR_QUIET)
  set(v "${d} (${h}")
  if(dirty)
    string(APPEND v ", modified")
  endif()
  string(APPEND v ")")
elseif(EXISTS "${SRC}/.pspexp-version")
  file(READ "${SRC}/.pspexp-version" v)
  string(STRIP "${v}" v)
endif()
if(NOT v)
  set(v "unknown")
endif()
set(content "#define PSPEXP_VERSION \"${v}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
  file(WRITE "${OUT}" "${content}")
endif()
