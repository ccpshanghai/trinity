set(VCPKG_POLICY_DLLS_IN_STATIC_LIBRARY enabled)

set(DIRECTX_DXC_TAG v1.9.2602)
set(DIRECTX_DXC_VERSION 2026_02_20)

if (NOT VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
   message(STATUS "Note: ${PORT} always requires dynamic library linkage at runtime.")
endif()

if (VCPKG_TARGET_IS_OSX)
    # Nothing to download: upstream ${DIRECTX_DXC_TAG} publishes Windows and Linux
    # archives only. This branch installs a dxc built from source on this machine
    # instead -- see vcpkg.json's $comment for why the overlay exists at all.
    set(DXC_OSX_ROOT "$ENV{DXC_ROOT}")
    if (DXC_OSX_ROOT STREQUAL "")
        set(DXC_OSX_ROOT "$ENV{HOME}/Workspace/carbon/carbon-toolchain/dxc")
    endif()

    # The headers do not live under the install prefix: `ninja dxc` installs bin/ and
    # lib/ and nothing else, and the release archives this port unpacks on the other two
    # platforms have no macOS counterpart. So they are copied out of the SOURCE tree the
    # dylib was built from. DXC_SOURCE_ROOT overrides; the default is the layout
    # docs/HANDOFF-m6-macbook.md's dxc step produces.
    set(DXC_OSX_SRC "$ENV{DXC_SOURCE_ROOT}")
    if (DXC_OSX_SRC STREQUAL "")
        get_filename_component(_dxc_toolchain "${DXC_OSX_ROOT}" DIRECTORY)
        set(DXC_OSX_SRC "${_dxc_toolchain}/src/DirectXShaderCompiler")
    endif()
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
  # their own directory, the way they do inside dxc's own build.
  if (NOT EXISTS "${DXC_OSX_ROOT}/bin/dxc" OR NOT EXISTS "${DXC_OSX_ROOT}/lib/libdxcompiler.dylib")
    message(FATAL_ERROR
      "${PORT}: no macOS dxc found under '${DXC_OSX_ROOT}'.\n"
      "Upstream ${DIRECTX_DXC_TAG} publishes Windows and Linux binaries only, so this "
      "overlay port installs a dxc built from source on this machine. Both of these "
      "files must exist:\n"
      "    ${DXC_OSX_ROOT}/bin/dxc\n"
      "    ${DXC_OSX_ROOT}/lib/libdxcompiler.dylib\n"
      "Point the DXC_ROOT environment variable at the prefix of such a build, or build "
      "${DIRECTX_DXC_TAG} from https://github.com/microsoft/DirectXShaderCompiler and "
      "install it there.")
  endif()

  foreach(_header
      "${DXC_OSX_SRC}/include/dxc/dxcapi.h"
      "${DXC_OSX_SRC}/include/dxc/WinAdapter.h"
      "${DXC_OSX_SRC}/external/DirectX-Headers/include/directx/d3d12shader.h"
      "${DXC_OSX_SRC}/external/DirectX-Headers/include/wsl/stubs/rpc.h")
    if (NOT EXISTS "${_header}")
      message(FATAL_ERROR
        "${PORT}: '${_header}' is missing.\n"
        "The macOS branch of this port copies dxc's headers out of the SOURCE tree the "
        "dylib was built from, because a `ninja dxc` install prefix carries none. Keep "
        "the checkout (with its submodules) beside the install prefix, or point "
        "DXC_SOURCE_ROOT at it. Looked under:\n"
        "    ${DXC_OSX_SRC}")
    endif()
  endforeach()

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
    "${DXC_OSX_SRC}/external/DirectX-Headers/include/directx/d3d12shader.h"
    "${DXC_OSX_SRC}/external/DirectX-Headers/include/directx/d3dcommon.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  # The four MIDL stubs d3dcommon.h includes unconditionally, plus winapifamily.h.
  # Each is a handful of lines; dxc puts the same directory on its own include path.
  file(INSTALL
    "${DXC_OSX_SRC}/external/DirectX-Headers/include/wsl/stubs/rpc.h"
    "${DXC_OSX_SRC}/external/DirectX-Headers/include/wsl/stubs/rpcndr.h"
    "${DXC_OSX_SRC}/external/DirectX-Headers/include/wsl/stubs/OAIdl.h"
    "${DXC_OSX_SRC}/external/DirectX-Headers/include/wsl/stubs/OCIdl.h"
    "${DXC_OSX_SRC}/external/DirectX-Headers/include/wsl/stubs/winapifamily.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}")

  # The driver's rpath is @executable_path/../lib, so installed as
  # tools/directx-dxc/dxc it resolves its dylib at tools/lib/ -- which is why the
  # dylib is copied there as well as into lib/. Copying bytes is deliberate: the
  # alternative is to put the dylib beside the binary and add @executable_path to
  # the copy with `install_name_tool -add_rpath`, and rewriting an arm64 Mach-O's
  # load commands invalidates its code signature, after which the kernel SIGKILLs
  # the binary ("Killed: 9") instead of merely warning. Nothing here modifies the
  # build under ${DXC_OSX_ROOT}.
  file(INSTALL
    "${DXC_OSX_ROOT}/lib/libdxcompiler.dylib"
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/lib")

  file(INSTALL
    "${DXC_OSX_ROOT}/lib/libdxcompiler.dylib"
    DESTINATION "${CURRENT_PACKAGES_DIR}/lib")

  if(NOT DEFINED VCPKG_BUILD_TYPE)
    file(INSTALL
      "${DXC_OSX_ROOT}/lib/libdxcompiler.dylib"
      DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
  endif()

  file(INSTALL
    "${DXC_OSX_ROOT}/bin/dxc"
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/"
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
  file(INSTALL
    "${PACKAGE_PATH}/include/dxc/dxcapi.h"
    "${PACKAGE_PATH}/include/dxc/dxcerrors.h"
    "${PACKAGE_PATH}/include/dxc/dxcisense.h"
    "${PACKAGE_PATH}/include/dxc/WinAdapter.h"
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
