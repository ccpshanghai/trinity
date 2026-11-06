set(VCPKG_POLICY_DLLS_IN_STATIC_LIBRARY enabled)

set(DIRECTX_DXC_TAG v1.9.2602)
set(DIRECTX_DXC_VERSION 2026_02_20)

if (NOT VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
   message(STATUS "Note: ${PORT} always requires dynamic library linkage at runtime.")
endif()

if (VCPKG_TARGET_IS_OSX)
    # Upstream ${DIRECTX_DXC_TAG} publishes Windows and Linux archives only, so on macOS
    # this port builds the same tag from source -- see vcpkg.json's $comment for why this
    # port exists at all. A GitHub archive carries no submodules, so the three DXC needs
    # are fetched at the commits ${DIRECTX_DXC_TAG}'s gitlinks name and handed to its
    # CMake by path. (`.gitmodules` also lists googletest, but the tag has no gitlink for
    # it, and effcee/re2 are SPIRV-Tools' test dependencies; with tests off none is needed.)
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO microsoft/DirectXShaderCompiler
        REF ${DIRECTX_DXC_TAG}
        SHA512 66137da630163828bea416cb2e336a52e425a43a9e57a805a0dc29efb02b62e31c924b6afd45fac954e3292ee2e77c64779a86ef01e774cc7e6c689b0975b9fd
        HEAD_REF main
    )
    vcpkg_from_github(
        OUT_SOURCE_PATH SPIRV_HEADERS_PATH
        REPO KhronosGroup/SPIRV-Headers
        REF 04f10f650d514df88b76d25e83db360142c7b174
        SHA512 cae8cd179c9013068876908fecc1d158168310ad6ac250398a41f0f5206ceff6469e2aaeab9c820bce9d1b08950c725c89c46e94b89a692be9805432cf749396
        HEAD_REF main
    )
    vcpkg_from_github(
        OUT_SOURCE_PATH SPIRV_TOOLS_PATH
        REPO KhronosGroup/SPIRV-Tools
        REF fbe4f3ad913c44fe8700545f8ffe35d1382b7093
        SHA512 e76e383d2e26c5d4953fa5863cc1973cb70a4a570f185ec2cb1bd2ad0a7c49c18dc5e687d8b41f49eb3a4e39c0b8ed960af65bd8cdc0b1fb34baf9e25bb7062b
        HEAD_REF main
    )
    vcpkg_from_github(
        OUT_SOURCE_PATH DIRECTX_HEADERS_PATH
        REPO microsoft/DirectX-Headers
        REF 980971e835876dc0cde415e8f9bc646e64667bf7
        SHA512 a570068d3c25c68eba34a2653108e2019f888a7392a803f97da9a401713b14bf4235520f0adc37c2f6ffb27cfb118ca7bb0fc8e446b342a7393c9c64afd84ed8
        HEAD_REF main
    )

    vcpkg_find_acquire_program(PYTHON3)

    # DXC's LLVM must be static: libdxcompiler.dylib is the one shared object it is meant
    # to produce, with LLVM linked into it. The triplet's dynamic linkage would make
    # vcpkg_cmake_configure pass BUILD_SHARED_LIBS=ON after OPTIONS, where an OFF of ours
    # would lose, and turn every LLVM component into a dylib of its own.
    set(VCPKG_LIBRARY_LINKAGE static)
    # DXC does not build against the macOS 26.5 SDK without this (docs/HANDOFF-m6-macbook.md
    # in carbon-trinity-mobile); vcpkg_cmake_configure wants the C flags defined alongside.
    string(APPEND VCPKG_CXX_FLAGS " -Wno-invalid-specialization")
    string(APPEND VCPKG_C_FLAGS "")

    # The flags are docs/BuildingAndTestingDXC.rst's (the PredefinedParams cache script,
    # SPIR-V on) with every test tree off, as the hand-built dxc this replaces was made.
    vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}"
        OPTIONS
            -C "${SOURCE_PATH}/cmake/caches/PredefinedParams.cmake"
            "-DPython3_EXECUTABLE=${PYTHON3}"
            -DLLVM_ENABLE_ASSERTIONS=OFF
            -DHLSL_INCLUDE_TESTS=OFF
            -DLLVM_INCLUDE_TESTS=OFF
            -DCLANG_INCLUDE_TESTS=OFF
            -DSPIRV_BUILD_TESTS=OFF
            -DENABLE_SPIRV_CODEGEN=ON
            "-DDXC_SPIRV_HEADERS_DIR=${SPIRV_HEADERS_PATH}"
            "-DDXC_SPIRV_TOOLS_DIR=${SPIRV_TOOLS_PATH}"
            "-DDIRECTX_HEADER_INCLUDE_DIR=${DIRECTX_HEADERS_PATH}/include"
    )
    # The driver only: libdxcompiler is its dependency, and the `install` target would
    # install DXC's whole LLVM.
    vcpkg_cmake_build(TARGET dxc LOGFILE_BASE build-dxc)

    set(DXC_OSX_BUILD "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel")
    set(DXC_OSX_SRC "${SOURCE_PATH}")
