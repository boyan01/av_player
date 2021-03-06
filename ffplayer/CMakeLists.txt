﻿cmake_minimum_required(VERSION 3.10)

option(MEDIA_BUILD_EXAMPLE "Build example project" ON)
option(DISABLE_MEDIA_TEST "Disable test" OFF)

if (WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    #    set(BUILD_SHARED_LIBS TRUE)
    set(CMAKE_SYSTEM_VERSION "10")
    set(CMAKE_SYSTEM_NAME "WindowsStore")
endif ()

project("media" "CXX" "C")
set(CMAKE_CXX_STANDARD 14)

if (MEDIA_WINDOWS)
    set(MEDIA_USE_SDL 1)
    set(DISABLE_MEDIA_TEST 1)
    add_compile_definitions(_MEDIA_WINDOWS)
elseif (MEDIA_LINUX)
    set(DISABLE_MEDIA_TEST 1)
    set(MEDIA_USE_SDL 1)
    add_compile_definitions(_MEDIA_LINUX)
elseif (MEDIA_ANDROID)
    set(MEDIA_USE_SDL 0)
    set(DISABLE_MEDIA_TEST 1)
    add_compile_definitions(_MEDIA_ANDROID)
elseif (MEDIA_MACOS)
    set(MEDIA_USE_SDL 0)
    set(DISABLE_MEDIA_TEST 1)
    add_compile_definitions(_MEDIA_MACOS)
    #    add_compile_definitions(_MEDIA_ENABLE_HW_ACCEL)

    find_library(FFMPEG_KIT NAMES FFmpegKit HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../example/macos/Pods/ffmpeg-kit-macos-full REQUIRED)
    list(APPEND FFP_LIBS ${FFmpegKit})

elseif (MEDIA_IOS)
    set(MEDIA_USE_SDL 0)
    set(DISABLE_MEDIA_TEST 1)
    add_compile_definitions(_MEDIA_IOS)

    find_library(FFMPEG_KIT NAMES FFmpegKit HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../example/ios/Pods/ffmpeg-kit-ios-full REQUIRED)
    list(APPEND FFP_LIBS ${FFMPEG_KIT})
endif ()

if (NOT DEFINED FFP_LIBS)
    list(APPEND MEDIA_THIRD_PARTY_LIBS
            avutil
            avcodec
            avformat
            swscale
            swresample
            )
    if (MEDIA_USE_SDL)
        list(APPEND MEDIA_THIRD_PARTY_LIBS SDL2)
    endif ()
    if (WIN32)
        set(WINDOWS_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/windows")

        function(add_windows_lib NAME)
            add_library(${NAME} SHARED IMPORTED GLOBAL)
            SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_IMPLIB ${WINDOWS_LIB_PATH}/lib/${NAME}.lib)
            if (DEFINED ARGV1)
                SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_LOCATION ${WINDOWS_LIB_PATH}/bin/${NAME}-${ARGV1}.dll)
            else ()
                SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_LOCATION ${WINDOWS_LIB_PATH}/bin/${NAME}.dll)
            endif ()
        endfunction()

        add_windows_lib(avutil 56)
        add_windows_lib(avcodec 58)
        add_windows_lib(avformat 58)
        add_windows_lib(swscale 5)
        add_windows_lib(swresample 3)
        add_windows_lib(SDL2)

    endif ()

    if (APPLE)
        function(add_macos_lib NAME)
            if (CMAKE_FLUTTER_MEDIA_IOS)
                # add_library(${NAME} STATIC IMPORTED GLOBAL)
                # set_target_properties(${NAME} PROPERTIES
                #         IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/ios/lib/lib${NAME}.a)
            else ()
                add_library(${NAME} SHARED IMPORTED GLOBAL)
                set_target_properties(${NAME} PROPERTIES IMPORTED_LOCATION /usr/local/lib/lib${NAME}.dylib)
            endif ()
        endfunction()

        add_macos_lib(avutil)
        add_macos_lib(avcodec)
        add_macos_lib(avformat)
        add_macos_lib(swscale)
        add_macos_lib(swresample)
        add_macos_lib(SDL2)

    endif ()
    #    # TODO more imported libs.
    #    if (UNIX AND NOT APPLE)
    #        set(LINUX_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/linux")
    #        function(add_linux_lib NAME)
    #            find_library(lib_${NAME} ${NAME} REQUIRED)
    #            add_library(${NAME} SHARED IMPORTED GLOBAL)
    #            set_target_properties(${NAME} PROPERTIES IMPORTED_LOCATION ${lib_${NAME}})
    #        endfunction()
    #        add_linux_lib(avutil)
    #        add_linux_lib(avcodec)
    #        add_linux_lib(avformat)
    #        add_linux_lib(swscale)
    #        add_linux_lib(swresample)
    #        add_linux_lib(SDL2)
    #    endif ()

    list(APPEND FFP_LIBS ${MEDIA_THIRD_PARTY_LIBS})
    if (UNIX)
        # linux do not have math library.
        list(APPEND FFP_LIBS m pthread)
    endif ()
endif ()

if (NOT DISABLE_MEDIA_TEST)
    # https://google.github.io/googletest/quickstart-cmake.html#set-up-a-project
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest
            GIT_TAG 609281088cfefc76f9d0ce82e1ff6c30cc3591e5
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif ()


add_subdirectory(base)

add_subdirectory(player)

if (MEDIA_USE_SDL)
    add_subdirectory(sdl)
endif ()

if (APPLE)
    add_subdirectory(macos)
endif ()

add_subdirectory(flutter)

if (MEDIA_BUILD_EXAMPLE)
    add_subdirectory(example)
endif ()