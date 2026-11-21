foreach(option IN ITEMS SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL SDL_GPU SDL_OPENGL SDL_OPENGLES)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
elseif(SMBD_LINUX_INSTALLER)
    # The Linux installer is one self-contained executable drawn by SDL's
    # software renderer, so it needs no GPU driver. It has its own build tree.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_RENDER ON CACHE BOOL "" FORCE)
    set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
else()
    # The renderer and the DXVK WSI must see the same SDL window objects.
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(sdl3
    URL https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-3.4.20
    URL_HASH SHA256=5CB56D4223A41283F3C2BAF6F1263FBA171692409AE405A50417A7C8E7E28DEB
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(imgui
    URL https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.93.0
    URL_HASH SHA256=F7BF6BC76E5D79D40D751096663E6C5D18DAF08129DD52E7F7B09E4251C8EEEE
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(imgui)
if(SMBD_BATCH OR SMBD_LINUX_INSTALLER)
    FetchContent_MakeAvailable(sdl3)
endif()
