# 寄寄之家启动器 - CMake 构建配置
# 依赖通过 vcpkg 管理（见 vcpkg.json，需 microsoft-webview2）
cmake_minimum_required(VERSION 3.20)
project(StardustLauncher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 关闭默认 Manifest，使用自定义清单（带 DPI 感知、长路径支持）
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")

# ---------- 找到 WebView2 SDK（来自 vcpkg 的 microsoft-webview2） ----------
find_package(WebView2 CONFIG REQUIRED)

# ---------- 收集源文件 ----------
file(GLOB SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(StardustLauncher WIN32 ${SRC_FILES})

target_include_directories(StardustLauncher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(StardustLauncher PRIVATE
    WebView2::WebView2
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

# ---------- 复制 WebView2 运行库（若有 DLL） ----------
# vcpkg 的 microsoft-webview2 提供 WebView2Loader.dll，运行时需要它
get_target_property(_wv_dll WebView2::WebView2 IMPORTED_LOCATION)
if(_wv_dll AND EXISTS _wv_dll)
    add_custom_command(TARGET StardustLauncher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_wv_dll}
            $<TARGET_FILE_DIR:StardustLauncher>/
        COMMENT "复制 WebView2Loader.dll"
    )
endif()

# 设置输出目录，方便打包
set_target_properties(StardustLauncher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
