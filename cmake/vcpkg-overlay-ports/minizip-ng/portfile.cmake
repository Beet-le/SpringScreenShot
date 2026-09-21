if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
    # minizip is linked into the Snow Shot executable directly and must use the
    # same CRT as that executable: /MT with the static Qt kit, /MD with the
    # dynamic Qt kit. The triplet's VCPKG_CRT_LINKAGE selects the matching
    # runtime.
    if(VCPKG_CRT_LINKAGE STREQUAL "static")
        set(SNOW_MINIZIP_MSVC_RUNTIME_RELEASE MultiThreaded)
        set(SNOW_MINIZIP_MSVC_RUNTIME_DEBUG MultiThreadedDebug)
    else()
        set(SNOW_MINIZIP_MSVC_RUNTIME_RELEASE MultiThreadedDLL)
        set(SNOW_MINIZIP_MSVC_RUNTIME_DEBUG MultiThreadedDebugDLL)
    endif()
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zlib-ng/minizip-ng
    REF "${VERSION}"
    SHA512 9ea5dde14acd2f7d1efd0e38b11017b679d3aaabac61552f9c5f4c7f45f2563543e0fbb2d74429c6b1b9c37d8728ebc4f1cf0efad5f71807c11bb8a2a681a556
    HEAD_REF master
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES zlib MZ_ZLIB
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DMZ_FETCH_LIBS=OFF
        -DMZ_LIB_SUFFIX=-ng
        -DMZ_ICONV=OFF
        -DMZ_COMPAT=OFF
    OPTIONS_RELEASE
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${SNOW_MINIZIP_MSVC_RUNTIME_RELEASE}
    OPTIONS_DEBUG
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${SNOW_MINIZIP_MSVC_RUNTIME_DEBUG}
)
vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/minizip-ng)
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share" "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
