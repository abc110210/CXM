# 寄寄之家启动器 - CMake 构建配置
# WebView2 SDK 通过官方 NuGet 包 Microsoft.Web.WebView2 提供（CI 中下载并解压），
# 不再依赖 vcpkg，构建更快更可控。
cmake_minimum_required(VERSION 3.20)
project(StardustLauncher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 通过 res/app.manifest 嵌入自定义清单（DPI PerMonitorV2、长路径、v6 通用控件）

# ---------- 定位 WebView2 SDK ----------
# WEBVIEW2_SDK_DIR 应指向 NuGet 包解压后包含 "include/WebView2.h" 与
# "x64/WebView2Loader*.lib" 的那一层（即包内的 build/native）。CI 会自动探测并传入，
# 本地手动构建时用 -DWEBVIEW2_SDK_DIR=.../build/native 指定。
# 注意：新版 WebView2 SDK（>=1.0.2xxx）把导入库改名为 WebView2Loader.dll.lib，
# 旧版叫 WebView2Loader.lib；故导入库路径由 CI 通过 WEBVIEW2_LOADER_LIB 显式传入，
# 缺省才回退到旧命名，避免写死文件名导致 LNK1181。
if(NOT WEBVIEW2_SDK_DIR)
  set(WEBVIEW2_SDK_DIR "${CMAKE_SOURCE_DIR}/../WebView2SDK" CACHE PATH "WebView2 SDK (build/native) 目录")
endif()
if(NOT WEBVIEW2_LOADER_LIB)
  set(WEBVIEW2_LOADER_LIB "${WEBVIEW2_SDK_DIR}/x64/WebView2Loader.lib" CACHE FILEPATH "WebView2Loader 导入库路径")
endif()

set(WV_INCLUDE "${WEBVIEW2_SDK_DIR}/include")
set(WV_LIB "${WEBVIEW2_LOADER_LIB}")
set(WV_DLL "${WEBVIEW2_SDK_DIR}/x64/WebView2Loader.dll")

if(NOT EXISTS "${WV_INCLUDE}/WebView2.h")
  message(FATAL_ERROR
    "未找到 WebView2 SDK 头文件（${WV_INCLUDE}/WebView2.h）。请确认 WEBVIEW2_SDK_DIR "
    "指向 NuGet 包解压后含 build/native 的目录。")
endif()

# 导入库存在性检查（文件名随 SDK 版本变化，必须由 CI 正确传入 WEBVIEW2_LOADER_LIB）
if(NOT EXISTS "${WV_LIB}")
  message(FATAL_ERROR
    "未找到 WebView2Loader 导入库（${WV_LIB}）。新版 SDK 命名为 WebView2Loader.dll.lib，"
    "旧版为 WebView2Loader.lib；请在 CI 中通过 -DWEBVIEW2_LOADER_LIB 传入真实路径。")
endif()

# ---------- 收集源文件 ----------
file(GLOB SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(StardustLauncher WIN32 ${SRC_FILES}
    "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest"
)

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
    dwmapi
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
