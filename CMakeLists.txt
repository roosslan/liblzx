cmake_minimum_required(VERSION 3.16)

project(liblzx
        VERSION 1.0.0
        DESCRIPTION "LZX-сжатие и запись CAB-архивов"
        LANGUAGES C)

# Утилита и тесты по умолчанию собираются, только если проект собирается
# самостоятельно, а не подключён через add_subdirectory (например, субмодулем).
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
        set(LIBLZX_TOP_LEVEL ON)
else()
        set(LIBLZX_TOP_LEVEL OFF)
endif()

option(LIBLZX_BUILD_TOOLS "Собирать утилиту lzxcab" ${LIBLZX_TOP_LEVEL})
option(LIBLZX_BUILD_TESTS "Собирать тесты" ${LIBLZX_TOP_LEVEL})
option(LIBLZX_WARNINGS_AS_ERRORS "Считать предупреждения ошибками" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
endif()

# Общие настройки предупреждений для целей проекта
function(liblzx_set_warnings target)
        if(MSVC)
                target_compile_options(${target} PRIVATE /W3 /utf-8)
                target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
                if(LIBLZX_WARNINGS_AS_ERRORS)
                        target_compile_options(${target} PRIVATE /WX)
                endif()
        else()
                target_compile_options(${target} PRIVATE -Wall -Wextra)
                if(LIBLZX_WARNINGS_AS_ERRORS)
                        target_compile_options(${target} PRIVATE -Werror)
                endif()
        endif()
endfunction()

set(LIBLZX_SOURCES
        liblzx/liblzx_compress_common.c
        liblzx/liblzx_lzx_common.c
        liblzx/liblzx_lzx_compress.c)

# Компрессор LZX
add_library(liblzx STATIC ${LIBLZX_SOURCES})
add_library(liblzx::liblzx ALIAS liblzx)
set_target_properties(liblzx PROPERTIES
        OUTPUT_NAME lzx
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
target_include_directories(liblzx PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/liblzx>)
liblzx_set_warnings(liblzx)
if(MSVC)
        # Код компрессора унаследован от wimlib и содержит неявные сужающие
        # преобразования (ptrdiff_t/size_t/uint64_t -> uint32_t/uint16_t)
        # для значений, ограниченных по построению: позиций в буфере окна
        # (< 2^23), длин кодов и т. п.  gcc и clang с -Wall -Wextra на них не
        # предупреждают; MSVC при /W3 выдаёт C4244/C4267.  Явные приведения в десятках мест кода
        # оригинала затруднили бы сравнение с wimlib и upstream, поэтому эти
        # предупреждения отключены только для этой цели.
        target_compile_options(liblzx PRIVATE /wd4244 /wd4267)
endif()

# Запись CAB-архивов
add_library(lzxcab STATIC cab/cabwriter.c)
add_library(liblzx::cab ALIAS lzxcab)
set_target_properties(lzxcab PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
target_include_directories(lzxcab PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cab>)
target_link_libraries(lzxcab PUBLIC liblzx)
liblzx_set_warnings(lzxcab)

if(LIBLZX_BUILD_TOOLS)
        add_executable(lzxcab_tool tools/lzxcab/lzxcab.c)
        set_target_properties(lzxcab_tool PROPERTIES
                OUTPUT_NAME lzxcab
                C_STANDARD 11
                C_STANDARD_REQUIRED ON)
        target_link_libraries(lzxcab_tool PRIVATE lzxcab)
        liblzx_set_warnings(lzxcab_tool)

        # Сравнение с cabinet.dll (FCI): только Windows
        if(WIN32)
                add_executable(cabbench tools/cabbench/cabbench.c)
                set_target_properties(cabbench PROPERTIES
                        C_STANDARD 11
                        C_STANDARD_REQUIRED ON)
                target_link_libraries(cabbench PRIVATE lzxcab cabinet)
                liblzx_set_warnings(cabbench)
        endif()
endif()

if(LIBLZX_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
endif()
