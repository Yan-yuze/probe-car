param(
    [switch]$Upload,
    [string]$ProbeSerial = ''
)

$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$keilPath = 'C:\Keil_v5\UV4\UV4.exe'
$cubePath = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
if (-not (Test-Path -LiteralPath $keilPath)) { throw '未找到 Keil UV4.exe，请修改脚本中的 keilPath。' }
$firmwareDir = Join-Path $taskRoot 'stm32\firmware'
New-Item -ItemType Directory -Path $firmwareDir -Force | Out-Null
$taskDrive = $null
foreach ($candidate in @('P:', 'Q:', 'R:', 'S:', 'T:')) {
    if (-not (Test-Path "$candidate\")) { $taskDrive = $candidate; break }
}
if (-not $taskDrive) { throw '没有空闲的临时盘符。' }
& subst.exe $taskDrive $taskRoot
if ($LASTEXITCODE -ne 0) { throw '创建编译路径失败。' }
try {
    $buildArgs = @('-b', "$taskDrive\stm32\Project.uvprojx", '-t', '"Target 1"', '-o', "$taskDrive\stm32\firmware\build.log")
    $buildProcess = Start-Process -FilePath $keilPath -ArgumentList $buildArgs -WindowStyle Hidden -Wait -PassThru
    $buildLog = Get-Content -LiteralPath (Join-Path $firmwareDir 'build.log') -Raw
    Write-Output $buildLog
    if ($buildProcess.ExitCode -ne 0 -or $buildLog -notmatch '0 Error\(s\)') {
        throw "STM32 编译未通过，Keil 退出码 $($buildProcess.ExitCode)。"
    }
    $firmwarePath = Join-Path $firmwareDir 'probe_car_stm32_sg90.hex'
    Copy-Item -LiteralPath (Join-Path $taskRoot 'stm32\Objects\Project.hex') -Destination $firmwarePath
    Write-Output "固件：$firmwarePath"
    if ($Upload) {
        if (-not (Test-Path -LiteralPath $cubePath)) { throw '未找到 STM32CubeProgrammer。' }
        $connectionArgs = @('-c', 'port=SWD', 'freq=1000')
        if ($ProbeSerial) { $connectionArgs += "sn=$ProbeSerial" }
        $backupDir = Join-Path $firmwareDir 'backups'
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
        $backupPath = Join-Path $backupDir ("stm32-before-sg90-{0}.bin" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        & $cubePath @connectionArgs '-u' '0x08000000' '131072' $backupPath
        if ($LASTEXITCODE -ne 0) { throw '读取 STM32 原固件失败，未开始烧录。' }
        & $cubePath @connectionArgs '-w' $firmwarePath '-v' '-rst'
        if ($LASTEXITCODE -ne 0) { throw 'STM32 烧录或校验失败。' }
        Write-Output "原固件备份：$backupPath"
    }
}
finally {
    & subst.exe $taskDrive /D
}
