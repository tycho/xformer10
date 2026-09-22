cmake_minimum_required(VERSION 3.16)
project(xformer10 C)

# CMake 4.x compatibility for old vendored dependencies such as
# SDL2_ttf's bundled FreeType.
set(CMAKE_POLICY_VERSION_MINIMUM 3.16)

# C11: identical typedef redefinitions (ADDR, PFNREAD, ...) across headers are
# legal in C11, which silences a pile of -Wtypedef-redefinition warnings.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

#
# Common CFLAGS for Visual Studio builds
#
if(CMAKE_GENERATOR MATCHES "Visual Studio")
	add_compile_options(
		# Inline any suitable
		/Ob2
		# Enable intrinsic functions
		/Oi
		# Multi-process build
		/MP
	)
endif()

#
# Build all vendored libraries statically unless overridden explicitly.
#
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)

#
# SDL2
#
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.32.10
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(SDL2)

#
# SDL2_ttf
#
# Build/fetch FreeType etc. internally, rather than requiring them from
# the host system.
set(SDL2TTF_VENDORED ON  CACHE BOOL "" FORCE)
set(SDL2TTF_SAMPLES  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL2_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        release-2.24.0
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(SDL2_ttf)

#
# fontconfig is only needed by the Unix implementation of font_sdl.c.
#
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
endif()


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

target_include_directories(xformer10 PRIVATE
    src
    src/atari8.vm
    src/blocklib
    src/res
)

target_compile_definitions(xformer10 PRIVATE
    SDL2_ENABLED
)

#
# Platform-specific definitions/libraries.
#
if(WIN32)
    # Needed by the Windows SDLUIFontPath() implementation if using
    # SHGetKnownFolderPath(FOLDERID_Fonts).
    target_link_libraries(xformer10 PRIVATE
        shell32
        ole32
    )
else()
    target_compile_definitions(xformer10 PRIVATE
        LINUX_PORT
        _GNU_SOURCE
    )

    target_link_libraries(xformer10 PRIVATE
        m
        pthread
    )
endif()

#
# These are GCC/Clang compatibility flags for the old codebase.
#
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xformer10 PRIVATE
        -Wno-incompatible-pointer-types
        -Wno-unused-value
        -Wno-implicit-function-declaration
        -Wno-duplicate-decl-specifier
        -fno-strict-aliasing
        -fwrapv
        -fcommon
    )
endif()

#
# Portable dependencies.
#
target_link_libraries(xformer10 PRIVATE
    SDL2::SDL2-static
    SDL2::SDL2main
    SDL2_ttf::SDL2_ttf-static
)

if(TARGET PkgConfig::FONTCONFIG)
    target_link_libraries(xformer10 PRIVATE
        PkgConfig::FONTCONFIG
    )
endif()
