vcpkg_download_distfile(ARCHIVE
  URLS "https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-4.0.0.tar.gz"
  FILENAME "mpdecimal-4.0.0.tar.gz"
  SHA512 7610ac53ac79f7a8a33fa7a3e61515810444ec73ebca859df7a9ddc18e96b990c99323172810c9cc7f6d6e1502c0be308cd443d6c2d5d0c871648e4842e05d59
)

vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME mpdecimal)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYRIGHT.txt")
