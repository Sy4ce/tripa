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
    'paragraph.h', 'proofsheet.h', 'texteditor.h'
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
Pop-Location

# ------------------------------------------------------------------ tripa
if ($Targets -contains 'tripa') {
    Write-Host '== tripa'
    $objs = @()
    Push-Location $Build
    $sources = @(
        'main.cpp', 'mainwindow.cpp', 'texteditor.cpp', 'effectsrenderer.cpp',
        'handwriting.cpp', 'noise.cpp', 'effect.cpp', 'pagesetup.cpp', 'paragraph.cpp',
        'fontspool.cpp', 'proofsheet.cpp', 'baselineadjust.cpp', 'tripadocument.cpp'
    )
    foreach ($s in $sources) {
        $obj = "CMakeFiles/tripa.dir/$s.obj"
        Compile "$Root/$s" "$Build/$obj" @()
        $objs += $obj
    }
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
function BuildTest([string]$name, [string[]]$sources) {
    Write-Host "== $name"
    $objs = @()
    Push-Location $Build
    foreach ($s in $sources) {
        $obj = "CMakeFiles/$name.dir/$s.obj"
        Compile "$Root/$s" "$Build/$obj" @()
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

Write-Host '构建完成'
