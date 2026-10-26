vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/core.git
  REF c8e50e0851897cdea0f2669a863f3616eb9356ab
  HEAD_REF mobile-exp
)

# The git tree spells these Ccp* while CMakeLists.txt spells CCP*. Windows finds
# them because its filesystem is case-insensitive. Linux does not.
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt" "CCPDefines.cpp" "CcpDefines.cpp")
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt" "CCPMemoryTrackerMutex.h" "CcpMemoryTrackerMutex.h")

# Same REF as external/core. The git tarball is cached, so copy the checkout
# over the extraction. external/core is four levels above this port.
set(_CARBON_CORE_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/core")
if(EXISTS "${_CARBON_CORE_CHECKOUT}/include/CCPLog.h")
  file(COPY
    "${_CARBON_CORE_CHECKOUT}/include/CCPLog.h"
    "${_CARBON_CORE_CHECKOUT}/include/CcpThread.h"
    DESTINATION "${SOURCE_PATH}/include")
  file(COPY
    "${_CARBON_CORE_CHECKOUT}/CcpThread.cpp"
    "${_CARBON_CORE_CHECKOUT}/CCPAssert.cpp"
    "${_CARBON_CORE_CHECKOUT}/CCPMemory.cpp"
    "${_CARBON_CORE_CHECKOUT}/CcpFileUtils.cpp"
    "${_CARBON_CORE_CHECKOUT}/CCPCallstack.cpp"
    "${_CARBON_CORE_CHECKOUT}/CcpTime.cpp"
    "${_CARBON_CORE_CHECKOUT}/CcpSemaphore.cpp"
    "${_CARBON_CORE_CHECKOUT}/CcpStatistics.cpp"
    DESTINATION "${SOURCE_PATH}")
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
      documentation BUILD_DOCUMENTATION
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
