vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/blueexposure.git
  REF 43826910575afeb9f3c002bcf72daa35aeb3daa3
  HEAD_REF main
  # Both Android fixes live in the fork's source now (ccpshanghai/blueexposure#1) instead of in
  # PATCHES here: `long` and `int64_t` are the same type under bionic on LP64, which three traits
  # specialisations did not expect, and three interfaces blue dynamic_casts to across shared
  # objects needed key functions, because the NDK's libc++abi compares type_info by address. The
  # second is ABI-shaped -- it adds a virtual -- which is precisely why it should not be a
  # downstream patch: a consumer could otherwise build unpatched source against a patched header.
)

# Same tree as ccpshanghai/blueexposure mobile-exp (registry 2.2.0#1). external/blueexposure's
# carbon-bind branch adds two things a GCC build needs: the long/int64_t guard for glibc LP64, and
# (temp until mobile-exp sync) the CcpLog.h/CcpAssert.h spelling of core 3.3.0's headers. The git
# tarball is cached, so copy those files over the extraction. external/blueexposure is four levels
# above this port.
set(_CARBON_BLUEEXPOSURE_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/blueexposure")
if(EXISTS "${_CARBON_BLUEEXPOSURE_CHECKOUT}/include/BlueExposureTypeSignature.h")
  foreach(_file
      include/BlueExposureTypeSignature.h
      include/BlueSmartPtr.h
      BlueExtractArgument.cpp
      BluePyWrap.cpp
      Copier.cpp
      PythonKlass.cpp)
    configure_file("${_CARBON_BLUEEXPOSURE_CHECKOUT}/${_file}" "${SOURCE_PATH}/${_file}" COPYONLY)
  endforeach()
endif()

vcpkg_cmake_configure(
  SOURCE_PATH ${SOURCE_PATH}
  OPTIONS
  -DBUILD_TESTING=OFF
  -DVCPKG_USE_HOST_TOOLS=ON
  -DVCPKG_HOST_TRIPLET=${HOST_TRIPLET}
  -DCMAKE_BUILD_TYPE=${CARBON_BUILD_TYPE}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup()
vcpkg_copy_pdbs()