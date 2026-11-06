vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  # M3 = imageio is the seventh fork (spec §2.5): the ASTC/KTX2 work lives on
  # ccpshanghai/imageio. The m3-ktx2-astc branch squash-merged into the fork's mobile-exp
  # as caaf391e, so the REF here is the merged commit rather than the topic branch's tip --
  # a pin to a retired branch breaks the day the branch goes (the same trap the trinity pin
  # fix walked one level up). Landed bottom-up: fork -> port URL/REF (here) -> registry PR
  # -> trinity baseline bump.
  URL git@github.com:ccpshanghai/imageio.git
  REF caaf391e0055689b5e0c496fb66137f815e748ae
  HEAD_REF mobile-exp
  PATCHES
    cstring.patch
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