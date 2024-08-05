file(GLOB_RECURSE _cpp_files "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE _c_files "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
file(GLOB_RECURSE _head_hpp_files "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
file(GLOB_RECURSE _head_files "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

foreach(_file IN ITEMS ${_cpp_files})
    target_sources(${PROJECT_NAME} PRIVATE "${_file}")
endforeach()

foreach(_file IN ITEMS ${_c_files})
    target_sources(${PROJECT_NAME} PRIVATE "${_file}")
endforeach()

foreach(_file IN ITEMS ${_head_files})
    target_sources(${PROJECT_NAME} PRIVATE "${_file}")
endforeach()

foreach(_file IN ITEMS ${_head_hpp_files})
    target_sources(${PROJECT_NAME} PRIVATE "${_file}")
endforeach()