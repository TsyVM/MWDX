cmake_minimum_required(VERSION 3.20)
project(MWDX CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── x86 only — NFSMW speed.exe is 32-bit ─────────────────────────────────────
if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR
   CMAKE_GENERATOR_PLATFORM STREQUAL "AMD64" OR
   (NOT CMAKE_GENERATOR_PLATFORM AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(FATAL_ERROR
        "MWDX must be built for Win32 (x86).\n"
        "Use Build.bat, or pass -A Win32 to CMake.")
endif()
message(STATUS "Target: Win32 / x86")

# ── DirectX June 2010 SDK ─────────────────────────────────────────────────────
if(NOT DEFINED DXSDK_DIR OR DXSDK_DIR STREQUAL "")
    if(DEFINED ENV{DXSDK_DIR})
        set(DXSDK_DIR "$ENV{DXSDK_DIR}")
    else()
        # The June 2010 installer does not always leave DXSDK_DIR behind, so
        # look where it actually puts itself before giving up.
        foreach(candidate
                "$ENV{ProgramFiles\(x86\)}/Microsoft DirectX SDK (June 2010)"
                "$ENV{ProgramFiles}/Microsoft DirectX SDK (June 2010)"
                "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)"
                "C:/Program Files/Microsoft DirectX SDK (June 2010)")
            if(EXISTS "${candidate}/Lib/x86/d3d9.lib")
                set(DXSDK_DIR "${candidate}")
                break()
            endif()
        endforeach()
    endif()
endif()
if(NOT DXSDK_DIR OR DXSDK_DIR STREQUAL "")
    message(FATAL_ERROR "DXSDK_DIR not set. Install DirectX June 2010 SDK.")
endif()
string(REGEX REPLACE "[/\\\\]+$" "" DXSDK_DIR "${DXSDK_DIR}")
message(STATUS "DXSDK_DIR: ${DXSDK_DIR}")

# ── External library roots ────────────────────────────────────────────────────
# Prefer the in-tree copies under external/ so a clone builds without depending
# on an unrelated absolute path; fall back to the TeamVanilla working copies.
function(mw4real_pick_root outVar inTree fallback)
    if(EXISTS "${inTree}")
        set(${outVar} "${inTree}" PARENT_SCOPE)
    else()
        set(${outVar} "${fallback}" PARENT_SCOPE)
    endif()
endfunction()

mw4real_pick_root(VANGFX_ROOT "${CMAKE_SOURCE_DIR}/external/VanGFX"
                              "C:/Users/Ty/Desktop/TeamVanilla/Projects/VanGFX")
mw4real_pick_root(VANGUI_ROOT "${CMAKE_SOURCE_DIR}/external/VanGUI"
                              "C:/Users/Ty/Desktop/TeamVanilla/Projects/VanGUI")
set(VANHOOKS_ROOT "${CMAKE_SOURCE_DIR}/external/VanHooks")
message(STATUS "VanGFX root: ${VANGFX_ROOT}")
message(STATUS "VanGUI root: ${VANGUI_ROOT}")

# ── Zydis (required by VanHooks) ─────────────────────────────────────────────
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG        v4.1.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
set(ZYDIS_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zydis)

# ── VanHooks static lib ───────────────────────────────────────────────────────
add_library(vanhooks STATIC
    "${VANHOOKS_ROOT}/src/engine.cpp"
    "${VANHOOKS_ROOT}/src/disasm.cpp"
    "${VANHOOKS_ROOT}/src/memory_win.cpp"
    "${VANHOOKS_ROOT}/src/tables_win.cpp"
    "${VANHOOKS_ROOT}/src/thread_fence.cpp"
    "${VANHOOKS_ROOT}/src/trampoline.cpp"
    "${VANHOOKS_ROOT}/src/vh_bridge.cpp"
)
target_include_directories(vanhooks PUBLIC "${VANHOOKS_ROOT}/include")
target_link_libraries(vanhooks PUBLIC Zydis)
set_target_properties(vanhooks PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(vanhooks PRIVATE
        $<$<CONFIG:Release>:/MT>
        $<$<CONFIG:Debug>:/MTd>
    )
endif()

# ── DX11 backend sources ──────────────────────────────────────────────────────
set(DX11_SOURCES
    src/DirectX-Handler/dx11/BackendSelect.cpp
    src/DirectX-Handler/dx11/core/CapsProbe.cpp
    src/DirectX-Handler/dx11/core/CapsTable.cpp
    src/DirectX-Handler/dx11/core/ConstantMapper.cpp
    src/DirectX-Handler/dx11/core/D3D9ShaderTranslator.cpp
    src/DirectX-Handler/dx11/core/DeviceContext11.cpp
    src/DirectX-Handler/dx11/core/DynamicRingBuffer.cpp
    src/DirectX-Handler/dx11/core/FFPEmulator.cpp
    src/DirectX-Handler/dx11/core/FFPShaderGen.cpp
    src/DirectX-Handler/dx11/core/FormatConverter.cpp
    src/DirectX-Handler/dx11/core/InputLayoutCache.cpp
    src/DirectX-Handler/dx11/core/Log.cpp
    src/DirectX-Handler/dx11/core/PSOCache.cpp
    src/DirectX-Handler/dx11/core/QueryBridge.cpp
    src/DirectX-Handler/dx11/core/RenderStateTracker.cpp
    src/DirectX-Handler/dx11/core/ResourceManager.cpp
    src/DirectX-Handler/dx11/core/ShaderCache.cpp
    src/DirectX-Handler/dx11/core/ShaderMods.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9CubeTexture.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Device.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Device_Draw.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Device_Present.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Device_Resource.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Device_State.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9IndexBuffer.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9PixelShader.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Query.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Root.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9StateBlock.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Surface.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9Texture.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9VertexBuffer.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9VertexDecl.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9VertexShader.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9VolumeTexture.cpp
    src/DirectX-Handler/dx11/d3d9proxy/D9SwapChain.cpp
)

# ── DX12 backend sources ──────────────────────────────────────────────────────
set(DX12_SOURCES
    src/DirectX-Handler/dx12/core/Blitter12.cpp
    src/DirectX-Handler/dx12/core/DeviceContext12.cpp
    src/DirectX-Handler/dx12/core/FFPEmulator12.cpp
    src/DirectX-Handler/dx12/core/PSOCache12.cpp
    src/DirectX-Handler/dx12/core/Resource12.cpp
    src/DirectX-Handler/dx12/core/ShaderCache12.cpp
    src/DirectX-Handler/dx12/core/TextureStorage12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/Buffers12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Root12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/Shaders12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12_Draw.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12_Present.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12_Resource.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12_State.cpp
    src/DirectX-Handler/dx12/d3d9proxy/Textures12.cpp
)

# ── MWDX sources ───────────────────────────────────────────────────────────
set(SOURCES
    src/dllmain.cpp
    src/Logger.cpp
    src/DirectX-Handler/directx/WrappedIDirect3D9.cpp
    src/DirectX-Handler/directx/WrappedIDirect3DDevice9.cpp
    ${DX11_SOURCES}
    ${DX12_SOURCES}
)

# ── DLL target ────────────────────────────────────────────────────────────────
add_library(MWDX_dll SHARED ${SOURCES})

target_include_directories(MWDX_dll PRIVATE
    src
    src/DirectX-Handler
    src/DirectX-Handler/directx
    src/DirectX-Handler/dx11/inc
    src/DirectX-Handler/dx12/inc
    external/MWSDK/include
    "${VANGFX_ROOT}/include"
    "${VANGFX_ROOT}/src"
    "${VANGFX_ROOT}/src/internal"
    "${VANGUI_ROOT}/include"
    "${VANGUI_ROOT}/include/vangui"
    "${VANGUI_ROOT}/src"
    "${VANGUI_ROOT}/backends"
)
# DXSDK include intentionally omitted — Windows 10 SDK ships d3d9.h, dxgi.h,
# d3dcompiler.h with modern signatures. DXSDK headers are June-2010 vintage
# and lack DXGI 1.2 types (DXGI_FORMAT_B4G4R4A4_UNORM) and the 4-arg D3DReflect.

set(DXSDK_LIB "${DXSDK_DIR}/Lib/x86")
target_link_libraries(MWDX_dll PRIVATE
    vanhooks
    "${DXSDK_LIB}/d3d9.lib"
    "${DXSDK_LIB}/dxguid.lib"
    d3d11.lib
    d3d12.lib
    dxgi.lib
    d3dcompiler.lib
)

# ── Static CRT ────────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(MWDX_dll PRIVATE
        $<$<CONFIG:Release>:/MT>
        $<$<CONFIG:Debug>:/MTd>
        /W3
        /wd4996
        /wd4005
    )
endif()

target_compile_definitions(MWDX_dll PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    MW4REAL_VERSION_MAJOR=0
    MW4REAL_VERSION_MINOR=1
)

set_target_properties(MWDX_dll PROPERTIES
    OUTPUT_NAME "d3d9"
    SUFFIX      ".dll"
    PDB_NAME    "MWDX"
    LINK_FLAGS  "/DEF:\"${CMAKE_SOURCE_DIR}/MWDX.def\" /NODEFAULTLIB:MSVCRT"
)