elseif (VCPKG_TARGET_IS_LINUX)
    vcpkg_download_distfile(ARCHIVE
        URLS "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DIRECTX_DXC_TAG}/linux_dxc_${DIRECTX_DXC_VERSION}.x86_64.tar.gz"
        FILENAME "linux_dxc_${DIRECTX_DXC_VERSION}.tar.gz"
        SHA512 74e1d310e3ece2b29ca6ed8836ffd99160d35f8aac4bc23e72f3a12d4f1311fc6ff405ad55683187f98a871bdac10e1342d2cd1aac05bdb3d2a81131091618cf
    )
else()
    vcpkg_download_distfile(ARCHIVE
        URLS "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DIRECTX_DXC_TAG}/dxc_${DIRECTX_DXC_VERSION}.zip"
        FILENAME "dxc_${DIRECTX_DXC_VERSION}.zip"
        SHA512 47c34ff760080f5121496db4a6b92cce88dfaaf8b16075ffb3e4487ad7b1433d4b36c4deaab55846fd9f3f01ae5e0ed71474fc538b6cad79055d66c3dc5021e8
    )
endif()

vcpkg_download_distfile(
    LICENSE_TXT
    URLS "https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/${DIRECTX_DXC_TAG}/LICENSE.TXT"
    FILENAME "LICENSE.${DIRECTX_DXC_VERSION}"
    SHA512  9feaa85ca6d42d5a2d6fe773706bbab8241e78390a9d61ea9061c8f0eeb5a3e380ff07c222e02fbf61af7f2b2f6dd31c5fc87247a94dae275dc0a20cdfcc8c9d
)

if (NOT VCPKG_TARGET_IS_OSX)
  vcpkg_extract_source_archive(
      PACKAGE_PATH
      ARCHIVE ${ARCHIVE}
      NO_REMOVE_ONE_LEVEL
  )
endif()

