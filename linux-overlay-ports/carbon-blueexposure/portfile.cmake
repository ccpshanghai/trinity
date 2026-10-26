vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/blueexposure.git
  REF 38708579cb7dc20e7d7b994b8265c53c7b941b59
  HEAD_REF main
  # Both Android fixes live in the fork's source now (ccpshanghai/blueexposure#1) instead of in
  # PATCHES here: `long` and `int64_t` are the same type under bionic on LP64, which three traits
  # specialisations did not expect, and three interfaces blue dynamic_casts to across shared
  # objects needed key functions, because the NDK's libc++abi compares type_info by address. The
  # second is ABI-shaped -- it adds a virtual -- which is precisely why it should not be a
  # downstream patch: a consumer could otherwise build unpatched source against a patched header.
)

# Same REF as external/blueexposure. glibc LP64 has the same long/int64_t
# identity. Copy that guard over the cached extract. external/blueexposure
# is four levels above this port.
set(_CARBON_BLUEEXPOSURE_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/blueexposure")
if(EXISTS "${_CARBON_BLUEEXPOSURE_CHECKOUT}/include/BlueExposureTypeSignature.h")
  file(COPY
    "${_CARBON_BLUEEXPOSURE_CHECKOUT}/include/BlueExposureTypeSignature.h"
    DESTINATION "${SOURCE_PATH}/include")
  file(COPY
    "${_CARBON_BLUEEXPOSURE_CHECKOUT}/BlueExtractArgument.cpp"
    DESTINATION "${SOURCE_PATH}")
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