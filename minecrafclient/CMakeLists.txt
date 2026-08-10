# 寄寄之家启动器 - CMake 构建配置
# WebView2 SDK 通过官方 NuGet 包 Microsoft.Web.WebView2 提供（CI 中下载并解压），
# 不再依赖 vcpkg，构建更快更可控。
cmake_minimum_required(VERSION 3.20)
project(StardustLauncher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 关闭默认 Manifest，使用自定义清单（带 DPI 感知、长路径支持）
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")

# ---------- 定位 WebView2 SDK ----------
# WEBVIEW2_SDK_DIR 应指向 NuGet 包解压后包含 "include/WebView2.h" 与
# "x64/WebView2Loader.lib" 的那一层（即包内的 build/native）。CI 会自动探测并传入，
# 本地手动构建时用 -DWEBVIEW2_SDK_DIR=.../build/native 指定。
if(NOT WEBVIEW2_SDK_DIR)
  set(WEBVIEW2_SDK_DIR "${CMAKE_SOURCE_DIR}/../WebView2SDK" CACHE PATH "WebView2 SDK (build/native) 目录")
endif()

set(WV_INCLUDE "${WEBVIEW2_SDK_DIR}/include")
set(WV_LIB "${WEBVIEW2_SDK_DIR}/x64/WebView2Loader.lib")
set(WV_DLL "${WEBVIEW2_SDK_DIR}/x64/WebView2Loader.dll")

if(NOT EXISTS "${WV_INCLUDE}/WebView2.h")
  message(FATAL_ERROR
    "未找到 WebView2 SDK 头文件（${WV_INCLUDE}/WebView2.h）。请确认 WEBVIEW2_SDK_DIR "
    "指向 NuGet 包解压后含 build/native 的目录。")
endif()

# ---------- 收集源文件 ----------
file(GLOB SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(StardustLauncher WIN32 ${SRC_FILES})

target_include_directories(StardustLauncher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${WV_INCLUDE}
)

target_link_libraries(StardustLauncher PRIVATE
    ${WV_LIB}
    shlwapi
    winhttp
    bcrypt
    version
)

# ---------- 复制资源目录到输出目录 ----------
# WebView2 以本地文件方式加载 res/launcher.html，需要保证 exe 旁边有 res/ 文件夹
add_custom_command(TARGET StardustLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/res
        $<TARGET_FILE_DIR:StardustLauncher>/res
    COMMENT "复制 res 资源到输出目录"
)

# ---------- 复制 WebView2 运行库 WebView2Loader.dll ----------
if(EXISTS "${WV_DLL}")
    add_custom_command(TARGET StardustLauncher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${WV_DLL}
            $<TARGET_FILE_DIR:StardustLauncher>/
        COMMENT "复制 WebView2Loader.dll"
    )
endif()

# 设置输出目录，方便打包
set_target_properties(StardustLauncher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
