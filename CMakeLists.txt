cmake_minimum_required(VERSION 3.16)

project(tripa VERSION 0.4 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 23)
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
        # 渲染层：按需分页（纸张几何全在这里，文档里没有纸的概念）
        paginatinglayout.cpp
        paginatinglayout.h
        texteditor.cpp
        texteditor.h
        # 富文本层：正文 + 逐字格式 + 手写/抖动数据 + 段落字行边距（不管分页）
        richdocument.cpp
        richdocument.h
        effectsrenderer.cpp
        effectsrenderer.h
        # 效果层的计算层：显示表缓存 + 几何分片算（与绘制解耦）
        effectplanner.cpp
        effectplanner.h
        # 长活儿的分片调度 + 进度（状态栏进度条就靠它）
        jobrunner.cpp
        jobrunner.h
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
        baselineadjust.cpp
        baselineadjust.h
        tripadocument.cpp
        tripadocument.h
        tripalog.cpp
        tripalog.h
        resources.qrc
)

# ---------------------------------------------------------------- 程序图标（Windows）
# exe **自己**要带一份图标资源，否则资源管理器 / Alt+Tab 里显示的是默认白板图标
# （QApplication::setWindowIcon() 只管窗口左上角和任务栏，管不到 exe 的文件图标）。
# MSVC 认 .rc，MinGW 会自动调 windres —— 两边都不用额外配置，加进源文件即可。
# 资源里引用的图标写成 "icons\\tripa.ico"（相对 .rc 所在的目录，也就是本文件旁边）。
if(WIN32)
    list(APPEND PROJECT_SOURCES tripa.rc)
endif()

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
    target_link_libraries(tripa PRIVATE Qt6::Widgets Qt6::PrintSupport)
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

# ---------------------------------------------------------------- 文档格式自检
# .tripa 的存/读往返：正文、逐字格式、段落格式、效果区间、页面设置、噪声波。
# 跑法: cmake --build build --target test_document && ./build/test_document.exe
add_executable(test_document
    tests/test_document.cpp
    tripadocument.cpp
    handwriting.cpp
    noise.cpp
    effect.cpp
)
target_include_directories(test_document PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_document PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Gui
)
if(MSVC)
    target_compile_options(test_document PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------- 分页排版自检
# 渲染层的几何：按需分页、一行都不越下边界、每一页只有自己那一段内容、
# 滚动下边界的定义、命中测试与光标的往返。
# 跑法: cmake --build build --target test_layout && ./build/test_layout.exe
add_executable(test_layout
    tests/test_layout.cpp
    paginatinglayout.cpp
    richdocument.cpp
    pagesetup.cpp
    paragraph.cpp
    tripadocument.cpp
    # paginatinglayout / richdocument 里的 TRIPA_* 宏最终走到这里
    tripalog.cpp
    handwriting.cpp
    noise.cpp
    effect.cpp
)
target_include_directories(test_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_layout PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Gui
    # pagesetup.cpp 的 applyToPrinter() 用了 QPrinter（打印支持模块）
    Qt${QT_VERSION_MAJOR}::PrintSupport
)
if(MSVC)
    target_compile_options(test_layout PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------- 长活儿 / 计算层自检
# 分片调度（真的让出事件循环了吗、进度单调吗、取消干净吗）、
# 以及效果显示表的按段落惰性计算、"算好才挖洞"。
# 跑法: cmake --build build --target test_jobs && ./build/test_jobs.exe
add_executable(test_jobs
    tests/test_jobs.cpp
    jobrunner.cpp
    effectplanner.cpp
    effectsrenderer.cpp
    paginatinglayout.cpp
    richdocument.cpp
    pagesetup.cpp
    paragraph.cpp
    handwriting.cpp
    noise.cpp
    effect.cpp
    tripalog.cpp
)
target_include_directories(test_jobs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_jobs PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::PrintSupport
)
if(MSVC)
    target_compile_options(test_jobs PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------- 性能基准（不是"通过/失败"）
# 量清楚"加噪声"这条路上每一步各花多久：逐字写格式 / 规划显示表 / 算字形几何 /
# 绘制一次。这些数字是 2.6.2 那套"算的归算、画的归画"的依据 ——
# 以后要把什么挪出绘制路径，先跑它，别拍脑袋。
# 跑法: cmake --build build --target bench_effects && ./build/bench_effects.exe [字数]
add_executable(bench_effects
    tests/bench_effects.cpp
    effectsrenderer.cpp
    paginatinglayout.cpp
    richdocument.cpp
    pagesetup.cpp
    paragraph.cpp
    handwriting.cpp
    noise.cpp
    effect.cpp
    tripalog.cpp
)
target_include_directories(bench_effects PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_effects PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::PrintSupport
)
if(MSVC)
    target_compile_options(bench_effects PRIVATE /utf-8)
endif()
