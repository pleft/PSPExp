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
  # a release (a tag vX.Y.Z on this very commit, and nothing changed): its name in front, as
  # "1.0.0, 2026.10.11 (7ba9a2f)"; the packages keep their date versions, which updates compare
  execute_process(COMMAND git -C "${SRC}" describe --tags --exact-match --match "v[0-9]*" HEAD
                  OUTPUT_VARIABLE tag OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE tr ERROR_QUIET)
  if(tr EQUAL 0 AND tag AND NOT dirty)
    string(REGEX REPLACE "^v" "" tag "${tag}")
    set(v "${tag}, ${v}")
  endif()
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
