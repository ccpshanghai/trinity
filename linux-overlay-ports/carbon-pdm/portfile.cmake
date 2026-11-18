vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/pdm.git
  REF 13d210d4fd57674b268828524aa87beb2e7c49b5
  HEAD_REF master
)

# Same tree as ccpshanghai/pdm mobile-exp (registry 2.3.0#1), which has upstream's Linux backend.
# external/pdm's carbon-bind branch only adds the CMakeLists filter that keeps ios_data.mm out of
# g++ builds. The git tarball is cached, so copy that file over the extraction. external/pdm is
# four levels above this port.
set(_CARBON_PDM_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/pdm")
if(EXISTS "${_CARBON_PDM_CHECKOUT}/CMakeLists.txt")
  file(COPY "${_CARBON_PDM_CHECKOUT}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
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
