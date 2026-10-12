# PSPExp host build: protocol, runtime core, libPSPExp, executors, transports,
# tools and tests. The PSP side is built with the PSPSDK Makefiles under psp/
# (see scripts/build.sh).
cmake_minimum_required(VERSION 3.16)
project(PSPExp C)
# the static libraries go into shared ones (the SDL layer): position-independent everywhere
# (x86-64 refused to link them otherwise; ARM64 had let it pass)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
  protocol/pspexp_mp3.c
  protocol/serialization/pspexp_writer.c
  protocol/serialization/pspexp_reader.c)
target_include_directories(pspexp_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- runtime core: shared with the PSP build ----------------------------
add_library(pspexp_runtime STATIC
  runtime/pspexp_cache.c
  runtime/pspexp_mixer.c
  runtime/pspexp_bmp.c
  runtime/pspexp_runtime.c
  runtime/pspexp_fs_posix.c)
target_link_libraries(pspexp_runtime PUBLIC pspexp_protocol)

# --- host library --------------------------------------------------------
add_library(pspexp STATIC
  host/libpspexp/pspexp.c
  host/libpspexp/pspexp_font.c
  host/libpspexp/pspexp_time.c
  host/libpspexp/pspexp_local_audio.c
  host/libpspexp/pspexp_slots.c
  protocol/pspexp_sha256.c)
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
  add_library(sdl-pspexp SHARED compatibility/sdl-pspexp/sdl_pspexp.c compatibility/sdl-pspexp/gl_pspexp.c compatibility/sdl-pspexp/glsl_pspexp.c)
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
  else()
    # gl_pspexp.c interposes the framework's OpenGL functions
    target_link_libraries(sdl-pspexp PRIVATE "-framework OpenGL")
  endif()
  # the same layer without its OpenGL: the program's GL is the host's GPU, the frames go to the PSP as
  # pictures (PSPEXP_HOST_GL; for programs too heavy for the GE, OpenMW)
  add_library(sdl-pspexp-hostgl SHARED compatibility/sdl-pspexp/sdl_pspexp.c)
  target_compile_definitions(sdl-pspexp-hostgl PRIVATE PSPEXP_HOST_GL=1)
  target_link_libraries(sdl-pspexp-hostgl PRIVATE pspexp pspexp_null pspexp_sdl pspexp_tcp ${SDL2_LINK_LIBRARIES})
  target_include_directories(sdl-pspexp-hostgl PRIVATE ${SDL2_INCLUDE_DIRS})
  target_compile_options(sdl-pspexp-hostgl PRIVATE ${SDL2_CFLAGS_OTHER})
  set_target_properties(sdl-pspexp-hostgl PROPERTIES OUTPUT_NAME "sdl-pspexp-hostgl" PREFIX "lib")
  if(PSPEXP_HAVE_USB)
    target_link_libraries(sdl-pspexp-hostgl PRIVATE pspexp_usb)
    target_compile_definitions(sdl-pspexp-hostgl PRIVATE PSPEXP_HAVE_USB=1)
  endif()
  if(NOT APPLE)
    target_link_libraries(sdl-pspexp-hostgl PRIVATE ${CMAKE_DL_LIBS} m)
  endif()

  add_executable(sdl-game examples/sdl-game/main.c)
  target_link_libraries(sdl-game PRIVATE ${SDL2_LINK_LIBRARIES})
  if(NOT APPLE)
    target_link_libraries(sdl-game PRIVATE m)
  endif()
  target_include_directories(sdl-game PRIVATE ${SDL2_INCLUDE_DIRS})
  target_compile_options(sdl-game PRIVATE ${SDL2_CFLAGS_OTHER})

  # the GLSL reader on its own (tests/glsl_plan.c)
  add_executable(glsl_plan tests/glsl_plan.c compatibility/sdl-pspexp/glsl_pspexp.c)
  if(NOT APPLE)
    target_link_libraries(glsl_plan PRIVATE m)
  endif()

  # an SDL2 + OpenGL 1.x program, for the SDL layer's OpenGL part
  find_package(OpenGL QUIET)
  if(OPENGL_FOUND)
    add_executable(gl-demo examples/gl-demo/main.c)
    target_link_libraries(gl-demo PRIVATE ${SDL2_LINK_LIBRARIES} OpenGL::GL)
    if(NOT APPLE)
      target_link_libraries(gl-demo PRIVATE m)
    endif()
    target_include_directories(gl-demo PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_options(gl-demo PRIVATE ${SDL2_CFLAGS_OTHER} -DGL_SILENCE_DEPRECATION)
  endif()

  # libretro player: a plain SDL2 program (the SDL layer puts it on the PSP)
  add_executable(pspexp-retro tools/pspexp-retro/main.c)
  target_link_libraries(pspexp-retro PRIVATE ${SDL2_LINK_LIBRARIES} ${CMAKE_DL_LIBS})
  target_include_directories(pspexp-retro PRIVATE ${SDL2_INCLUDE_DIRS})
  target_compile_options(pspexp-retro PRIVATE ${SDL2_CFLAGS_OTHER})
endif()

# --- tools and examples -----------------------------------------------------
add_executable(pspexp-ms tools/pspexp-ms/main.c)
target_link_libraries(pspexp-ms PRIVATE pspexp pspexp_tcp)
if(PSPEXP_HAVE_USB)
  target_link_libraries(pspexp-ms PRIVATE pspexp_usb)
  target_compile_definitions(pspexp-ms PRIVATE PSPEXP_HAVE_USB=1)
endif()
add_executable(pspexp-bench tools/usb-bench/main.c)
target_link_libraries(pspexp-bench PRIVATE pspexp pspexp_null pspexp_tcp)
if(PSPEXP_HAVE_USB)
  target_link_libraries(pspexp-bench PRIVATE pspexp_usb)
  target_compile_definitions(pspexp-bench PRIVATE PSPEXP_HAVE_USB=1)
endif()

add_executable(pspexp-term tools/pspexp-term/main.c tools/pspexp-term/term_menu.c tools/pspexp-term/launcher_ui.c tools/pspexp-term/files.c tools/pspexp-term/player.c tools/pspexp-term/music.c tools/pspexp-term/upnp.c tools/pspexp-term/notify.c tools/pspexp-term/btaudio.c tools/pspexp-term/files_ops.c tools/pspexp-term/files_psp.c tools/pspexp-term/control.c tools/pspexp-term/apps.c tools/pspexp-term/monitor.c tools/pspexp-term/sysinfo.c tools/pspexp-term/store.c tools/pspexp-term/term_fonts.c tools/pspexp-term/term_themes.c tools/pspexp-term/launcher.c tools/pspexp-term/launcher_board.c
  tools/pspexp-term/launcher_lib.c tools/pspexp-term/launcher_gfx.c)
target_compile_definitions(pspexp-term PRIVATE PSPEXP_LAUNCHER_ASSET_DIR="${CMAKE_SOURCE_DIR}/tools/pspexp-term/assets")
# the console's version, for About: generated at every build (cmake/pspexp_version.cmake)
add_custom_target(pspexp_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/pspexp_version.h -P ${CMAKE_SOURCE_DIR}/cmake/pspexp_version.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/pspexp_version.h)
add_dependencies(pspexp-term pspexp_version)
target_include_directories(pspexp-term PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(pspexp-term PRIVATE pspexp pspexp_null pspexp_tcp)
if(NOT APPLE)
  target_link_libraries(pspexp-term PRIVATE util m pthread)
else()
  target_sources(pspexp-term PRIVATE tools/pspexp-term/launcher_mac.c)
  target_link_libraries(pspexp-term PRIVATE "-framework IOKit" "-framework CoreFoundation")
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
  add_executable(test_launcher_board tests/test_launcher_board.c tools/pspexp-term/launcher_board.c)
  add_test(NAME launcher_board COMMAND test_launcher_board)
  add_executable(test_launcher_gfx tests/test_launcher_gfx.c tools/pspexp-term/launcher_gfx.c)
  target_link_libraries(test_launcher_gfx PRIVATE pspexp pspexp_null m)
  target_compile_definitions(test_launcher_gfx PRIVATE PSPEXP_LAUNCHER_ASSET_DIR="${CMAKE_SOURCE_DIR}/tools/pspexp-term/assets")
  add_test(NAME launcher_gfx COMMAND test_launcher_gfx)
  add_executable(test_term_menu tests/test_term_menu.c tools/pspexp-term/term_menu.c)
  add_test(NAME term_menu COMMAND test_term_menu)
  add_executable(test_upnp tests/test_upnp.c tools/pspexp-term/upnp.c)
  add_test(NAME upnp COMMAND test_upnp)
  add_executable(test_files_ops tests/test_files_ops.c tools/pspexp-term/files_ops.c tools/pspexp-term/files_psp.c)
  target_link_libraries(test_files_ops PRIVATE pspexp pspexp_null pthread)
  add_test(NAME files_ops COMMAND test_files_ops)
  add_executable(test_runtime tests/test_runtime.c)
  target_link_libraries(test_runtime PRIVATE pspexp pspexp_null)
  add_test(NAME runtime COMMAND test_runtime)
  add_executable(test_fs tests/test_fs.c)
  target_link_libraries(test_fs PRIVATE pspexp pspexp_null)
  add_test(NAME fs COMMAND test_fs)
  add_test(NAME recipes COMMAND ${CMAKE_SOURCE_DIR}/scripts/packages/check-recipes ${CMAKE_SOURCE_DIR}/recipes)
  add_test(NAME bench_loopback COMMAND pspexp-bench --null --mb 1 --echo 20 --frames 30 --sprites 200)
  add_test(NAME sprite_demo_null COMMAND pspexp-sprite-demo --null --frames 30 --field 2000)
  if(SDL2_FOUND)
    add_test(NAME sdl_pspexp COMMAND ${CMAKE_SOURCE_DIR}/tests/run_sdl_pspexp.sh ${CMAKE_BINARY_DIR})
    add_test(NAME glsl_plan COMMAND ${CMAKE_SOURCE_DIR}/tests/run_glsl_plan.sh ${CMAKE_BINARY_DIR})
  endif()
endif()
