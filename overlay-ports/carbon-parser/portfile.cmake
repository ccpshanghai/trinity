# Overlay of the registry's carbon-parser 1.0.2#1, carrying one patch until ccpshanghai/parser
# takes it and the registry moves its REF: codebuilder.cpp's BitScan had no return on Android
# (Windows and Apple branches only), which is undefined behaviour clang honoured by deleting the
# exit of BuildCode's argument loop -- every expression with a function call then walked into
# BuildCode( nullptr ) (M6 Plan 1 Task 3b crash #2, m6-crash2-shippreview-20260910.txt). The
# patch is external/parser's working-tree diff; -Werror=return-type rides with it.
vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/parser.git
  REF 1ad0b204abc02c61c21eb167d3e8743f1e1fce38
  HEAD_REF main
  PATCHES
    android-bitscan-return.patch
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
