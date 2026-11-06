vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/blue.git
  REF 8dcd8cae279def008016bdb947ffc500d332e38e
  HEAD_REF main
)

# Same REF as external/blue. yamlConfig.cmake does not match find_package(YAML)
# on a case-sensitive filesystem. external/blue is four levels above this port.
set(_CARBON_BLUE_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/blue")
if(EXISTS "${_CARBON_BLUE_CHECKOUT}/CMakeLists.txt")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/NetworkLogger.h" DESTINATION "${SOURCE_PATH}/src")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/BlueResFileSystemLocal.cpp" DESTINATION "${SOURCE_PATH}/src")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/errormessage.cpp" DESTINATION "${SOURCE_PATH}/src")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/blue.cpp" DESTINATION "${SOURCE_PATH}/src")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/YamlReader.cpp" DESTINATION "${SOURCE_PATH}/src")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/include/IBlueOS.h" DESTINATION "${SOURCE_PATH}/include")
  # Linux bodies for BlueSysInfo and BlueClipboard, and the LINUX_OS value they report.
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/BlueSysInfoLinux.cpp" DESTINATION "${SOURCE_PATH}/src")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/BlueClipboardLinux.cpp" DESTINATION "${SOURCE_PATH}/src")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/BlueSysInfo.h" DESTINATION "${SOURCE_PATH}/src")
  file(COPY "${_CARBON_BLUE_CHECKOUT}/src/BlueSysInfo_Blue.cpp" DESTINATION "${SOURCE_PATH}/src")
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
