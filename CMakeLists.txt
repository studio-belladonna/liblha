cmake_minimum_required(VERSION 3.20)

project(liblha
    VERSION 0.1.0
    DESCRIPTION "Lempel-Ziv-Huffman encoding library"
    LANGUAGES CXX
)

set(SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")

add_library(liblha STATIC)
add_library(liblha::liblha ALIAS liblha)

target_sources(liblha
    PRIVATE
        "${SOURCE_DIR}/lha.cpp"
        "${SOURCE_DIR}/archive.cpp"
        "${SOURCE_DIR}/codecs/codec_common.cpp"
        "${SOURCE_DIR}/codecs/codec_modules.cpp"
        "${SOURCE_DIR}/codecs/lh0.cpp"
        "${SOURCE_DIR}/codecs/lh1.cpp"
        "${SOURCE_DIR}/codecs/lh2.cpp"
        "${SOURCE_DIR}/codecs/lh3.cpp"
        "${SOURCE_DIR}/codecs/lh4.cpp"
        "${SOURCE_DIR}/codecs/lh_family.cpp"
        "${SOURCE_DIR}/codecs/lh5.cpp"
        "${SOURCE_DIR}/codecs/lh6.cpp"
        "${SOURCE_DIR}/codecs/lh7.cpp"
        "${SOURCE_DIR}/codecs/lz4.cpp"
        "${SOURCE_DIR}/codecs/lz5.cpp"
        "${SOURCE_DIR}/codecs/lzs.cpp"
        "${SOURCE_DIR}/codecs/lhx.cpp"
        "${SOURCE_DIR}/codecs/lk7.cpp"
        "${SOURCE_DIR}/codecs/pma.cpp"
)

target_compile_features(liblha PUBLIC cxx_std_20)

target_include_directories(liblha
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

option(liblha_BUILD_TESTS "Build compression tests" ON)

if(liblha_BUILD_TESTS)
    enable_testing()

    # all codecs
    add_executable(liblha_codec_tests
        "${TESTS_DIR}/test_all.cpp"
    )

    target_link_libraries(liblha_codec_tests PRIVATE liblha::liblha)

    foreach(method IN ITEMS lh0 lh1 lh2 lh3 lh4 lh5 lh6 lh7 lz4 lz5 lzs lhx lk7 pm0 pm1 pm2)
        add_test(
            NAME codec_${method} 
            COMMAND liblha_codec_tests -${method}-
        )
    endforeach()
endif()