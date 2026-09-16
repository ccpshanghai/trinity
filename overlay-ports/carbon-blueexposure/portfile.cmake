vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL git@github.com:carbonengine/blueexposure.git
  REF c855175f5f261a301a12b63c0337e8eb8d92b081
  HEAD_REF main
  # 38 redefinition errors on arm64-android, all from one cause: three places specialise for
  # both int64_t and long, which are the same type under bionic on LP64. The guards assumed
  # non-MSVC means long is distinct from int64_t -- true on Apple, false on Linux. Only the
  # 64-bit Android case is excluded; MSVC, Apple and 32-bit Android keep their existing traits.
  # Three interfaces blue dynamic_casts to from blue.so on objects other libraries create --
  # IList, IPythonMethods, IPythonNumeric -- get a key function and default visibility, so
  # their vtable and typeinfo exist once (in blue) instead of privately in every .so. Without
  # it the NDK's libc++abi (type_info compared by address) makes every such cast nullptr,
  # and the app's first run on Android died in IList_Thunk::Pyappend. See BLUE_INTERFACE_ANCHOR
  # in the patched BlueTypes.h. Windows and Apple are unaffected by the cast and unchanged in
  # behaviour: one trailing do-nothing virtual per interface.
  PATCHES
    android-lp64-long-is-int64.patch
    android-interface-typeinfo-anchors.patch
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
vcpkg_copy_pdbs()