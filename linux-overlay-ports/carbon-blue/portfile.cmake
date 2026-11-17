vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL https://github.com/ccpshanghai/blue.git
  REF 9184840129bba9425def2ea4ae6091181389ab33
  HEAD_REF mobile-exp
)

# The Linux edits are not in REF: ccpshanghai/blue 18d1f0f ("Linux desktop: BlueSysInfo and
# BlueClipboard bodies, LINUX_OS, and the GCC/case-sensitive fixes") sits on top of it. Copy
# the files it touches from external/blue (four levels above this port) over the extraction.
# yamlConfig.cmake does not match find_package(YAML) on a case-sensitive filesystem.
set(_CARBON_BLUE_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/blue")
if(EXISTS "${_CARBON_BLUE_CHECKOUT}/src/BlueSysInfoLinux.cpp")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/include/IBlueOS.h" DESTINATION "${SOURCE_PATH}/include")
  file(COPY
    "${_CARBON_BLUE_CHECKOUT}/src/BlueResFileSystemLocal.cpp"
    "${_CARBON_BLUE_CHECKOUT}/src/errormessage.cpp"
    "${_CARBON_BLUE_CHECKOUT}/src/blue.cpp"
    "${_CARBON_BLUE_CHECKOUT}/src/YamlReader.cpp"
    # Linux bodies for BlueSysInfo and BlueClipboard, and the LINUX_OS value they report.
    "${_CARBON_BLUE_CHECKOUT}/src/BlueSysInfoLinux.cpp"
    "${_CARBON_BLUE_CHECKOUT}/src/BlueClipboardLinux.cpp"
    "${_CARBON_BLUE_CHECKOUT}/src/BlueSysInfo.h"
    "${_CARBON_BLUE_CHECKOUT}/src/BlueSysInfo_Blue.cpp"
    DESTINATION "${SOURCE_PATH}/src")
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
set(BUILD_PATHS
    "${CURRENT_PACKAGES_DIR}/bin/*.pyd"
    "${CURRENT_PACKAGES_DIR}/debug/bin/*.pyd"
)
vcpkg_copy_pdbs(BUILD_PATHS ${BUILD_PATHS})
ccp_externalize_apple_debuginfo()
