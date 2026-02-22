cmake_minimum_required(VERSION 3.16)
project(ANGLESharedTextureDemo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------
# 用户配置：设置 ANGLE SDK 路径
# 例如：cmake -DANGLE_DIR="C:/angle/out/Release" ..
# -----------------------------------------------------------------------
set(ANGLE_DIR "third_party/angle" CACHE PATH "ANGLE 构建输出目录，包含 include/ 和 lib/")
#set(ANGLE_DIR "" CACHE PATH "ANGLE 构建输出目录，包含 include/ 和 lib/")

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
set(SOURCES
    main.cpp
    ANGLEProducer.cpp
    D3D11Consumer.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

# -----------------------------------------------------------------------
# Include 路径
# -----------------------------------------------------------------------
target_include_directories(${PROJECT_NAME} PRIVATE
    ${ANGLE_DIR_ABS}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
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

# -----------------------------------------------------------------------
# 编译选项
# -----------------------------------------------------------------------
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    $<$<CONFIG:Debug>:_DEBUG>
)

target_compile_options(${PROJECT_NAME} PRIVATE
    /W4       # 高警告级别
    /WX-      # 警告不视为错误（EGL 头文件可能有警告）
    /wd4100   # 未使用参数（EGL/GL 头文件）
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-finput-charset=UTF-8>
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fexec-charset=UTF-8>
)


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

message(STATUS "ANGLE_DIR       = ${ANGLE_DIR} (as given)")
message(STATUS "ANGLE_DIR_ABS   = ${ANGLE_DIR_ABS}")
message(STATUS "ANGLE_EGL_LIB   = ${ANGLE_EGL_LIB}")
message(STATUS "ANGLE_GLES_LIB  = ${ANGLE_GLES_LIB}")
