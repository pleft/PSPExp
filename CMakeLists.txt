# PSPExp host build: protocol, runtime core, libPSPExp, executors, transports,
# tools and tests. The PSP side is built with the PSPSDK Makefiles under psp/
# (see scripts/build.sh).
cmake_minimum_required(VERSION 3.16)
project(PSPExp C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
if(APPLE OR UNIX)
  add_compile_definitions(_DARWIN_C_SOURCE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
endif()

option(PSPEXP_BUILD_USB "Build the libusb host transport" ON)
option(PSPEXP_BUILD_TESTS "Build unit tests" ON)

# --- protocol: plain C99, shared with the PSP build ---------------------
add_library(pspexp_protocol STATIC
  protocol/pspexp_batch.c
  protocol/pspexp_texture.c
  protocol/pspexp_names.c
  protocol/serialization/pspexp_writer.c
  protocol/serialization/pspexp_reader.c)
target_include_directories(pspexp_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- runtime core: shared with the PSP build ----------------------------
add_library(pspexp_runtime STATIC
  runtime/pspexp_cache.c
  runtime/pspexp_mixer.c
  runtime/pspexp_bmp.c
  runtime/pspexp_runtime.c)
target_link_libraries(pspexp_runtime PUBLIC pspexp_protocol)

# --- host library --------------------------------------------------------
add_library(pspexp STATIC
  host/libpspexp/pspexp.c
  host/libpspexp/pspexp_font.c
  host/libpspexp/pspexp_time.c)
target_link_libraries(pspexp PUBLIC pspexp_protocol)

add_library(pspexp_null STATIC host/executor-null/pspexp_null.c)
target_link_libraries(pspexp_null PUBLIC pspexp pspexp_runtime)

set(PSPEXP_HAVE_USB OFF)
if(PSPEXP_BUILD_USB)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB libusb-1.0)
  endif()
  if(LIBUSB_FOUND)
    add_library(pspexp_usb STATIC host/transport-usb/pspexp_usb.c)
    target_link_libraries(pspexp_usb PUBLIC pspexp ${LIBUSB_LINK_LIBRARIES})
    target_include_directories(pspexp_usb PUBLIC ${LIBUSB_INCLUDE_DIRS})
    target_compile_options(pspexp_usb PUBLIC ${LIBUSB_CFLAGS_OTHER})
    find_package(Threads REQUIRED)
    target_link_libraries(pspexp_usb PUBLIC Threads::Threads)
    set(PSPEXP_HAVE_USB ON)
  else()
    message(STATUS "libusb-1.0 not found: USB transport disabled")
  endif()
endif()

# --- TCP transport (Wi-Fi to the PSP, or the PSP runtime under PPSSPP) -------
add_library(pspexp_tcp STATIC host/transport-tcp/pspexp_tcp.c)
target_link_libraries(pspexp_tcp PUBLIC pspexp)

# --- SDL executor -----------------------------------------------------------
option(PSPEXP_BUILD_SDL "Build the SDL executor" ON)
set(PSPEXP_HAVE_SDL OFF)
if(PSPEXP_BUILD_SDL)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 sdl2)
  endif()
  if(SDL2_FOUND)
    add_library(pspexp_sdl STATIC host/executor-sdl/pspexp_sdl.c)
    target_link_libraries(pspexp_sdl PUBLIC pspexp pspexp_runtime ${SDL2_LINK_LIBRARIES})
    target_include_directories(pspexp_sdl PUBLIC ${SDL2_INCLUDE_DIRS})
    target_compile_options(pspexp_sdl PUBLIC ${SDL2_CFLAGS_OTHER})
    set(PSPEXP_HAVE_SDL ON)
  else()
    message(STATUS "SDL2 not found: SDL executor disabled")
  endif()
endif()

# --- sdl-pspexp: interposition library for unmodified SDL2 programs (v0.4) ---
if(SDL2_FOUND)
  add_library(sdl-pspexp SHARED compatibility/sdl-pspexp/sdl_pspexp.c)
  target_link_libraries(sdl-pspexp PRIVATE pspexp pspexp_null pspexp_sdl pspexp_tcp ${SDL2_LINK_LIBRARIES})
  target_include_directories(sdl-pspexp PRIVATE ${SDL2_INCLUDE_DIRS})
  target_compile_options(sdl-pspexp PRIVATE ${SDL2_CFLAGS_OTHER})
  set_target_properties(sdl-pspexp PROPERTIES OUTPUT_NAME "sdl-pspexp" PREFIX "lib")
  if(PSPEXP_HAVE_USB)
    target_link_libraries(sdl-pspexp PRIVATE pspexp_usb)
    target_compile_definitions(sdl-pspexp PRIVATE PSPEXP_HAVE_USB=1)
  endif()
  if(NOT APPLE)
    target_link_libraries(sdl-pspexp PRIVATE ${CMAKE_DL_LIBS} m)
  endif()

  add_executable(sdl-game examples/sdl-game/main.c)
  target_link_libraries(sdl-game PRIVATE ${SDL2_LINK_LIBRARIES})
  if(NOT APPLE)
    target_link_libraries(sdl-game PRIVATE m)
  endif()
  target_include_directories(sdl-game PRIVATE ${SDL2_INCLUDE_DIRS})
  target_compile_options(sdl-game PRIVATE ${SDL2_CFLAGS_OTHER})
endif()

# --- tools and examples -----------------------------------------------------
add_executable(pspexp-bench tools/usb-bench/main.c)
target_link_libraries(pspexp-bench PRIVATE pspexp pspexp_null pspexp_tcp)
if(PSPEXP_HAVE_USB)
  target_link_libraries(pspexp-bench PRIVATE pspexp_usb)
  target_compile_definitions(pspexp-bench PRIVATE PSPEXP_HAVE_USB=1)
endif()

add_executable(pspexp-term tools/pspexp-term/main.c tools/pspexp-term/launcher.c
  tools/pspexp-term/launcher_lib.c tools/pspexp-term/launcher_gfx.c)
target_compile_definitions(pspexp-term PRIVATE PSPEXP_LAUNCHER_ASSET_DIR="${CMAKE_SOURCE_DIR}/tools/pspexp-term/assets")
target_link_libraries(pspexp-term PRIVATE pspexp pspexp_null pspexp_tcp)
if(NOT APPLE)
  target_link_libraries(pspexp-term PRIVATE util m)
endif()
if(PSPEXP_HAVE_USB)
  target_link_libraries(pspexp-term PRIVATE pspexp_usb)
  target_compile_definitions(pspexp-term PRIVATE PSPEXP_HAVE_USB=1)
endif()
if(PSPEXP_HAVE_SDL)
  target_link_libraries(pspexp-term PRIVATE pspexp_sdl)
  target_compile_definitions(pspexp-term PRIVATE PSPEXP_HAVE_SDL=1)
endif()

if(PSPEXP_HAVE_SDL)
  add_executable(pspexp-sdl-server tools/pspexp-sdl-server/main.c)
  target_link_libraries(pspexp-sdl-server PRIVATE pspexp_sdl pspexp)
endif()

add_executable(pspexp-sprite-demo examples/sprite-demo/main.c)
target_link_libraries(pspexp-sprite-demo PRIVATE pspexp pspexp_null pspexp_tcp)
if(PSPEXP_HAVE_USB)
  target_link_libraries(pspexp-sprite-demo PRIVATE pspexp_usb)
  target_compile_definitions(pspexp-sprite-demo PRIVATE PSPEXP_HAVE_USB=1)
endif()
if(PSPEXP_HAVE_SDL)
  target_link_libraries(pspexp-sprite-demo PRIVATE pspexp_sdl)
  target_compile_definitions(pspexp-sprite-demo PRIVATE PSPEXP_HAVE_SDL=1)
endif()

# --- tests ----------------------------------------------------------------
if(PSPEXP_BUILD_TESTS)
  enable_testing()
  add_executable(test_protocol tests/test_protocol.c)
  target_link_libraries(test_protocol PRIVATE pspexp_protocol)
  add_test(NAME protocol COMMAND test_protocol)
  add_executable(test_launcher_lib tests/test_launcher_lib.c tools/pspexp-term/launcher_lib.c)
  add_test(NAME launcher_lib COMMAND test_launcher_lib)
  add_executable(test_runtime tests/test_runtime.c)
  target_link_libraries(test_runtime PRIVATE pspexp pspexp_null)
  add_test(NAME runtime COMMAND test_runtime)
  add_test(NAME bench_loopback COMMAND pspexp-bench --null --mb 1 --echo 20 --frames 30 --sprites 200)
  add_test(NAME sprite_demo_null COMMAND pspexp-sprite-demo --null --frames 30 --field 2000)
  if(SDL2_FOUND)
    add_test(NAME sdl_pspexp COMMAND ${CMAKE_SOURCE_DIR}/tests/run_sdl_pspexp.sh ${CMAKE_BINARY_DIR})
  endif()
endif()
