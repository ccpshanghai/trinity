vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/pdm.git
  REF 777482cb841c30101cc4aecdad25bdf10d870cfe
  HEAD_REF master
)

# Same tree as external/pdm. The git tarball is cached, so copy the Linux
# edits over the extraction. external/pdm is four levels above this port.
set(_CARBON_PDM_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/pdm")
if(EXISTS "${_CARBON_PDM_CHECKOUT}/CMakeLists.txt")
  file(COPY "${_CARBON_PDM_CHECKOUT}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
  file(COPY "${_CARBON_PDM_CHECKOUT}/include/pdm_data.h" DESTINATION "${SOURCE_PATH}/include")
  file(COPY "${_CARBON_PDM_CHECKOUT}/src/linux/linux_data.cpp" DESTINATION "${SOURCE_PATH}/src/linux")
endif()

vcpkg_cmake_configure(
  SOURCE_PATH ${SOURCE_PATH}
  OPTIONS
  ${FEATURE_OPTIONS}
  -DBUILD_TESTING=OFF
  -DVCPKG_USE_HOST_TOOLS=ON
  -DVCPKG_HOST_TRIPLET=${HOST_TRIPLET}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup()
vcpkg_copy_pdbs()
ccp_externalize_apple_debuginfo()
