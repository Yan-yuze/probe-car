param(
    [switch]$Upload,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$pio = Get-Command pio -ErrorAction SilentlyContinue
$pioPath = if ($pio) { $pio.Source } else { $null }

if (-not $pioPath) {
    $commonPio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'
    if (Test-Path -LiteralPath $commonPio) {
        $pioPath = $commonPio
    } else {
        throw 'PlatformIO 未安装。请先在 VS Code 安装 PlatformIO IDE 扩展。'
    }
}

$drive = $null
foreach ($candidate in @('P:', 'Q:', 'R:', 'S:', 'T:')) {
    if (-not (Test-Path "$candidate\")) {
        $drive = $candidate
        break
    }
}
if (-not $drive) {
    throw 'P: 到 T: 都已占用，无法为旧版 Xtensa 工具链创建 ASCII 路径。'
}

& subst.exe $drive $projectRoot
if ($LASTEXITCODE -ne 0) {
    throw "无法创建临时盘符 $drive"
}

try {
    # Keep both the compiler executable and source paths free of non-ASCII text.
    $env:PLATFORMIO_CORE_DIR = Join-Path $env:USERPROFILE '.platformio'
    Set-Location "$drive\esp32"

    $arguments = @('run')
    if ($Clean) {
        $arguments += @('-t', 'clean')
    } elseif ($Upload) {
        $arguments += @('-t', 'upload')
    }

    & $pioPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO 退出码：$LASTEXITCODE"
    }
}
finally {
    Set-Location $projectRoot
    & subst.exe $drive /D
}

