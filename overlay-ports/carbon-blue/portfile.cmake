# Overlay of the registry's carbon-blue 6.0.0#4 carrying one patch until ccpshanghai/blue takes
# it and the registry moves its REF: BlueResFileSystemLocal::ResolvePathW retries a res path
# lowercased on case-sensitive filesystems. The engine asks for "res:/Graphics/Effect/Managed/..."
# (92 such spellings in trinity) and data for "res:/Texture/Global/..." while the staged tree is
# lowercase; on Android's ext4 the literal spelling fails (M6, 2026-09-10). The patch is
# external/blue's working-tree diff.
vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:ccpshanghai/blue.git
  REF 9de531fbc61b20e540d8496c1ff22a74d623a559
  HEAD_REF main
  PATCHES
    android-res-path-case.patch
)

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
