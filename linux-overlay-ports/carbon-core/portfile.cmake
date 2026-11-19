vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL https://github.com/ccpshanghai/core.git
  REF 98fd27862fe3a275ab5bac9eb7106389289d1f42
  HEAD_REF mobile-exp
)

# REF (core 3.3.0) has upstream's Linux support and spells its files right. What is not in it
# is ccpshanghai/core a3bfd26: CcpGetProcessMemoryInfo takes the /proc/self/statm branch on
# __linux__ (upstream has it for Android only). Copy that file from external/core (four levels
# above this port) over the extraction.
set(_CARBON_CORE_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/core")
if(EXISTS "${_CARBON_CORE_CHECKOUT}/CcpMemory.cpp")
  file(COPY "${_CARBON_CORE_CHECKOUT}/CcpMemory.cpp" DESTINATION "${SOURCE_PATH}")
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
      telemetry   WITH_TELEMETRY
      docs        BUILD_DOCUMENTATION
)

vcpkg_cmake_configure(
  SOURCE_PATH ${SOURCE_PATH}
  OPTIONS
  ${FEATURE_OPTIONS}
  -DBUILD_TESTING=OFF
  -DVCPKG_USE_HOST_TOOLS=ON
  -DVCPKG_HOST_TRIPLET=${HOST_TRIPLET}
  -DCMAKE_BUILD_TYPE=${CARBON_BUILD_TYPE}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup()
vcpkg_copy_pdbs()
ccp_externalize_apple_debuginfo()
