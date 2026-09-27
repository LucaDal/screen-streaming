param(
    [string]$PackageDir = $PSScriptRoot,
    [switch]$Server
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$PackageDir = (Resolve-Path $PackageDir).Path
$executable = if ($Server) { "signaling_server.exe" } else { "streaming_app.exe" }
$requiredFiles = @($executable, "qt.conf", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
if (-not $Server) {
    $requiredFiles += @("platforms\qwindows.dll", "multimedia\ffmpegmediaplugin.dll",
        "libexec\gstreamer-1.0\gst-plugin-scanner.exe")
}
foreach ($file in $requiredFiles) {
    if (-not (Test-Path (Join-Path $PackageDir $file) -PathType Leaf)) {
        throw "Pacchetto incompleto: manca $file in $PackageDir"
    }
}

# Non usare plugin, DLL o registry provenienti dai kit della macchina di build.
$savedEnvironment = @{}
foreach ($entry in (Get-ChildItem Env:)) {
    if ($entry.Name -eq "PATH" -or $entry.Name -match '^(GST_|QT_|QML)') {
        $savedEnvironment[$entry.Name] = $entry.Value
    }
}
$temporary = Join-Path ([System.IO.Path]::GetTempPath()) ("streaming-runtime-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $temporary | Out-Null
$process = $null
try {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $null, "Process")
    }
    $env:PATH = "$PackageDir;$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $PackageDir
    $env:QT_MEDIA_BACKEND = "ffmpeg"
    $env:QT_FORCE_STDERR_LOGGING = "1"
    $env:GST_PLUGIN_PATH_1_0 = Join-Path $PackageDir "gstreamer-1.0"
    $env:GST_PLUGIN_SYSTEM_PATH_1_0 = $env:GST_PLUGIN_PATH_1_0
    $env:GST_PLUGIN_SCANNER_1_0 = Join-Path $PackageDir "libexec\gstreamer-1.0\gst-plugin-scanner.exe"
    $env:GST_REGISTRY_1_0 = Join-Path $temporary "registry.bin"
    $stdout = Join-Path $temporary "stdout.txt"
    $stderr = Join-Path $temporary "stderr.txt"
    $process = Start-Process -FilePath (Join-Path $PackageDir $executable) -ArgumentList "--check-runtime" `
        -WorkingDirectory $temporary -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    if (-not $process.WaitForExit(60000)) {
        throw "Verifica runtime scaduta per $executable. Controlla eventuali errori del loader Windows."
    }
    # Completa anche la lettura asincrona degli stream reindirizzati.
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        $details = (Get-Content $stderr -Raw) + (Get-Content $stdout -Raw)
        throw "Verifica runtime fallita per $executable (exit $($process.ExitCode)).`n$details`nRipara il kit Qt/GStreamer sulla macchina di build e ricrea il pacchetto."
    }
    Write-Host "PASS: $executable, runtime incluso nel pacchetto." -ForegroundColor Green
} finally {
    if ($null -ne $process) {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
    foreach ($entry in (Get-ChildItem Env:)) {
        if ($entry.Name -eq "PATH" -or $entry.Name -match '^(GST_|QT_|QML)') {
            [Environment]::SetEnvironmentVariable($entry.Name, $null, "Process")
        }
    }
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], "Process")
    }
    Remove-Item $temporary -Recurse -Force
}
