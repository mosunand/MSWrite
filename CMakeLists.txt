cmake_minimum_required(VERSION 3.21)
project(Mswrite VERSION 3.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Network Sql Pdf PdfWidgets QuickWidgets QuickControls2)

add_executable(Mswrite WIN32
    src/main.cpp
    src/webviewhost.h      src/webviewhost.cpp
    src/bridge.h           src/bridge.cpp
    src/fileservice.h      src/fileservice.cpp
    src/outlinedock.h      src/outlinedock.cpp
    src/searchdock.h       src/searchdock.cpp
    src/quickopendialog.h  src/quickopendialog.cpp
    src/mainwindow.h       src/mainwindow.cpp
    src/pdfview.h          src/pdfview.cpp
    src/uidialogs.h        src/uidialogs.cpp
    src/welcomeoverlay.h   src/welcomeoverlay.cpp
    src/ai/Types.h
    src/ai/Http.h          src/ai/Http.cpp
    src/ai/LlmCodec.h      src/ai/LlmCodec.cpp
    src/ai/Llm.h           src/ai/Llm.cpp
    src/ai/AiProviders.h   src/ai/AiProviders.cpp
    src/ai/AiDoctor.h      src/ai/AiDoctor.cpp
    src/ai/MswriteSkill.h  src/ai/MswriteSkill.cpp
    src/ai/AiWorker.h      src/ai/AiWorker.cpp
    src/ai/AiChatDock.h    src/ai/AiChatDock.cpp
    src/ai/AiModelPicker.h
    src/ai/ChatView.h      src/ai/ChatView.cpp
    src/ai/ChatWebView.h   src/ai/ChatWebView.cpp
    src/ai/AiMath.h        src/ai/AiMath.cpp
    src/ai/AiMathPainter.h src/ai/AiMathPainter.cpp
    src/ai/AiText.h        src/ai/AiText.cpp
    src/ai/AiCcSwitch.h    src/ai/AiCcSwitch.cpp
    src/ai/AiConfigDialog.h src/ai/AiConfigDialog.cpp
    resources/app.qrc
    resources/app.rc
)

target_include_directories(Mswrite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2
)

# 源码目录注入:开发态运行时从源码树读取 resources/web 页面资源
target_compile_definitions(Mswrite PRIVATE
    MSWRITE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    MSWRITE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(Mswrite PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    Qt6::Sql
    Qt6::Pdf
    Qt6::PdfWidgets
    Qt6::QuickWidgets
    Qt6::QuickControls2
    ole32
    oleaut32
    uuid
    user32
    gdi32
    shlwapi
)

# WebView2Loader.dll 放到 exe 旁边,运行时 LoadLibrary 动态加载
# Qt6 Pdf 两枚 DLL 一并复制:脱离 Qt 环境也能打开 PDF
add_custom_command(TARGET Mswrite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/WebView2Loader.dll
            $<TARGET_FILE_DIR:Mswrite>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Qt6::Pdf> $<TARGET_FILE_DIR:Mswrite>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Qt6::PdfWidgets> $<TARGET_FILE_DIR:Mswrite>
)
