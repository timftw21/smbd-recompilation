cmake_minimum_required(VERSION 4.2)
function(smbd_run directory)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${directory}" COMMAND_ERROR_IS_FATAL ANY)
endfunction()
set(ENV{CC} "${SMBD_CC}")
set(ENV{CXX} "${SMBD_CXX}")
set(ENV{GIT_CEILING_DIRECTORIES} "${SMBD_DXVK_SOURCE}:${SMBD_MUON_SOURCE}")
set(ENV{PATH} "${SMBD_WORK}/muon:$ENV{PATH}")
set(muon "${SMBD_WORK}/muon/muon")
if(NOT EXISTS "${muon}")
    smbd_run("${SMBD_MUON_SOURCE}" "${CMAKE_COMMAND}" -E env CFLAGS=-O2
        sh bootstrap.sh "${SMBD_WORK}/muon")
    smbd_run("${SMBD_MUON_SOURCE}" "${SMBD_WORK}/muon/muon-bootstrap" setup
        -Dbuildtype=release -Dlibcurl=disabled -Dlibarchive=disabled -Dlibpkgconf=disabled
        -Dnative_backtrace=disabled -Ddocs=disabled -Dmeson-docs=disabled -Dmeson-tests=disabled
        "${SMBD_WORK}/muon")
    smbd_run("${SMBD_MUON_SOURCE}" "${SMBD_NINJA}" -C "${SMBD_WORK}/muon" -j1 muon)
endif()
file(MAKE_DIRECTORY "${SMBD_WORK}/pkgconfig" "${SMBD_WORK}/lib")
file(WRITE "${SMBD_WORK}/pkgconfig/sdl3.pc"
    "Name: SDL3\nDescription: Shared SDL3 built by Super Monkey Ball Deluxe\nVersion: 3.4.20\nLibs: -L\"${SMBD_SDL_LIB}\" -lSDL3\nCflags: -I\"${SMBD_SDL_INCLUDE}\"\n")
set(ENV{PKG_CONFIG_PATH} "${SMBD_WORK}/pkgconfig:$ENV{PKG_CONFIG_PATH}")
smbd_run("${SMBD_DXVK_SOURCE}" "${muon}" setup -W -Dbuildtype=release -Ddebug=false -Db_ndebug=if-release
    "-Dcpp_link_args=['-Wl,-rpath,$ORIGIN']"
    -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false
    -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled "${SMBD_WORK}/dxvk")
smbd_run("${SMBD_DXVK_SOURCE}" "${SMBD_NINJA}" -C "${SMBD_WORK}/dxvk" -j1
    src/d3d11/libdxvk_d3d11.so.0.30101 src/dxgi/libdxvk_dxgi.so.0.30101)
foreach(component IN ITEMS d3d11 dxgi)
    set(name "libdxvk_${component}.so.0.30101")
    file(COPY_FILE "${SMBD_WORK}/dxvk/src/${component}/${name}" "${SMBD_WORK}/lib/${name}")
    if(SMBD_STRIP)
        smbd_run("${SMBD_WORK}/lib" "${SMBD_STRIP}" --strip-unneeded "${name}")
    endif()
    if(NOT EXISTS "${SMBD_WORK}/lib/libdxvk_${component}.so.0")
        file(CREATE_LINK "${name}" "${SMBD_WORK}/lib/libdxvk_${component}.so.0" SYMBOLIC)
    endif()
endforeach()
