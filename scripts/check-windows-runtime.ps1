param(
    [string]$PackageDir = $PSScriptRoot,
    [switch]$Server
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$PackageDir = (Resolve-Path $PackageDir).Path

$executable = if ($Server) {
    "signaling_server.exe"
} else {
    "streaming_app.exe"
}

# ---------------------------------------------------------------------------
# Verifica preliminare dei file necessari
# ---------------------------------------------------------------------------

$requiredFiles = @(
    $executable,
    "qt.conf",
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)

if (-not $Server) {
    $requiredFiles += @(
        "platforms\qwindows.dll",
        "multimedia\ffmpegmediaplugin.dll",
        "libexec\gstreamer-1.0\gst-plugin-scanner.exe"
    )
}

foreach ($file in $requiredFiles) {

    $fullPath = Join-Path $PackageDir $file

    if (-not (Test-Path $fullPath -PathType Leaf)) {
        throw "Pacchetto incompleto: manca $file in $PackageDir"
    }
}

# ---------------------------------------------------------------------------
# Salva l'ambiente corrente
#
# Il test deve verificare SOLO il contenuto del pacchetto.
# Non deve accidentalmente trovare Qt/GStreamer installati sulla macchina
# utilizzata per creare la build.
# ---------------------------------------------------------------------------

$savedEnvironment = @{}

foreach ($entry in (Get-ChildItem Env:)) {

    if (
        $entry.Name -eq "PATH" -or
        $entry.Name -match '^(GST_|QT_|QML)'
    ) {
        $savedEnvironment[$entry.Name] = $entry.Value
    }
}

# Directory temporanea utilizzata anche per il registry GStreamer.
$temporary = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ("streaming-runtime-" + [guid]::NewGuid().ToString("N"))

New-Item -ItemType Directory -Path $temporary | Out-Null

$process = $null

try {

    # -----------------------------------------------------------------------
    # Pulisce l'ambiente Qt / GStreamer
    # -----------------------------------------------------------------------

    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable(
            $name,
            $null,
            "Process"
        )
    }

    # Windows deve cercare le DLL prima di tutto dentro il pacchetto.
    $env:PATH = "$PackageDir;$env:SystemRoot\System32;$env:SystemRoot"

    # -----------------------------------------------------------------------
    # Qt
    # -----------------------------------------------------------------------

    $env:QT_PLUGIN_PATH = $PackageDir
    $env:QT_MEDIA_BACKEND = "ffmpeg"

    # Fa comparire eventuali errori Qt su stderr.
    $env:QT_FORCE_STDERR_LOGGING = "1"

    # -----------------------------------------------------------------------
    # GStreamer
    # -----------------------------------------------------------------------

    $env:GST_PLUGIN_PATH_1_0 = Join-Path `
        $PackageDir `
        "gstreamer-1.0"

    $env:GST_PLUGIN_SYSTEM_PATH_1_0 = `
        $env:GST_PLUGIN_PATH_1_0

    $env:GST_PLUGIN_SCANNER_1_0 = Join-Path `
        $PackageDir `
        "libexec\gstreamer-1.0\gst-plugin-scanner.exe"

    # Registry temporaneo:
    # in questo modo il test non usa quello eventualmente presente
    # nel profilo Windows dell'utente.
    $env:GST_REGISTRY_1_0 = Join-Path `
        $temporary `
        "registry.bin"

    # -----------------------------------------------------------------------
    # Avvio del check runtime
    # -----------------------------------------------------------------------

    $exePath = Join-Path $PackageDir $executable

    Write-Host ""
    Write-Host "Verifica runtime: $executable"
    Write-Host "Package        : $PackageDir"
    Write-Host "Executable     : $exePath"
    Write-Host ""

    $psi = New-Object System.Diagnostics.ProcessStartInfo

    $psi.FileName = $exePath
    $psi.Arguments = "--check-runtime"
    $psi.WorkingDirectory = $temporary

    # Fondamentale per poter redirigere stdout/stderr.
    $psi.UseShellExecute = $false

    $psi.CreateNoWindow = $true

    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi

    if (-not $process.Start()) {
        throw "Impossibile avviare $executable."
    }

    # Leggiamo entrambi gli stream in modo asincrono.
    # Evita deadlock nel caso uno dei buffer venga riempito.
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    # -----------------------------------------------------------------------
    # Timeout
    # -----------------------------------------------------------------------

    $timeoutMs = 60000

    if (-not $process.WaitForExit($timeoutMs)) {

        try {
            $process.Kill()
            $process.WaitForExit()
        }
        catch {
            # Ignora eventuali errori durante il kill:
            # stiamo comunque terminando il test.
        }

        throw @"
Verifica runtime scaduta per $executable dopo 60 secondi.

Controlla eventuali errori del loader Windows,
Qt, TLS oppure GStreamer.
"@
    }

    # Garantisce che il processo sia completamente terminato e che
    # ExitCode sia disponibile.
    $process.WaitForExit()

    # Recupera l'output dopo la terminazione.
    $stdoutText = $stdoutTask.Result
    $stderrText = $stderrTask.Result

    # Salviamo esplicitamente l'exit code PRIMA di disporre il processo.
    $exitCode = $process.ExitCode

    # -----------------------------------------------------------------------
    # Mostra l'output prodotto dal programma
    # -----------------------------------------------------------------------

    if (-not [string]::IsNullOrWhiteSpace($stdoutText)) {
        Write-Host $stdoutText.TrimEnd()
    }

    if (-not [string]::IsNullOrWhiteSpace($stderrText)) {
        Write-Host $stderrText.TrimEnd()
    }

    # -----------------------------------------------------------------------
    # Controllo risultato
    # -----------------------------------------------------------------------

    if ($exitCode -ne 0) {

        $details = ""

        if (-not [string]::IsNullOrWhiteSpace($stderrText)) {
            $details += "`nSTDERR:`n$stderrText"
        }

        if (-not [string]::IsNullOrWhiteSpace($stdoutText)) {
            $details += "`nSTDOUT:`n$stdoutText"
        }

        throw @"
Verifica runtime fallita per $executable (exit $exitCode).
$details

Ripara il kit Qt/GStreamer sulla macchina di build
e ricrea il pacchetto.
"@
    }

    Write-Host ""
    Write-Host `
        "PASS: $executable, runtime incluso nel pacchetto." `
        -ForegroundColor Green
}
finally {

    # -----------------------------------------------------------------------
    # Cleanup processo
    # -----------------------------------------------------------------------

    if ($null -ne $process) {

        try {

            if (-not $process.HasExited) {
                $process.Kill()
                $process.WaitForExit()
            }

        }
        catch {
            # Non mascherare eventuali errori precedenti.
        }

        $process.Dispose()
    }

    # -----------------------------------------------------------------------
    # Rimuove variabili Qt/GStreamer impostate dal test
    # -----------------------------------------------------------------------

    foreach ($entry in (Get-ChildItem Env:)) {

        if (
            $entry.Name -eq "PATH" -or
            $entry.Name -match '^(GST_|QT_|QML)'
        ) {

            [Environment]::SetEnvironmentVariable(
                $entry.Name,
                $null,
                "Process"
            )
        }
    }

    # -----------------------------------------------------------------------
    # Ripristina l'ambiente originale
    # -----------------------------------------------------------------------

    foreach ($name in $savedEnvironment.Keys) {

        [Environment]::SetEnvironmentVariable(
            $name,
            $savedEnvironment[$name],
            "Process"
        )
    }

    # -----------------------------------------------------------------------
    # Rimuove directory temporanea
    # -----------------------------------------------------------------------

    if (Test-Path $temporary) {
        Remove-Item $temporary -Recurse -Force
    }
}