cmake_minimum_required(VERSION 3.16)

project(tripa VERSION 0.2 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets PrintSupport LinguistTools Test)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets PrintSupport LinguistTools Test)

set(TS_FILES tripa_zh_CN.ts)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        texteditor.cpp
        texteditor.h
        effectsoverlay.cpp
        effectsoverlay.h
        effectsrenderer.cpp
        effectsrenderer.h
        handwriting.cpp
        handwriting.h
        noise.cpp
        noise.h
        effect.cpp
        effect.h
        pagesetup.cpp
        pagesetup.h
        paragraph.cpp
        paragraph.h
        fontspool.cpp
        fontspool.h
        proofsheet.cpp
        proofsheet.h
        resources.qrc
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(tripa
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
    # 编译翻译并自动打包成 :/i18n/tripa_zh_CN.qm
    qt_add_translations(tripa
        TS_FILES ${TS_FILES}
        RESOURCE_PREFIX "/i18n"
    )
else()
    add_executable(tripa ${PROJECT_SOURCES})
    qt5_create_translation(QM_FILES ${CMAKE_CURRENT_SOURCE_DIR} ${TS_FILES})
    add_dependencies(tripa ${QM_FILES})
    target_link_libraries(tripa PRIVATE Qt5::Widgets Qt5::PrintSupport)
endif()

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    target_link_libraries(tripa PRIVATE
        Qt6::Widgets
        Qt6::PrintSupport
        # 只为 --uitest 里的鼠标交互自检：QTest::mousePress/Move/Release
        # 是唯一能正确维护 Qt 内部按键/拖拽状态的做法，
        # 手搓 QMouseEvent + sendEvent 送不进拖选（实测：移动事件被丢掉）。
        Qt6::Test
    )
endif()

if(MSVC)
    target_compile_options(tripa PRIVATE /utf-8 /W3)
else()
    target_compile_options(tripa PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.tripa)
endif()
set_target_properties(tripa PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS tripa
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(tripa)
endif()

# ---------------------------------------------------------------- 纯逻辑自检
# 不依赖控件：CSV 解析、手写库、噪声波、笔画扭曲、字符分类。
# 跑法: cmake --build build --target test_core && ./build/test_core.exe
add_executable(test_core
    tests/test_core.cpp
    handwriting.cpp
    noise.cpp
    effect.cpp
)
target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_core PRIVATE Qt${QT_VERSION_MAJOR}::Gui)
if(MSVC)
    target_compile_options(test_core PRIVATE /utf-8)
endif()
