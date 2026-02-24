cmake_minimum_required(VERSION 3.16)
project(ANGLESharedTextureDemo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------
# 用户配置：设置 ANGLE SDK 路径
# 例如：cmake -DANGLE_DIR="C:/angle/out/Release" ..
# -----------------------------------------------------------------------
set(ANGLE_DIR "third_party/angle" CACHE PATH "ANGLE 构建输出目录，包含 include/ 和 lib/")

if(NOT ANGLE_DIR)
    message(FATAL_ERROR
        "请设置 ANGLE_DIR，例如：\n"
        "  cmake -DANGLE_DIR=\"third_party/angle\" ..")
endif()

# 将 ANGLE_DIR 解析为相对于源码目录的绝对路径（如果用户传入相对路径）
get_filename_component(ANGLE_DIR_ABS "${ANGLE_DIR}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# -----------------------------------------------------------------------
# 源文件
# -----------------------------------------------------------------------
set(COMMON_SOURCES
    ANGLEProducer.cpp
    D3D11Consumer.cpp
)

# 单线程版本：基础示例
add_executable(${PROJECT_NAME} ${COMMON_SOURCES} main.cpp)

# 双线程流水线版本：高性能多线程架构
add_executable(PipelineDemo ${COMMON_SOURCES} main_pipeline.cpp)

# -----------------------------------------------------------------------
# Include 路径
# -----------------------------------------------------------------------
target_include_directories(${PROJECT_NAME} PRIVATE
    ${ANGLE_DIR_ABS}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_include_directories(PipelineDemo PRIVATE
    ${ANGLE_DIR_ABS}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# -----------------------------------------------------------------------
# ANGLE 库
# -----------------------------------------------------------------------
find_library(ANGLE_EGL_LIB libEGL
    PATHS ${ANGLE_DIR_ABS}/lib ${ANGLE_DIR_ABS}
    NO_DEFAULT_PATH)
find_library(ANGLE_GLES_LIB libGLESv2
    PATHS ${ANGLE_DIR_ABS}/lib ${ANGLE_DIR_ABS}
    NO_DEFAULT_PATH)

if(NOT ANGLE_EGL_LIB)
    message(FATAL_ERROR "找不到 libEGL.lib，请检查 ANGLE_DIR=${ANGLE_DIR}")
endif()
if(NOT ANGLE_GLES_LIB)
    message(FATAL_ERROR "找不到 libGLESv2.lib，请检查 ANGLE_DIR=${ANGLE_DIR}")
endif()

# -----------------------------------------------------------------------
# 系统库
# -----------------------------------------------------------------------
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${ANGLE_EGL_LIB}
    ${ANGLE_GLES_LIB}
    d3d11.lib
    dxgi.lib
    dxguid.lib
)

target_link_libraries(PipelineDemo PRIVATE
    ${ANGLE_EGL_LIB}
    ${ANGLE_GLES_LIB}
    d3d11.lib
    dxgi.lib
    dxguid.lib
)

# -----------------------------------------------------------------------
# 编译选项
# -----------------------------------------------------------------------
set(COMMON_COMPILE_DEFS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    $<$<CONFIG:Debug>:_DEBUG>
)

set(COMMON_COMPILE_OPTS
    /W4
    /WX-
    /wd4100
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-finput-charset=UTF-8>
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fexec-charset=UTF-8>
)

target_compile_definitions(${PROJECT_NAME} PRIVATE ${COMMON_COMPILE_DEFS})
target_compile_options(${PROJECT_NAME} PRIVATE ${COMMON_COMPILE_OPTS})

target_compile_definitions(PipelineDemo PRIVATE ${COMMON_COMPILE_DEFS})
target_compile_options(PipelineDemo PRIVATE ${COMMON_COMPILE_OPTS})

# -----------------------------------------------------------------------
# 拷贝 ANGLE DLL 到输出目录（便于直接运行）
# -----------------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ANGLE_DIR_ABS}/bin/libEGL.dll"
        "${ANGLE_DIR_ABS}/bin/libGLESv2.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMENT "Copying ANGLE DLLs to output directory"
)

add_custom_command(TARGET PipelineDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ANGLE_DIR_ABS}/bin/libEGL.dll"
        "${ANGLE_DIR_ABS}/bin/libGLESv2.dll"
        $<TARGET_FILE_DIR:PipelineDemo>
    COMMENT "Copying ANGLE DLLs to output directory"
)

message(STATUS "ANGLE_DIR       = ${ANGLE_DIR} (as given)")
message(STATUS "ANGLE_DIR_ABS   = ${ANGLE_DIR_ABS}")
message(STATUS "ANGLE_EGL_LIB   = ${ANGLE_EGL_LIB}")
message(STATUS "ANGLE_GLES_LIB  = ${ANGLE_GLES_LIB}")
