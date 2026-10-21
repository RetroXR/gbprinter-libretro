cmake_minimum_required(VERSION 3.16)
project(gbprinter_libretro C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(GBPRINTER_TESTS "Build the Godot-free printer test" ON)

set(GBPRINTER_SOURCES src/libretro.c src/printer.c src/sound.c)

add_library(gbprinter_libretro SHARED ${GBPRINTER_SOURCES})
target_include_directories(gbprinter_libretro PRIVATE include src)

# The file name is the contract with every frontend: no "lib" prefix anywhere,
# and Android's suffix added by the release workflow when it is packaged.
set_target_properties(gbprinter_libretro PROPERTIES PREFIX "")

if(NOT MSVC)
  target_link_libraries(gbprinter_libretro PRIVATE m)
endif()

if(MSVC)
  target_compile_options(gbprinter_libretro PRIVATE /W4 /wd4100)
  # Static CRT: a core is copied into a temp dir and loaded from there, and must
  # not depend on a runtime the player may not have.
  set_property(TARGET gbprinter_libretro PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(gbprinter_libretro PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)
  if(APPLE)
    target_link_options(gbprinter_libretro PRIVATE -Wl,-exported_symbol,_retro_*)
  elseif(WIN32)
    # mingw: export only retro_*, and carry libgcc in the DLL.
    target_link_options(gbprinter_libretro PRIVATE -static-libgcc
      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T)
  else()
    target_link_options(gbprinter_libretro PRIVATE
      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T -Wl,--no-undefined)
  endif()
endif()

if(GBPRINTER_TESTS AND NOT ANDROID AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  # The test #includes the sources, so it links nothing of the library's.
  add_executable(printer_test tests/printer_test.c)
  target_include_directories(printer_test PRIVATE include src)
  if(MSVC)
    target_compile_definitions(printer_test PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_link_libraries(printer_test PRIVATE m)
  endif()
  add_test(NAME printer_test COMMAND printer_test)
  # Not a test: renders a print's sound to a WAV for listening (render_wav out.wav).
  add_executable(render_wav tests/render_wav.c)
  target_include_directories(render_wav PRIVATE include src)
  if(MSVC)
    target_compile_definitions(render_wav PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_link_libraries(render_wav PRIVATE m)
  endif()
endif()
