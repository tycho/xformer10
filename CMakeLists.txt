cmake_minimum_required(VERSION 3.10)
project(xformer10 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)
find_package(SDL2_ttf REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

add_executable(xformer10
    src/atari8.vm/atari800.c
    src/atari8.vm/atarikey.c
    src/atari8.vm/atariosb.c
    src/atari8.vm/atarixl5.c
    src/atari8.vm/atarixlb.c
    src/atari8.vm/atarixlc.c
    src/atari8.vm/atarixlo.c
    src/atari8.vm/x6502.c
    src/atari8.vm/xkey.c
    src/atari8.vm/xlxeram.c
    src/atari8.vm/xmon.c
    src/atari8.vm/xsio.c
    src/atari8.vm/xvideo.c
    src/blocklib/blockapi.c
    src/blocklib/sectorio.c
    src/blocklib/mac_hfs.c
    src/gemul8r.c
    src/ddlib.c
    src/ddlib_sdl.c
    src/sound.c
    src/props.c
    src/printer.c
    src/serial.c
    src/romcard.c
    src/keymap_sdl.c
    src/main_linux.c
    src/stubs_linux.c
    src/menu_sdl.c
    src/sdl_filebrowser.c
    src/font_sdl.c
)

target_compile_definitions(xformer10 PRIVATE
    LINUX_PORT
    _GNU_SOURCE
    SDL2_ENABLED
)

target_include_directories(xformer10 PRIVATE
    src
    src/atari8.vm
    src/blocklib
    src/res
)

target_compile_options(xformer10 PRIVATE
    -Wno-incompatible-pointer-types
    -Wno-unused-value
    -Wno-implicit-function-declaration
    -Wno-duplicate-decl-specifier
    -fno-strict-aliasing
    -fwrapv
    -fcommon
)

if(TARGET SDL2::SDL2)
    target_link_libraries(xformer10 PRIVATE m pthread SDL2::SDL2 SDL2_ttf::SDL2_ttf PkgConfig::FONTCONFIG)
else()
    target_include_directories(xformer10 PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(xformer10 PRIVATE m pthread ${SDL2_LIBRARIES} SDL2_ttf::SDL2_ttf PkgConfig::FONTCONFIG)
endif()
