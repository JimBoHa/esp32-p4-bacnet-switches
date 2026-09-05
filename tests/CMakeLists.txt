cmake_minimum_required(VERSION 3.20)
project(bacnet_codec_tests C)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(
    bacnet_codec_tests
    test_bacnet_codec.c
    ../main/bacnet_codec.c
    ../main/config_model.c
    ../main/cov_retry_cache.c
    ../main/diagnostics_time.c
    ../main/input_line_classifier.c
    ../main/ota_auth.c
    ../main/ota_health.c
)
target_include_directories(bacnet_codec_tests PRIVATE ../main)
target_compile_features(bacnet_codec_tests PRIVATE c_std_11)
target_compile_options(
    bacnet_codec_tests
    PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
)
target_link_options(
    bacnet_codec_tests
    PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
)

add_test(NAME bacnet_codec_tests COMMAND bacnet_codec_tests)
add_test(
    NAME ota_tool_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ota_tools.py
)
