# 寄寄之家启动器 - CMake 构建配置
# WebView2 SDK 通过官方 NuGet 包 Microsoft.Web.WebView2 提供（CI 中下载并解压），
# 不再依赖 vcpkg，构建更快更可控。
cmake_minimum_required(VERSION 3.20)
# RC 语言用于把内嵌 HTML 以 RT_HTML 资源编译进 exe（网页不再放程序目录）
project(StardustLauncher CXX RC)

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

# ---------- 生成内嵌版 HTML（背景图 base64 注入）----------
# WebView2 用 NavigateToString 渲染内嵌 HTML，无法引用外部文件，
# 故由 tools/embed_html.py 在构建时把 background.png 转 data URI 内联进 HTML。
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(EMBEDDED_HTML "${CMAKE_CURRENT_BINARY_DIR}/embedded/launcher.html")
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_html.py"
            --html "${CMAKE_CURRENT_SOURCE_DIR}/res/launcher.html"
            --png  "${CMAKE_CURRENT_SOURCE_DIR}/res/background.png"
            --out  "${EMBEDDED_HTML}"
    RESULT_VARIABLE EMBED_RC
    COMMENT "内嵌背景图到 HTML（base64）"
)
if(NOT EMBED_RC EQUAL 0)
    message(FATAL_ERROR "生成内嵌版 HTML 失败（embed_html.py 退出码 ${EMBED_RC}）")
endif()

# 生成 RC：把内嵌版 HTML 以 RT_HTML 资源编译进 exe
# 用原生反斜杠路径，并把每个反斜杠转义成 \\，避免 RC 编译器把 \a \t 等当转义序列
file(TO_NATIVE_PATH "${EMBEDDED_HTML}" EMBEDDED_HTML_NATIVE)
string(REPLACE "\\" "\\\\" EMBEDDED_HTML "${EMBEDDED_HTML_NATIVE}")
configure_file(res/html_rc.in "${CMAKE_CURRENT_BINARY_DIR}/html_rc.rc" @ONLY)

# ---------- 收集源文件 ----------
file(GLOB SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(StardustLauncher WIN32 ${SRC_FILES}
    "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest"
    "${CMAKE_CURRENT_BINARY_DIR}/html_rc.rc"
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

# ---------- 资源已内嵌进 exe，不再复制 res\ 到输出目录 ----------
# 网页（launcher.html）与背景图已编译进 exe（RT_HTML 资源），程序目录不再需要 res\。

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
