cmake_minimum_required(VERSION 3.21)
project(MswriteChatTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Qt6 REQUIRED COMPONENTS Pdf QuickWidgets QuickControls2 Test)

add_executable(chat-regression
    chat-regression.cpp
    ../src/ai/ChatView.cpp ../src/ai/ChatView.h
    ../src/ai/AiMathPainter.cpp ../src/ai/AiMathPainter.h
)
target_include_directories(chat-regression PRIVATE ../src)
target_link_libraries(chat-regression PRIVATE Qt6::Widgets)

enable_testing()
add_test(NAME chat-regression COMMAND chat-regression)
set_tests_properties(chat-regression PROPERTIES
    TIMEOUT 45
)

set(CMAKE_AUTORCC ON)
add_executable(pdf-regression pdf-regression.cpp
    ../src/pdfview.cpp ../src/pdfview.h ../resources/app.qrc)
target_include_directories(pdf-regression PRIVATE ../src)
target_link_libraries(pdf-regression PRIVATE Qt6::Widgets Qt6::Pdf
    Qt6::QuickWidgets Qt6::QuickControls2 Qt6::Test ole32)
add_test(NAME pdf-regression COMMAND pdf-regression)
set_tests_properties(pdf-regression PROPERTIES TIMEOUT 60)
add_test(NAME pdf-highdpi-regression COMMAND pdf-regression)
set_tests_properties(pdf-highdpi-regression PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_SCALE_FACTOR=1.5")

option(MSWRITE_NATIVE_SMOKE "Build full desktop integration checks" OFF)
if(MSWRITE_NATIVE_SMOKE)
    find_package(Qt6 REQUIRED COMPONENTS Concurrent Network Sql PdfWidgets)
    file(GLOB APP_SOURCES ../src/*.cpp ../src/ai/*.cpp)
    list(FILTER APP_SOURCES EXCLUDE REGEX "/main\\.cpp$")
    add_executable(native-ui-regression native-ui-regression.cpp ${APP_SOURCES} ../resources/app.qrc)
    target_include_directories(native-ui-regression PRIVATE ../src ../third_party/webview2)
    target_compile_definitions(native-ui-regression PRIVATE MSWRITE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(native-ui-regression PRIVATE Qt6::Widgets Qt6::Pdf Qt6::PdfWidgets
        Qt6::QuickWidgets Qt6::QuickControls2 Qt6::Test Qt6::Concurrent Qt6::Network Qt6::Sql
        ole32 oleaut32 uuid user32 gdi32 shlwapi)
    add_custom_command(TARGET native-ui-regression POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2/WebView2Loader.dll $<TARGET_FILE_DIR:native-ui-regression>)
    add_test(NAME native-ui-regression COMMAND native-ui-regression)
    set_tests_properties(native-ui-regression PROPERTIES TIMEOUT 90)
    add_test(NAME save-regression COMMAND native-ui-regression --save-tests)
    set_tests_properties(save-regression PROPERTIES TIMEOUT 120)
endif()
