cmake_minimum_required(VERSION 3.16)

#
# macOS: pin the minimum OS version before project() so that every vendored,
# statically linked dependency is built against the same target. The .app
# depends only on system frameworks, so this alone decides where it runs.
# Override with -DCMAKE_OSX_DEPLOYMENT_TARGET=... at configure time.
#
if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
        "Minimum macOS version to target")
endif()

project(xformer10 C)

set(XFORMER_VERSION "10.10.2021")

option(XFORMER_MACOS_BUNDLE
       "macOS: build Xformer10.app instead of a bare executable" ON)
set(XFORMER_CODESIGN_IDENTITY "-" CACHE STRING
    "macOS: codesign identity for the .app ('-' = ad-hoc, empty = don't sign)")

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

if(WIN32 AND MSVC)
	add_link_options(
		/SUBSYSTEM:WINDOWS
	)
endif()

#
# Use LTO for non-Debug builds
#
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	add_compile_options(
		$<$<NOT:$<CONFIG:Debug>>:-flto=thin>
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
elseif(APPLE)
    target_compile_definitions(xformer10 PRIVATE
        LINUX_PORT
    )

    # CoreText resolves the UI font in font_sdl.c. Everything else the app
    # needs (Cocoa, CoreAudio, Metal, ...) is carried by SDL2::SDL2-static.
    target_link_libraries(xformer10 PRIVATE
        "-framework CoreText"
        "-framework CoreFoundation"
    )

    if(XFORMER_MACOS_BUNDLE)
        set(XFORMER_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/src/res/xformer.icns")
        target_sources(xformer10 PRIVATE "${XFORMER_ICNS}")
        set_source_files_properties("${XFORMER_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources
        )

        set_target_properties(xformer10 PROPERTIES
            OUTPUT_NAME                        "Xformer10"
            MACOSX_BUNDLE                      TRUE
            MACOSX_BUNDLE_INFO_PLIST           "${CMAKE_CURRENT_SOURCE_DIR}/src/res/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME          "Xformer 10"
            MACOSX_BUNDLE_GUI_IDENTIFIER       "com.emulators.xformer10"
            MACOSX_BUNDLE_ICON_FILE            "xformer.icns"
            MACOSX_BUNDLE_BUNDLE_VERSION       "${XFORMER_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${XFORMER_VERSION}"
            MACOSX_BUNDLE_COPYRIGHT            "Copyright (C) 1986-2021 Darek Mihocka and Danny Miller"
        )

        # Seal the bundle so Info.plist and the icon are covered by the
        # signature. Ad-hoc by default; set XFORMER_CODESIGN_IDENTITY to a
        # Developer ID to sign for distribution, or to "" to skip.
        find_program(XFORMER_CODESIGN codesign)
        if(XFORMER_CODESIGN AND NOT XFORMER_CODESIGN_IDENTITY STREQUAL "")
            add_custom_command(TARGET xformer10 POST_BUILD
                COMMAND "${XFORMER_CODESIGN}" --force
                        --sign "${XFORMER_CODESIGN_IDENTITY}"
                        "$<TARGET_BUNDLE_DIR:xformer10>"
                COMMENT "Signing $<TARGET_BUNDLE_DIR:xformer10>"
                VERBATIM
            )
        endif()
    endif()
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
