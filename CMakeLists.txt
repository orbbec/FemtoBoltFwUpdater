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
    else(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(CURRENT_BUILD_COMPILER win_x86)
    endif()
else(UNIX)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CURRENT_BUILD_COMPILER linux_x64)
    else(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(CURRENT_BUILD_COMPILER linux_x86)
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${THIRD_PARTY_DIR}/OrbbecSDK/include
    ${THIRD_PARTY_DIR}/usbscsicmd
)

# app source files
target_sources(${PROJECT_NAME} PRIVATE ${SRC_DIR}/main.cpp)

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