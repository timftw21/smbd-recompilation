find_package(PkgConfig REQUIRED)
pkg_check_modules(SMBD_VKD3D REQUIRED IMPORTED_TARGET smbd-vkd3d-shader=2.1)
FetchContent_Declare(dxvk_native
    URL https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-native-3.1.1-steamrt-sniper.tar.gz
    URL_HASH SHA256=506FA1EA5BB6CE9A89E11436F7AB5191BC0196E8A577E0CF0B16A45BBC9991B7
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(dxvk_native)
add_library(smbd-dxvk-headers INTERFACE)
target_include_directories(smbd-dxvk-headers SYSTEM INTERFACE "${dxvk_native_SOURCE_DIR}/include/dxvk")
target_compile_definitions(smbd-dxvk-headers INTERFACE DXVK_WSI_SDL3)
target_compile_options(smbd-dxvk-headers INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-fms-extensions;-Wno-ignored-attributes>")
include("${CMAKE_CURRENT_LIST_DIR}/ui-dependencies.cmake")
