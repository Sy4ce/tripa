# 手动构建 tripa / test_document / test_core。
#
# 为什么不用 ninja：本会话的沙箱里 CMake 的 AutoMoc 步骤会去开命名管道抓 moc 输出，
# 被拒（"libuv process spawn failed: operation not permitted"）后 ninja 就静默卡住。
# 这个脚本自己做同样的事：moc -> g++ -c -> g++ 链接，命令与 build.ninja 里的一致。

param(
    [string[]]$Targets = @('tripa', 'test_document', 'test_core')
)

$ErrorActionPreference = 'Stop'
$Qt = 'C:/Qt/6.11.1/mingw_64'
$Mingw = 'C:/Qt/Tools/mingw1310_64/bin'
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build'
$env:PATH = "$Mingw;$Qt/bin;" + $env:PATH

$gxx = "$Mingw/g++.exe"
$moc = "$Qt/bin/moc.exe"

$defines = @(
    '-DMINGW_HAS_SECURE_API=1', '-DQT_CORE_LIB', '-DQT_GUI_LIB', '-DQT_NEEDS_QMAIN',
    '-DQT_NO_DEBUG', '-DQT_PRINTSUPPORT_LIB', '-DQT_WIDGETS_LIB', '-DQT_TESTLIB_LIB',
    '-DUNICODE', '-DWIN32', '-DWIN64', '-D_ENABLE_EXTENDED_ALIGNED_STORAGE', '-D_UNICODE', '-D_WIN64'
)
$includes = @(
    "-I$Build/tripa_autogen/include",
    "-isystem", "$Qt/include/QtCore", "-isystem", "$Qt/include",
    "-isystem", "$Qt/mkspecs/win32-g++",
    "-isystem", "$Qt/include/QtWidgets", "-isystem", "$Qt/include/QtGui",
    "-isystem", "$Qt/include/QtPrintSupport", "-isystem", "$Qt/include/QtTest"
)
$cxxflags = @('-O2', '-DNDEBUG', '-std=gnu++17', '-Wall', '-Wextra', '-Wno-unused-parameter')
$includes += @("-I$Root")

$qtlibs = @(
    "$Qt/lib/libQt6PrintSupport.a", "$Qt/lib/libQt6Test.a", "$Qt/lib/libQt6Widgets.a",
    "$Qt/lib/libQt6Gui.a", '-ld3d11', '-ldxgi', '-ldxguid', '-ld3d12', '-lcomdlg32',
    '-lwinspool', "$Qt/lib/libQt6Core.a", '-lmpr', '-luserenv', '-lmingw32',
    "$Qt/lib/libQt6EntryPoint.a", '-lshell32', '-lkernel32', '-luser32', '-lgdi32',
    '-lwinspool', '-lshell32', '-lole32', '-loleaut32', '-luuid', '-lcomdlg32', '-ladvapi32'
)

$mocHeaders = @(
    'baselineadjust.h', 'fontspool.h', 'mainwindow.h', 'pagesetup.h',
    'paragraph.h', 'proofsheet.h', 'texteditor.h',
    # 新加的两层（都有 Q_OBJECT，必须 moc）
    'paginatinglayout.h', 'richdocument.h',
    # 效果层的计算层（EffectPlanner 有信号）+ 任务调度器
    'effectplanner.h', 'jobrunner.h'
)

function Invoke-Step([string]$what, [scriptblock]$body) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $body
    if ($LASTEXITCODE -ne 0) { throw "$what 失败（exit $LASTEXITCODE）" }
    Write-Host ("  {0}  ({1:n1}s)" -f $what, $sw.Elapsed.TotalSeconds)
}

function Compile([string]$source, [string]$object, [string[]]$extra) {
    $dir = Split-Path -Parent $object
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Invoke-Step "编译 $(Split-Path -Leaf $source)" {
        & $gxx @defines @includes @cxxflags @extra -o $object -c $source
    }
}

