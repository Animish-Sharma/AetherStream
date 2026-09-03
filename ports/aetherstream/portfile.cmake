# Overlay port for a checkout of the AetherStream repository.
get_filename_component(SOURCE_PATH "${CURRENT_PORT_DIR}/../.." ABSOLUTE)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DAETHER_BUILD_PYTHON=OFF
        -DAETHER_BUILD_TESTS=OFF
        -DAETHER_BUILD_FUZZERS=OFF
        -DAETHER_LOW_MEMORY=ON
    OPTIONS_FEATURES
        native AETHER_NATIVE_OPTIMIZED
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME AetherStream CONFIG_PATH lib/cmake/AetherStream)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
