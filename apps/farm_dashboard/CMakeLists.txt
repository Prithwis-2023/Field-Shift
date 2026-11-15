##
# @file CMakeLists.txt
#/

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

aux_source_directory(${APP_PATH}/src APP_SRC)
set(APP_INC ${APP_PATH}/include)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRC}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
        ${APP_PATH}/src
    )
