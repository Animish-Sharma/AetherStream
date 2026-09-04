cmake_minimum_required(VERSION 3.18)
project(AetherStream VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx512f" COMPILER_HAS_AVX512)
check_cxx_compiler_flag("-mavx2" COMPILER_HAS_AVX2)

# Default to a conservative, low-memory build.  High optimization levels,
# AVX-512, and many parallel compiler jobs can make template-heavy pybind11
# builds fail on small machines.  Enable the native profile explicitly when
# building on a machine with enough RAM:
#   cmake -S . -B build -DAETHER_NATIVE_OPTIMIZED=ON
option(AETHER_NATIVE_OPTIMIZED "Use -O3 and host-native SIMD flags" OFF)
option(AETHER_LOW_MEMORY "Use the 2GB-safe -O1/-g0/single-job profile" ON)
option(AETHER_LIMIT_BUILD_PARALLELISM "Compile/link one target at a time to reduce peak memory" ON)
if(DEFINED ENV{AETHER_LOW_MEMORY} AND "$ENV{AETHER_LOW_MEMORY}" STREQUAL "0")
    set(AETHER_LOW_MEMORY OFF CACHE BOOL "Use the low-memory build profile" FORCE)
endif()
option(AETHER_BUILD_PYTHON "Build the pybind11 Python extension when pybind11 is available" ON)
option(AETHER_BUILD_TESTS "Build native C++ tests" OFF)
option(AETHER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(AETHER_BUILD_FUZZERS "Build LLVM libFuzzer targets" OFF)

if(DEFINED ENV{AETHER_NATIVE_OPTIMIZED} AND
   "$ENV{AETHER_NATIVE_OPTIMIZED}" STREQUAL "1")
    set(AETHER_NATIVE_OPTIMIZED ON CACHE BOOL "Use native SIMD" FORCE)
endif()
if(DEFINED ENV{AETHER_LOW_MEMORY} AND "$ENV{AETHER_LOW_MEMORY}" STREQUAL "1")
    set(AETHER_LOW_MEMORY ON CACHE BOOL "Use the low-memory build profile" FORCE)
endif()

if(AETHER_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "AETHER_ENABLE_SANITIZERS requires GCC or Clang")
    endif()
endif()

if(MSVC)
    set(AETHER_WARNING_FLAGS /W4 /permissive-)
    if(AETHER_NATIVE_OPTIMIZED)
        set(AETHER_OPTIMIZATION_FLAGS /O2 /fp:fast)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
            list(APPEND AETHER_OPTIMIZATION_FLAGS /arch:AVX2)
        endif()
    else()
        set(AETHER_OPTIMIZATION_FLAGS /O1 /GL-)
    endif()
else()
    set(AETHER_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wnon-virtual-dtor
        -Wnull-dereference
    )
endif()

if(NOT MSVC AND AETHER_NATIVE_OPTIMIZED)
    set(AETHER_OPTIMIZATION_FLAGS -O3 -ffast-math)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        check_cxx_compiler_flag("-mcpu=native" COMPILER_HAS_MCPU_NATIVE)
        if(COMPILER_HAS_MCPU_NATIVE)
            list(APPEND AETHER_OPTIMIZATION_FLAGS -mcpu=native)
        endif()
    else()
        check_cxx_compiler_flag("-march=native" COMPILER_HAS_MARCH_NATIVE)
        if(COMPILER_HAS_MARCH_NATIVE)
            list(APPEND AETHER_OPTIMIZATION_FLAGS -march=native)
        endif()
    endif()
elseif(NOT MSVC)
    # AETHER_LOW_MEMORY=0 permits the toolchain's standard Release flags while
    # leaving LTO disabled.
    if(AETHER_ENABLE_SANITIZERS)
        set(AETHER_OPTIMIZATION_FLAGS -O1 -g -fno-lto)
    elseif(AETHER_LOW_MEMORY)
        set(AETHER_OPTIMIZATION_FLAGS -O1 -g0 -fno-lto)
    else()
        set(AETHER_OPTIMIZATION_FLAGS -fno-lto)
    endif()
endif()

if(AETHER_LIMIT_BUILD_PARALLELISM)
    set_property(GLOBAL PROPERTY JOB_POOLS aether_compile_pool=1 aether_link_pool=1)
endif()

set(
    AETHER_CORE_SOURCES
    src/common.cpp
    src/predictor.cpp
    src/ged_estimator.cpp
    src/eclm_quantizer.cpp
    src/vrans_codec.cpp
    src/stream_encoder.cpp
    src/table.cpp
)

add_library(aether_core ${AETHER_CORE_SOURCES})
add_library(aether::aether_core ALIAS aether_core)
target_include_directories(aether_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aether_core PUBLIC cxx_std_20)
target_compile_options(aether_core PRIVATE ${AETHER_OPTIMIZATION_FLAGS} ${AETHER_WARNING_FLAGS})
set_target_properties(
    aether_core
    PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION OFF
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        POSITION_INDEPENDENT_CODE ON
        EXPORT_NAME aether_core
)
# Windows toolchains create an import library for the Python module. Give the
# core library a distinct basename so both targets can share one output directory.
if(WIN32)
    set_target_properties(aether_core PROPERTIES OUTPUT_NAME aether_core)
else()
    set_target_properties(aether_core PROPERTIES OUTPUT_NAME aether)
endif()
if(AETHER_LIMIT_BUILD_PARALLELISM)
    set_property(TARGET aether_core PROPERTY JOB_POOL_COMPILE aether_compile_pool)
    set_property(TARGET aether_core PROPERTY JOB_POOL_LINK aether_link_pool)
endif()

if(AETHER_BUILD_PYTHON)
    set(PYBIND11_FINDPYTHON ON)
    find_package(Python COMPONENTS Interpreter Development.Module QUIET)
    find_package(pybind11 CONFIG QUIET)
    if(pybind11_FOUND)
        pybind11_add_module(aether src/bindings.cpp)
        target_link_libraries(aether PRIVATE aether_core)
        target_compile_options(aether PRIVATE ${AETHER_OPTIMIZATION_FLAGS} ${AETHER_WARNING_FLAGS})
        set_target_properties(aether PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
        if(AETHER_LIMIT_BUILD_PARALLELISM)
            set_property(TARGET aether PROPERTY JOB_POOL_COMPILE aether_compile_pool)
            set_property(TARGET aether PROPERTY JOB_POOL_LINK aether_link_pool)
        endif()
        install(TARGETS aether LIBRARY DESTINATION . RUNTIME DESTINATION .)
        install(DIRECTORY python/aetherstream DESTINATION .
                FILES_MATCHING PATTERN "*.py")
    endif()
endif()

if(AETHER_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    foreach(test_name predictor error_bound stream_encoder vrans sanitizers concurrency random_access)
        add_executable(
            test_${test_name}
            tests/test_${test_name}.cpp
        )
        target_link_libraries(test_${test_name} PRIVATE aether_core)
        target_compile_options(
            test_${test_name}
            PRIVATE ${AETHER_OPTIMIZATION_FLAGS} ${AETHER_WARNING_FLAGS}
        )
        # The native tests use assert(), including in Release configurations.
        if(MSVC)
            target_compile_options(test_${test_name} PRIVATE /UNDEBUG)
        else()
            target_compile_options(test_${test_name} PRIVATE -UNDEBUG)
        endif()
        set_target_properties(test_${test_name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
        if(AETHER_LIMIT_BUILD_PARALLELISM)
            set_property(TARGET test_${test_name} PROPERTY JOB_POOL_COMPILE aether_compile_pool)
            set_property(TARGET test_${test_name} PROPERTY JOB_POOL_LINK aether_link_pool)
        endif()
        if(test_name STREQUAL "concurrency")
            target_link_libraries(test_${test_name} PRIVATE Threads::Threads)
        elseif(test_name STREQUAL "sanitizers")
            target_compile_definitions(test_${test_name} PRIVATE
                AETHER_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
        endif()
        add_test(NAME ${test_name} COMMAND test_${test_name})
    endforeach()
endif()

if(AETHER_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

install(
    TARGETS aether_core
    EXPORT AetherStreamTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/aether DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    EXPORT AetherStreamTargets
    FILE AetherStreamTargets.cmake
    NAMESPACE aether::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AetherStream
)
configure_package_config_file(
    cmake/AetherStreamConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AetherStreamConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AetherStream
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AetherStreamConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/AetherStreamConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/AetherStreamConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AetherStream
)
