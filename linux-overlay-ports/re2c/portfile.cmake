vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO skvadrik/re2c
    REF refs/tags/2.2
    SHA512 59121c599a36b753dd4f306fbf1d9b2a7fadaa8baa88cf0b6b144716fd75ce2884ab45f5a42d8ae5f42a00ceb8c0d9fc5ff9a60125efc58f21231e8c021993bb
    HEAD_REF master
)

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()

# re2c is only ever consumed as a host tool, so the binaries have to land in
# tools/re2c/ -- vcpkg.cmake globs tools/* onto CMAKE_PROGRAM_PATH, and nothing puts
# bin/ there. Without this, find_program(Re2c re2c) fails for every consumer whose host
# triplet differs from its target, which is every mobile build. It succeeds when host and
# target match only because the target tree's bin/ is already on CMAKE_PREFIX_PATH, which
# is why this went unnoticed on desktop.
vcpkg_copy_tools(TOOL_NAMES re2c re2go AUTO_CLEAN)

vcpkg_cmake_config_fixup()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")