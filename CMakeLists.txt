cmake_minimum_required(VERSION 3.1.15)
set(CMAKE_CXX_STANDARD 11)
project(FemtoBoltUpgrader)

SET(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/install/)

if(UNIX)
    SET(CMAKE_SKIP_BUILD_RPATH FALSE)
    SET(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
    SET(CMAKE_SKIP_INSTALL_RPATH FALSE)
    SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

    SET(CMAKE_INSTALL_RPATH "$ORIGIN:${CMAKE_INSTALL_RPATH}")
endif()

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(${PROJECT_NAME}
    ${SRC_DIR}/utils.hpp
    ${SRC_DIR}/main.cpp
)
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CURRENT_BUILD_COMPILER win_x64)
    else()
        set(CURRENT_BUILD_COMPILER win_x86)
    endif()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(CURRENT_BUILD_COMPILER arm_64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CURRENT_BUILD_COMPILER linux_x64)
    else()
        set(CURRENT_BUILD_COMPILER linux_x86)
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${THIRD_PARTY_DIR}/OrbbecSDK/include
    ${THIRD_PARTY_DIR}/usbscsicmd
)

target_link_directories(${PROJECT_NAME} PRIVATE
    ${THIRD_PARTY_DIR}/OrbbecSDK/lib/${CURRENT_BUILD_COMPILER}/
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME}
        OrbbecSDK
    )
else(UNIX)
    target_link_libraries(${PROJECT_NAME}
        OrbbecSDK
        pthread
    )
endif()

install(TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION ./
	ARCHIVE DESTINATION lib)

add_subdirectory(${THIRD_PARTY_DIR}/usbscsicmd/)

# ------------------------------------------------------------------------------
# Copy runtime dependencies after build
# ------------------------------------------------------------------------------
if(WIN32)
    set(RELEASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Release/Windows)
    set(RUNTIME_FILES
        ${RELEASE_DIR}/OrbbecSDK.dll
        ${RELEASE_DIR}/depthengine_2_0.dll
        ${RELEASE_DIR}/AitUVCExtApi.dll
        ${RELEASE_DIR}/USBDownloadTool.exe
    )
    foreach(f ${RUNTIME_FILES})
        if(EXISTS ${f})
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${f} $<TARGET_FILE_DIR:${PROJECT_NAME}>
            )
        else()
            message(WARNING "Missing runtime dependency: ${f}")
        endif()
    endforeach()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(RELEASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Release/Arm64)
    else()
        set(RELEASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Release/Linux)
    endif()
    set(RUNTIME_FILES
        ${RELEASE_DIR}/libOrbbecSDK.so
        ${RELEASE_DIR}/libOrbbecSDK.so.1.9
        ${RELEASE_DIR}/libOrbbecSDK.so.1.9.5
        ${RELEASE_DIR}/libdepthengine.so
        ${RELEASE_DIR}/libdepthengine.so.2.0
        ${RELEASE_DIR}/usbdownload
    )
    foreach(f ${RUNTIME_FILES})
        if(EXISTS ${f})
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${f} $<TARGET_FILE_DIR:${PROJECT_NAME}>
            )
        else()
            message(WARNING "Missing runtime dependency: ${f}")
        endif()
    endforeach()
endif()
