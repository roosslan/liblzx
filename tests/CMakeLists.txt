# Тесты записи CAB.  Каждый сценарий — отдельный тест CTest; архивы
# распаковываются системными средствами (FDI и expand.exe на Windows,
# cabextract на Linux) и сверяются побайтно.

set(CABTEST_SCENARIOS
        basic windows levels multivolume tiny reserve names empty e8 random
        unknown_size folders cancel errors)

function(liblzx_add_cabtest target)
        add_executable(${target} cabtest.c)
        set_target_properties(${target} PROPERTIES
                C_STANDARD 11
                C_STANDARD_REQUIRED ON)
        liblzx_set_warnings(${target})
        if(WIN32)
                target_link_libraries(${target} PRIVATE cabinet)
        endif()
endfunction()

liblzx_add_cabtest(cabtest)
target_link_libraries(cabtest PRIVATE lzxcab)

foreach(scenario ${CABTEST_SCENARIOS})
        add_test(NAME ${scenario}
                COMMAND cabtest ${scenario}
                        ${CMAKE_CURRENT_BINARY_DIR}/work/${scenario})
        set_tests_properties(${scenario} PROPERTIES TIMEOUT 1800)
endforeach()

# Вариант библиотеки, в котором каждый третий фрагмент LZX принудительно
# записывается несжатым блоком: проверка запасного пути при переполнении
# выходного буфера, который на реальных данных почти не встречается.
list(TRANSFORM LIBLZX_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/
        OUTPUT_VARIABLE LIBLZX_RAWTEST_SOURCES)
add_library(liblzx_rawtest STATIC
        ${LIBLZX_RAWTEST_SOURCES}
        ${PROJECT_SOURCE_DIR}/cab/cabwriter.c)
set_target_properties(liblzx_rawtest PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON)
target_include_directories(liblzx_rawtest PUBLIC
        ${PROJECT_SOURCE_DIR}/liblzx ${PROJECT_SOURCE_DIR}/cab)
target_compile_definitions(liblzx_rawtest PRIVATE LIBLZX_TEST_FORCE_UNCOMPRESSED=3)
if(MSVC)
        target_compile_options(liblzx_rawtest PRIVATE /wd4244 /wd4267 /utf-8)
        target_compile_definitions(liblzx_rawtest PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

liblzx_add_cabtest(cabtest_raw)
target_link_libraries(cabtest_raw PRIVATE liblzx_rawtest)

foreach(scenario basic multivolume e8 tiny)
        add_test(NAME raw_${scenario}
                COMMAND cabtest_raw ${scenario}
                        ${CMAKE_CURRENT_BINARY_DIR}/work/raw_${scenario})
        set_tests_properties(raw_${scenario} PROPERTIES TIMEOUT 1800)
endforeach()