if (VCPKG_TARGET_IS_OSX)
  # This branch used to deliver the dxc DRIVER as a host tool and nothing else, on the
  # grounds that nothing on a Mac linked dxcompiler. Since 2026-09-08 something does:
  # ShaderCompiler's Vulkan back end is dxc's -spirv plus IDxcUtils::CreateReflection,
  # and it is built on macOS now (shadercompiler/stdafx.h). So the headers come too --
  # and not only dxc's own. macOS has no d3d11.h/d3d12shader.h of its own, and dxc's
  # DirectX-Headers submodule is where a portable d3d12shader.h and the four MIDL stubs
  # d3dcommon.h includes come from. They are installed FLAT into include/directx-dxc/,
  # which is the one directory the generated config puts on the include path; the
  # quoted includes inside d3dcommon.h ("rpc.h", "OAIdl.h", ...) then resolve against
  # their own directory, the way they do inside dxc's own build. A build of the `dxc`
  # target puts no headers anywhere, so they come out of the sources the build used.
  file(INSTALL
    "${DXC_OSX_SRC}/include/dxc/dxcapi.h"
    "${DXC_OSX_SRC}/include/dxc/dxcerrors.h"
    "${DXC_OSX_SRC}/include/dxc/dxcisense.h"
    "${DXC_OSX_SRC}/include/dxc/WinAdapter.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  # d3dcommon.h is not optional: d3d12shader.h's first line includes it, and it is what
  # carries ID3DBlob, ID3DInclude, D3D_SHADER_MACRO and every D3D_SIT_/D3D_SVT_ enum the
  # reflection code switches on.
  file(INSTALL
    "${DIRECTX_HEADERS_PATH}/include/directx/d3d12shader.h"
    "${DIRECTX_HEADERS_PATH}/include/directx/d3dcommon.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  # The four MIDL stubs d3dcommon.h includes unconditionally, plus winapifamily.h.
  # Each is a handful of lines; dxc puts the same directory on its own include path.
  file(INSTALL
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/rpc.h"
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/rpcndr.h"
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/OAIdl.h"
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/OCIdl.h"
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/winapifamily.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  # The driver's rpath is @executable_path/../lib, so installed as
  # tools/directx-dxc/dxc it resolves its dylib at tools/lib/ -- which is why the
  # dylib is copied there as well as into lib/. Copying bytes is deliberate: the
  # alternative is to put the dylib beside the binary and add @executable_path to
  # the copy with `install_name_tool -add_rpath`, and rewriting an arm64 Mach-O's
  # load commands invalidates its code signature, after which the kernel SIGKILLs
  # the binary ("Killed: 9") instead of merely warning. DXC's own CMake builds with its
  # install rpath (CMAKE_BUILD_WITH_INSTALL_RPATH), so the build tree's files are already
  # the relocatable ones.
  file(INSTALL
    "${DXC_OSX_BUILD}/lib/libdxcompiler.dylib"
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/lib")

  file(INSTALL
    "${DXC_OSX_BUILD}/lib/libdxcompiler.dylib"
    DESTINATION "${CURRENT_PACKAGES_DIR}/lib")

  if(NOT DEFINED VCPKG_BUILD_TYPE)
    file(INSTALL
      "${DXC_OSX_BUILD}/lib/libdxcompiler.dylib"
      DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
  endif()

  # bin/dxc is a symlink to bin/dxc-3.7; install the file it names, as dxc.
  file(REAL_PATH "${DXC_OSX_BUILD}/bin/dxc" _dxc_driver)
  file(INSTALL
    "${_dxc_driver}"
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/"
    RENAME dxc
    FILE_PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE)

  set(dll_name_dxc "libdxcompiler.dylib")
  # libdxil is the closed-source DXIL signer, shipped only in Microsoft's own
  # archives; a source build produces none, so Microsoft::DXIL names a file that
  # does not exist on osx. Harmless for the same reason as the include dir above.
  set(dll_name_dxil "libdxil.dylib")
  set(dll_dir  "lib")
  if(NOT DEFINED VCPKG_BUILD_TYPE)
    set(dll_debug_dir "debug/lib")
  else()
    set(dll_debug_dir "lib")
  endif()
  set(lib_name "libdxcompiler.dylib")
  set(tool_path "tools/${PORT}/dxc")
  # libdxil is the closed-source signer and a source build produces none, so
  # Microsoft::DXIL names a file that does not exist. It was harmless while nothing
  # linked the package; ShaderCompiler links it now, and an INTERFACE_LINK_LIBRARIES
  # entry pointing at a missing dylib is a link error. The target is still declared --
  # anything that names it explicitly gets the same diagnostic as before -- it just is
  # not dragged in by linking the compiler.
  set(dxc_interface_link_libraries "")
  # The dylib's install name is @rpath/libdxcompiler.dylib, so a consumer needs an
  # LC_RPATH. Told a bare soname, CMake concludes the loader will find the library on
  # its own and emits no -Wl,-rpath at all: measured 2026-09-08, ShaderCompiler linked
  # against the full path, came out with zero LC_RPATHs and died in dyld with
  # "Library not loaded: @rpath/libdxcompiler.dylib". Naming the real install name here
  # is what makes CMake add the build-tree rpath.
  set(dxc_imported_soname "@rpath/libdxcompiler.dylib")
elseif (VCPKG_TARGET_IS_LINUX)
  # The prebuilt linux_dxc archive has dxc's own headers and not d3d12shader.h.
  # The non-Windows shadercompiler stdafx includes that header the same way macOS
  # does, from the DirectX-Headers tree. Windows still takes d3d12shader.h from
  # the official zip below.
  vcpkg_from_github(
      OUT_SOURCE_PATH DIRECTX_HEADERS_PATH
      REPO microsoft/DirectX-Headers
      REF 980971e835876dc0cde415e8f9bc646e64667bf7
      SHA512 a570068d3c25c68eba34a2653108e2019f888a7392a803f97da9a401713b14bf4235520f0adc37c2f6ffb27cfb118ca7bb0fc8e446b342a7393c9c64afd84ed8
      HEAD_REF main
  )

  file(INSTALL
    "${PACKAGE_PATH}/include/dxc/dxcapi.h"
    "${PACKAGE_PATH}/include/dxc/dxcerrors.h"
    "${PACKAGE_PATH}/include/dxc/dxcisense.h"
    "${PACKAGE_PATH}/include/dxc/WinAdapter.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  file(INSTALL
    "${DIRECTX_HEADERS_PATH}/include/directx/d3d12shader.h"
    "${DIRECTX_HEADERS_PATH}/include/directx/d3dcommon.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  file(INSTALL
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/rpc.h"
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/rpcndr.h"
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/OAIdl.h"
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/OCIdl.h"
    "${DIRECTX_HEADERS_PATH}/include/wsl/stubs/winapifamily.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  file(INSTALL
    "${PACKAGE_PATH}/lib/libdxcompiler.so"
    "${PACKAGE_PATH}/lib/libdxil.so"
    DESTINATION "${CURRENT_PACKAGES_DIR}/lib")

  if(NOT DEFINED VCPKG_BUILD_TYPE)
    file(INSTALL
      "${PACKAGE_PATH}/lib/libdxcompiler.so"
      "${PACKAGE_PATH}/lib/libdxil.so"
      DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
  endif()

  file(INSTALL
    "${PACKAGE_PATH}/bin/dxc"
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/"
    FILE_PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE)

  set(dll_name_dxc "libdxcompiler.so")
  set(dll_name_dxil "libdxil.so")
  set(dll_dir  "lib")
  if(NOT DEFINED VCPKG_BUILD_TYPE)
    set(dll_debug_dir "debug/lib")
  else()
    set(dll_debug_dir "lib")
  endif()
  set(lib_name "libdxcompiler.so")
  set(tool_path "tools/${PORT}/dxc")
else()
  # VCPKG_TARGET_IS_WINDOWS
  if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
      set(DXC_ARCH arm64)
  elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
      set(DXC_ARCH x86)
  else()
      set(DXC_ARCH x64)
  endif()

  file(INSTALL
    "${PACKAGE_PATH}/inc/dxcapi.h"
    "${PACKAGE_PATH}/inc/dxcerrors.h"
    "${PACKAGE_PATH}/inc/dxcisense.h"
    "${PACKAGE_PATH}/inc/d3d12shader.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  file(INSTALL "${PACKAGE_PATH}/lib/${DXC_ARCH}/dxcompiler.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
  if(NOT DEFINED VCPKG_BUILD_TYPE)
    file(INSTALL "${PACKAGE_PATH}/lib/${DXC_ARCH}/dxcompiler.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
  endif()

  file(INSTALL
    "${PACKAGE_PATH}/bin/${DXC_ARCH}/dxcompiler.dll"
    "${PACKAGE_PATH}/bin/${DXC_ARCH}/dxil.dll"
    DESTINATION "${CURRENT_PACKAGES_DIR}/bin")

  if(NOT DEFINED VCPKG_BUILD_TYPE)
    file(INSTALL
      "${PACKAGE_PATH}/bin/${DXC_ARCH}/dxcompiler.dll"
      "${PACKAGE_PATH}/bin/${DXC_ARCH}/dxil.dll"
      DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
  endif()

  file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}/")

  file(INSTALL
    "${PACKAGE_PATH}/bin/${DXC_ARCH}/dxc.exe"
    "${PACKAGE_PATH}/bin/${DXC_ARCH}/dxcompiler.dll"
    "${PACKAGE_PATH}/bin/${DXC_ARCH}/dxil.dll"
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/")

  set(dll_name_dxc "dxcompiler.dll")
  set(dll_name_dxil "dxil.dll")
  set(dll_dir  "bin")
  set(dll_debug_dir "bin")
  set(lib_name "dxcompiler.lib")
  set(tool_path "tools/${PORT}/dxc.exe")
endif()

if (NOT DEFINED dxc_interface_link_libraries)
  set(dxc_interface_link_libraries "Microsoft::DXIL")
endif()
if (NOT DEFINED dxc_imported_soname)
  set(dxc_imported_soname "${lib_name}")
endif()

vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/tools/${PORT}")

configure_file("${CMAKE_CURRENT_LIST_DIR}/directx-dxc-config.cmake.in"
  "${CURRENT_PACKAGES_DIR}/share/${PORT}/${PORT}-config.cmake"
  @ONLY)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${LICENSE_TXT}")
