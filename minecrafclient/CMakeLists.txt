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

# 运行时 DLL 存在性检查（要嵌入 exe 资源段，RC 编译期必须能读到）
if(NOT EXISTS "${WV_DLL}")
  message(FATAL_ERROR
    "未找到 WebView2Loader 运行时 DLL（${WV_DLL}）。该文件会被嵌入 exe 资源段，"
    "编译期必须存在；请确认 NuGet 包 x64 目录下有 WebView2Loader.dll。")
endif()

# ---------- 生成内嵌版 HTML（背景图改为占位符）----------
# WebView2 用 NavigateToString 渲染内嵌 HTML，对超大 HTML 会静默失败（白屏）。
# 故 HTML 本体始终保持 < 50KB：背景图另作 RCDATA 资源，运行时由 C++ 注入。
# 同样把 WebView2Loader.dll 也作为 RCDATA 嵌入，运行时释放到 %TEMP% 后
# LoadLibrary，程序目录不再需要任何外部 dll。
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(EMBEDDED_HTML "${CMAKE_CURRENT_BINARY_DIR}/embedded/launcher.html")
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_html.py"
            --html "${CMAKE_CURRENT_SOURCE_DIR}/res/launcher.html"
            --out  "${EMBEDDED_HTML}"
    RESULT_VARIABLE EMBED_RC
)
if(NOT EMBED_RC EQUAL 0)
    message(FATAL_ERROR "生成内嵌版 HTML 失败（embed_html.py 退出码 ${EMBED_RC}）")
endif()

# 生成 RC：HTML / 背景图 / WebView2Loader.dll 全部作为资源编译进 exe
# 用原生反斜杠路径，并把每个反斜杠转义成 \\，避免 RC 编译器把 \a \t 等当转义序列
function(escape_rc_path input out)
    file(TO_NATIVE_PATH "${input}" _nat)
    string(REPLACE "\\" "\\\\" _esc "${_nat}")
    set(${out} "${_esc}" PARENT_SCOPE)
endfunction()

escape_rc_path("${EMBEDDED_HTML}" EMBEDDED_HTML_RC)
escape_rc_path("${CMAKE_CURRENT_SOURCE_DIR}/res/background.png" EMBEDDED_PNG_RC)
escape_rc_path("${WV_DLL}" EMBEDDED_DLL_RC)
escape_rc_path("${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico" EMBEDDED_ICO_RC)
configure_file(res/html_rc.in "${CMAKE_CURRENT_BINARY_DIR}/html_rc.rc" @ONLY)

# ---------- 收集源文件 ----------
file(GLOB SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(StardustLauncher WIN32 ${SRC_FILES}
    "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest"
    "${CMAKE_CURRENT_BINARY_DIR}/html_rc.rc"
)

# Windows 工程标配：走宽字符 API（否则 MAKEINTRESOURCE 展开成 ANSI 版 LPSTR，
# 传给 FindResourceW 等 W 函数会 C2664）
target_compile_definitions(StardustLauncher PRIVATE UNICODE _UNICODE)

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
    delayimp   # MSVC 延迟加载辅助库（/DELAYLOAD:WebView2Loader.dll 需要）
)

# 关键：延迟加载 WebView2Loader.dll
#   - 默认链接下，Windows loader 在 exe 启动时就自动 LoadLibrary("WebView2Loader.dll")，
#     程序目录没 dll 就弹"找不到 dll"错误，且在我们 EnsureWebView2LoaderDll() 之前。
#   - /DELAYLOAD 后 dll 只在第一次调用 WebView2 API 时才加载，给我们时间先释放 dll 到 %TEMP%。
#   - 失败兜底见 webview2_host.cpp 注册的 __pfnDliFailureHook2 钩子。
target_link_options(StardustLauncher PRIVATE /DELAYLOAD:WebView2Loader.dll)

# ---------- 资源已全部内嵌进 exe，程序目录无需任何外部文件 ----------
# 网页（RT_HTML）+ 背景图（RCDATA）+ WebView2Loader.dll（RCDATA）全部在 exe 内，
# 运行时 WebView2Loader.dll 释放到 %TEMP%，WebView2 缓存也放 %TEMP%。
# exe 顶层只剩：StardustLauncher.exe + config.ini。

# 设置输出目录，方便打包
set_target_properties(StardustLauncher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
