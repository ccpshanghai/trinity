vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/parser.git
  REF 66f5bd8d7e5cbcf07365afc5006cad712c1c6cca
  HEAD_REF main
)

# Same REF as external/parser. GCC does not pull <cstring> or <algorithm>
# in through the other standard headers. external/parser is four levels up.
set(_CARBON_PARSER_CHECKOUT "${CURRENT_PORT_DIR}/../../../../external/parser")
if(EXISTS "${_CARBON_PARSER_CHECKOUT}/src/pch.h")
  file(COPY "${_CARBON_PARSER_CHECKOUT}/src/pch.h" DESTINATION "${SOURCE_PATH}/src")
endif()

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