# ------------------------------------------------------------------ moc
# moc 不认 -isystem，用它自己的 -I 列表（内容与 AutogenInfo.json 里的一致）
$mocIncludes = @(
    "-I$Build/tripa_autogen/include",
    "-I$Qt/include/QtCore", "-I$Qt/include", "-I$Qt/mkspecs/win32-g++",
    "-I$Qt/include/QtWidgets", "-I$Qt/include/QtGui",
    "-I$Qt/include/QtPrintSupport", "-I$Qt/include/QtTest",
    "-I$Root"
)
Push-Location $Build
foreach ($h in $mocHeaders) {
    $out = "$Build/tripa_autogen/EWIEGA46WW/moc_$($h -replace '\.h$','.cpp')"
    Invoke-Step "moc $h" {
        & $moc @defines @mocIncludes '--include' "$Build/tripa_autogen/moc_predefs.h" `
            '-o' $out "$Root/$h"
    }
}

<#
    AutoMoc 生成的 mocs_compilation.cpp 是按 **CMake 配置时**的源文件列表写的。
    手工这条路不重跑 CMake，所以新加的头文件（有 Q_OBJECT）要在这里补一行
    #include，否则链接期报 “undefined reference to vtable”。
    幂等：已经在了就不重复加（CMake 重新生成过这个文件时也不会写重）。
#>
$mocCompilation = "$Build/tripa_autogen/mocs_compilation.cpp"
if (Test-Path $mocCompilation) {
    $text = Get-Content $mocCompilation -Raw
    foreach ($h in $mocHeaders) {
        $include = '#include "EWIEGA46WW/moc_' + ($h -replace '\.h$', '.cpp') + '"'
        if ($text -notmatch [regex]::Escape($include)) {
            Add-Content -Path $mocCompilation -Value $include
            Write-Host "  mocs_compilation.cpp += $include"
        }
    }
}
Pop-Location

# ------------------------------------------------------------------ tripa
if ($Targets -contains 'tripa') {
    Write-Host '== tripa'
    $objs = @()
    Push-Location $Build
    $sources = @(
        'main.cpp', 'mainwindow.cpp', 'texteditor.cpp', 'effectsrenderer.cpp',
        'handwriting.cpp', 'noise.cpp', 'effect.cpp', 'pagesetup.cpp', 'paragraph.cpp',
        'fontspool.cpp', 'proofsheet.cpp', 'baselineadjust.cpp', 'tripadocument.cpp',
        'tripalog.cpp', 'paginatinglayout.cpp', 'richdocument.cpp',
        'jobrunner.cpp', 'effectplanner.cpp'
    )
    foreach ($s in $sources) {
        $obj = "CMakeFiles/tripa.dir/$s.obj"
        Compile "$Root/$s" "$Build/$obj" @()
        $objs += $obj
    }
    <#
        Windows 资源（程序图标 + 版本信息）。
        用 windres 编成 .o 再链接：exe 自己不带 RT_GROUP_ICON 的话，
        资源管理器里显示的就是默认白板图标（Qt 的 setWindowIcon 管不到那儿）。
        CMake 那条路是自动的（tripa.rc 已经加进 PROJECT_SOURCES），手工这条路自己来。
        注意 **工作目录必须是仓库根**：tripa.rc 里的 ICON 是相对路径 "tripa.ico"，
        windres 是按它自己的当前目录找的（不是按 .rc 文件的位置）。
    #>
    New-Item -ItemType Directory -Force -Path "$Build/winres" | Out-Null
    Invoke-Step 'windres tripa.rc' {
        Push-Location $Root
        & "$Mingw/windres.exe" '-i' 'tripa.rc' '-o' "$Build/winres/tripa_rc.o"
        Pop-Location
        if ($LASTEXITCODE -ne 0) { throw "windres 失败（exit $LASTEXITCODE）" }
    }
    $objs += 'winres/tripa_rc.o'
    foreach ($extra in @('tripa_autogen/mocs_compilation.cpp',
                         'tripa_autogen/EWIEGA46WW/qrc_resources.cpp')) {
        $obj = "CMakeFiles/tripa.dir/$extra.obj"
        Compile "$Build/$extra" "$Build/$obj" @()
        $objs += $obj
    }
    $translation = 'build/.qt/rcc/qrc_tripa_translations.cpp'
    if (Test-Path "$Build/$translation") {
        $obj = "CMakeFiles/tripa.dir/$translation.obj"
        Compile "$Build/$translation" "$Build/$obj" @()
        $objs += $obj
    }
    Invoke-Step '链接 tripa.exe' {
        & $gxx '-O2' '-DNDEBUG' '-mwindows' @objs '-o' 'tripa.exe' `
            '-Wl,--out-implib,libtripa.dll.a' '-Wl,--major-image-version,0,--minor-image-version,0' `
            @qtlibs
    }
    Pop-Location
}

# ------------------------------------------------------------------ 测试程序
<#
    测试程序。
    \a mocFor：这些头文件里有 Q_OBJECT，但对应的 moc_*.cpp 默认只编进了 tripa 那个
    目标（mocs_compilation.cpp），测试程序里要用就得自己再编一份 ——
    否则链接期报 “undefined reference to vtable / staticMetaObject”。
#>
function BuildTest([string]$name, [string[]]$sources, [string[]]$mocFor = @()) {
    Write-Host "== $name"
    $objs = @()
    Push-Location $Build
    foreach ($s in $sources) {
        $obj = "CMakeFiles/$name.dir/$s.obj"
        Compile "$Root/$s" "$Build/$obj" @()
        $objs += $obj
    }
    foreach ($h in $mocFor) {
        $stub = $h -replace '\.h$', '.cpp'
        $obj = "CMakeFiles/$name.dir/moc_$stub.obj"
        Compile "$Build/tripa_autogen/EWIEGA46WW/moc_$stub" "$Build/$obj" @()
        $objs += $obj
    }
    Invoke-Step "链接 $name.exe" {
        & $gxx '-O2' '-DNDEBUG' '-mconsole' @objs '-o' "$name.exe" @qtlibs
    }
    Pop-Location
}

if ($Targets -contains 'test_document') {
    BuildTest 'test_document' @('tests/test_document.cpp', 'tripadocument.cpp',
                                'handwriting.cpp', 'noise.cpp', 'effect.cpp')
}
if ($Targets -contains 'test_core') {
    BuildTest 'test_core' @('tests/test_core.cpp', 'handwriting.cpp', 'noise.cpp', 'effect.cpp')
}
if ($Targets -contains 'test_layout') {
    BuildTest 'test_layout' @('tests/test_layout.cpp', 'paginatinglayout.cpp', 'richdocument.cpp',
                             'pagesetup.cpp', 'paragraph.cpp', 'tripadocument.cpp',
                             'handwriting.cpp', 'noise.cpp', 'effect.cpp', 'tripalog.cpp') `
              @('paginatinglayout.h', 'richdocument.h', 'pagesetup.h', 'paragraph.h')
}
if ($Targets -contains 'bench_effects') {
    BuildTest 'bench_effects' @('tests/bench_effects.cpp', 'paginatinglayout.cpp', 'richdocument.cpp',
                              'pagesetup.cpp', 'paragraph.cpp', 'effectsrenderer.cpp',
                              'handwriting.cpp', 'noise.cpp', 'effect.cpp', 'tripalog.cpp') `
              @('paginatinglayout.h', 'richdocument.h', 'pagesetup.h', 'paragraph.h')
}
if ($Targets -contains 'test_jobs') {
    BuildTest 'test_jobs' @('tests/test_jobs.cpp', 'jobrunner.cpp', 'effectplanner.cpp',
                            'effectsrenderer.cpp', 'paginatinglayout.cpp', 'richdocument.cpp',
                            'pagesetup.cpp', 'paragraph.cpp', 'handwriting.cpp', 'noise.cpp',
                            'effect.cpp', 'tripalog.cpp') `
              @('paginatinglayout.h', 'richdocument.h', 'effectplanner.h', 'jobrunner.h',
                'pagesetup.h', 'paragraph.h')
}

Write-Host '构建完成'
