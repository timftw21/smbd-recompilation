# Backport DXVK commit 3dbf33cb2205dd0d573cbc7a65e3855d97a4800b.
# Released Native binaries still present at the previous buffer dimensions.
# Muon builds the pinned sources without requiring Python/Meson.
FetchContent_Declare(smbd_dxvk_source
    URL https://codeload.github.com/doitsujin/dxvk/tar.gz/refs/tags/v3.1.1
    URL_HASH SHA256=EF4944134F6C39D0937F265391DB2D08BCA33CD3A2F0EE9133E1EB55F1CC657C
    SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/dxvk-3.1.1-src"
    SOURCE_SUBDIR smbd-no-cmake DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(smbd_muon
    URL https://codeload.github.com/muon-build/muon/tar.gz/refs/tags/0.7.1
    URL_HASH SHA256=967A99ADE648A850807D5637C80D55043DED34556E9DCBFEC8736D6DE52F96C5
    SOURCE_SUBDIR smbd-no-cmake DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(smbd_dxvk_source smbd_muon)

function(smbd_dxvk_submodule name destination repository revision checksum)
    FetchContent_Declare(${name}
        URL "https://codeload.github.com/${repository}/tar.gz/${revision}"
        URL_HASH "SHA256=${checksum}"
        SOURCE_DIR "${smbd_dxvk_source_SOURCE_DIR}/${destination}"
        SOURCE_SUBDIR smbd-no-cmake DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(${name})
endfunction()
smbd_dxvk_submodule(smbd_dxvk_vulkan include/vulkan KhronosGroup/Vulkan-Headers
    8864cdc896bbc2a9b6eb36b3218fc9ef57908d77 35824117F649BEFC664A12622BC801EDDBBA036AC03D906DB400EE1C1E7903E9)
smbd_dxvk_submodule(smbd_dxvk_spirv include/spirv KhronosGroup/SPIRV-Headers
    04f10f650d514df88b76d25e83db360142c7b174 1B220E3EEC1714F0451B0E3652979BD280EDF10893F617837B88E6359A804DED)
smbd_dxvk_submodule(smbd_dxvk_spv subprojects/dxbc-spirv doitsujin/dxbc-spirv
    bf14419e5fa7eacb817b7b632f03cb61d61bbad7 7A2BD35D2A2186F5F77137C00302B0D9364AC01017E8AE41E30F61850D7CC2F4)
smbd_dxvk_submodule(smbd_dxvk_spv_headers subprojects/dxbc-spirv/submodules/spirv_headers KhronosGroup/SPIRV-Headers
    c8ad050fcb29e42a2f57d9f59e97488f465c436d 5B6F38B430F1C68C7395C570053403DA14EB4CECC7FCB4040E7A5248C2F63874)
smbd_dxvk_submodule(smbd_dxvk_directx include/native/directx Joshua-Ashton/mingw-directx-headers
    9df86f2341616ef1888ae59919feaa6d4fad693d 9C1B8BBFD2D6C758FAC4D93C2808A46BC1EEE429FE656B4F21B29E02615AA8AA)
smbd_dxvk_submodule(smbd_dxvk_display subprojects/libdisplay-info doitsujin/libdisplay-info
    275e6459c7ab1ddd4b125f28d0440716e4888078 C83C3E600F90A3AC98932E9259DE62AD8AD3A65DD5EFB1D2ABCB1E4D1C8F494A)

find_program(smbd_dxvk_patch patch REQUIRED)
find_program(smbd_dxvk_ninja ninja REQUIRED)
find_program(smbd_dxvk_glslang NAMES glslang glslangValidator REQUIRED)
find_program(smbd_dxvk_perl perl REQUIRED)
file(READ "${smbd_dxvk_source_SOURCE_DIR}/src/d3d11/d3d11_swapchain.cpp" smbd_dxvk_swapchain)
string(FIND "${smbd_dxvk_swapchain}" "setSurfaceExtent({ pDesc->Width, pDesc->Height })" smbd_dxvk_fixed)
if(smbd_dxvk_fixed EQUAL -1)
    execute_process(COMMAND "${smbd_dxvk_patch}" -p1 -i "${CMAKE_CURRENT_LIST_DIR}/dxvk-native.patch"
        WORKING_DIRECTORY "${smbd_dxvk_source_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/dxvk-pnp.pl"
    "${smbd_dxvk_source_SOURCE_DIR}/subprojects/libdisplay-info/tool/gen-search-table.pl" COPYONLY)

set(smbd_dxvk_work "${CMAKE_BINARY_DIR}/_deps/dxvk-backport-3.1.1/$<CONFIG>")
set(smbd_dxvk_library_dir "${smbd_dxvk_work}/lib")
set(smbd_dxvk_libraries "${smbd_dxvk_library_dir}/libdxvk_d3d11.so.0.30101"
    "${smbd_dxvk_library_dir}/libdxvk_dxgi.so.0.30101")
add_custom_command(OUTPUT ${smbd_dxvk_libraries}
    COMMAND "${CMAKE_COMMAND}"
        "-DSMBD_DXVK_SOURCE=${smbd_dxvk_source_SOURCE_DIR}"
        "-DSMBD_MUON_SOURCE=${smbd_muon_SOURCE_DIR}"
        "-DSMBD_WORK=${smbd_dxvk_work}"
        "-DSMBD_CC=${CMAKE_C_COMPILER}" "-DSMBD_CXX=${CMAKE_CXX_COMPILER}"
        "-DSMBD_NINJA=${smbd_dxvk_ninja}" "-DSMBD_STRIP=${CMAKE_STRIP}"
        "-DSMBD_SDL_INCLUDE=${sdl3_SOURCE_DIR}/include"
        "-DSMBD_SDL_LIB=$<TARGET_FILE_DIR:SDL3-shared>"
        -P "${CMAKE_CURRENT_LIST_DIR}/build-dxvk.cmake"
    DEPENDS SDL3-shared "${CMAKE_CURRENT_LIST_FILE}" "${CMAKE_CURRENT_LIST_DIR}/build-dxvk.cmake"
        "${CMAKE_CURRENT_LIST_DIR}/dxvk-native.patch" "${CMAKE_CURRENT_LIST_DIR}/dxvk-pnp.pl"
        "${smbd_dxvk_source_SOURCE_DIR}/src/d3d11/d3d11_swapchain.cpp"
        "${smbd_dxvk_source_SOURCE_DIR}/subprojects/libdisplay-info/tool/gen-search-table.pl"
        "${smbd_dxvk_source_SOURCE_DIR}/subprojects/libdisplay-info/meson.build"
    USES_TERMINAL VERBATIM)
add_custom_target(smbd-dxvk-native DEPENDS ${smbd_dxvk_libraries})
foreach(component IN ITEMS d3d11 dxgi)
    add_library(smbd-dxvk-${component} SHARED IMPORTED GLOBAL)
    set_target_properties(smbd-dxvk-${component} PROPERTIES
        IMPORTED_SONAME "libdxvk_${component}.so.0"
        IMPORTED_CONFIGURATIONS "${CMAKE_CONFIGURATION_TYPES}")
    foreach(config IN ITEMS "${CMAKE_BUILD_TYPE}" ${CMAKE_CONFIGURATION_TYPES})
        string(REPLACE "$<CONFIG>" "${config}" location "${smbd_dxvk_library_dir}/libdxvk_${component}.so.0.30101")
        set(property IMPORTED_LOCATION)
        if(config)
            string(TOUPPER "${config}" upper)
            string(APPEND property "_${upper}")
        endif()
        set_property(TARGET smbd-dxvk-${component} PROPERTY ${property} "${location}")
    endforeach()
    add_dependencies(smbd-dxvk-${component} smbd-dxvk-native)
endforeach()
