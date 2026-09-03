if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AETHER_BUILD_FUZZERS requires Clang with libFuzzer")
endif()

target_compile_options(aether_core PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
target_link_options(aether_core PRIVATE -fsanitize=address,undefined)

add_executable(fuzz_decompress fuzz_decompress.cpp)
target_link_libraries(fuzz_decompress PRIVATE aether_core)
target_compile_options(fuzz_decompress PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
target_link_options(fuzz_decompress PRIVATE -fsanitize=fuzzer,address,undefined)
set_target_properties(fuzz_decompress PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

if(AETHER_LIMIT_BUILD_PARALLELISM)
    set_property(TARGET fuzz_decompress PROPERTY JOB_POOL_COMPILE aether_compile_pool)
    set_property(TARGET fuzz_decompress PROPERTY JOB_POOL_LINK aether_link_pool)
endif()
