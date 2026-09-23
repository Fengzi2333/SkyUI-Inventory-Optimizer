vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO alandtse/CommonLibSSE-NG
        REF 70c1acd5261210982bd52f6d4468a082fe04d798
        SHA512  4b13f15f05c220f9754260086efa1688566ad8c60c9f8002110f342976d0dc653deffd39745b8398360092e21b83c40040c6a88d02aec2c1ee1eef98396e53a2
        HEAD_REF main
)

vcpkg_from_github(
    OUT_SOURCE_PATH SUBMODULE_PATH
    REPO ValveSoftware/openvr
    #commit is defined in alandtse/CommonLibSSE-NG/extern/openvr submodule
    REF 60eb187801956ad277f1cae6680e3a410ee0873b
    SHA512 bb85b4705e7095ac65df9969112b2df8930cee7917cc5f14231c5a0ffeed7a73ffa60727fd32f8786a403656f95a3ec0f80bf3ceabc5b8ede964aefb920bc718
)

#move openvr to extern
file(REMOVE_RECURSE "${SOURCE_PATH}/extern/openvr")
file(COPY "${SUBMODULE_PATH}/" DESTINATION "${SOURCE_PATH}/extern/openvr")

vcpkg_configure_cmake(
        SOURCE_PATH "${SOURCE_PATH}"
        PREFER_NINJA
        OPTIONS -DBUILD_TESTS=off -DSKSE_SUPPORT_XBYAK=on
)

vcpkg_install_cmake()
vcpkg_cmake_config_fixup(PACKAGE_NAME CommonLibSSE CONFIG_PATH lib/cmake)
vcpkg_copy_pdbs()

file(GLOB CMAKE_CONFIGS "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE/*.cmake")
file(INSTALL ${CMAKE_CONFIGS} DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE")
file(INSTALL "${SOURCE_PATH}/cmake/CommonLibSSE.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE")

#fix directxtk with portfile
set(CONFIG_FILE
    "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSEConfig.cmake"
)
file(READ "${CONFIG_FILE}" CONFIG_CONTENT)
file(WRITE "${CONFIG_FILE}"
    "include(CMakeFindDependencyMacro)\nfind_dependency(directxtk CONFIG)\n${CONFIG_CONTENT}"
)

#copy openvr headers
file(INSTALL
    "${SOURCE_PATH}/extern/openvr/headers/"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE")