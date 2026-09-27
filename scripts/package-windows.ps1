param(
    [string]$QtDir = "C:\Qt\6.11.2\msvc2022_64",
    [string]$GstDir = "C:\gstreamer\1.0\msvc_x86_64",
    [string]$BuildDir = "",
    [string]$VcRedistDir = $env:VCToolsRedistDir,
    [switch]$SkipBuild,
    [switch]$Clean,
    [switch]$Run
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Assert-Exists {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not (Test-Path $Path)) {
        throw "$Message`nPercorso atteso: $Path"
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )

    Write-Host "> $FilePath $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Comando fallito con exit code $LASTEXITCODE`: $FilePath"
    }
}

# Lo script si trova in <progetto>\scripts\package-windows.ps1.
$projectDir = Split-Path $PSScriptRoot -Parent
if (-not $BuildDir) {
    $BuildDir = Join-Path $projectDir "build-windows"
}

Write-Host "StreamingApp - build/package Windows x64" -ForegroundColor Cyan
Write-Host "Project   : $projectDir"
Write-Host "Qt        : $QtDir"
Write-Host "GStreamer : $GstDir"
Write-Host "Build     : $BuildDir"

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw "CMake non trovato nel PATH. Installa CMake e riapri la shell."
}

$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja) {
    throw "Ninja non trovato nel PATH. Installa Ninja e riapri la shell."
}

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) {
    throw @"
MSVC (cl.exe) non trovato.
Avvia lo script da 'Developer PowerShell for VS' / 'x64 Native Tools' configurato per x64.
"@
}

# Qt e GStreamer usati da questo progetto sono x64: impediamo per errore una
# configurazione con il compilatore MSVC x86.
$clPath = $cl.Source
if ($clPath -match '(?i)\\Hostx86\\x86\\') {
    throw @"
MSVC e' configurato per target x86 (32 bit):
  $clPath

Qt e GStreamer sono x64. Apri una Developer shell con target x64.
Il percorso di cl.exe deve terminare, ad esempio, in Hostx64\x64\cl.exe
(o Hostx86\x64\cl.exe).
"@
}

Write-Host "MSVC      : $clPath" -ForegroundColor Green

# App-local CRT: il destinatario non deve eseguire vc_redist.x64.exe.
if (-not $VcRedistDir -and $env:VCINSTALLDIR) {
    $redistRoot = Join-Path $env:VCINSTALLDIR "Redist\MSVC"
    if (Test-Path $redistRoot) {
        $latestRedist = Get-ChildItem $redistRoot -Directory |
            Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
            Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
        if ($latestRedist) { $VcRedistDir = $latestRedist.FullName }
    }
}
if (-not $VcRedistDir) {
    throw "Runtime MSVC redistribuibile non trovato. Usa -VcRedistDir con la directory Redist\MSVC\<versione> di Visual Studio."
}
Assert-Exists $VcRedistDir "Directory runtime MSVC non trovata."
$crtDir = Get-ChildItem (Join-Path $VcRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" |
    Select-Object -First 1
if (-not $crtDir) { throw "Runtime CRT x64 non trovato in $VcRedistDir. Installa i componenti C++ di Visual Studio." }
foreach ($name in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
    Assert-Exists (Join-Path $crtDir.FullName $name) "Runtime CRT x64 incompleto."
}

# -----------------------------------------------------------------------------
# Qt
# -----------------------------------------------------------------------------
Assert-Exists $QtDir "Directory Qt non trovata. Usa -QtDir con la root del kit MSVC x64."
$QtDir = (Resolve-Path $QtDir).Path

$qtConfig = Join-Path $QtDir "lib\cmake\Qt6\Qt6Config.cmake"
$moc       = Join-Path $QtDir "bin\moc.exe"
$uic       = Join-Path $QtDir "bin\uic.exe"
$rcc       = Join-Path $QtDir "bin\rcc.exe"
$webSocketConfig = Join-Path $QtDir "lib\cmake\Qt6WebSockets\Qt6WebSocketsConfig.cmake"
$multimediaConfig = Join-Path $QtDir "lib\cmake\Qt6Multimedia\Qt6MultimediaConfig.cmake"
$windeployqt = Join-Path $QtDir "bin\windeployqt.exe"

Assert-Exists $qtConfig "Qt6Config.cmake non trovato: il kit Qt sembra incompleto o QtDir non punta alla root del kit."
Assert-Exists $moc "moc.exe non trovato: reinstalla/ripara il kit Qt MSVC 2022 64-bit."
Assert-Exists $uic "uic.exe non trovato: l'installazione Qt sembra incompleta."
Assert-Exists $rcc "rcc.exe non trovato: l'installazione Qt sembra incompleta."
Assert-Exists $webSocketConfig "Qt WebSockets non e' installato per questo kit Qt. Aggiungilo dal Qt Maintenance Tool."
Assert-Exists $multimediaConfig "Qt Multimedia non e' installato per questo kit Qt. Aggiungilo dal Qt Maintenance Tool."
Assert-Exists $windeployqt "windeployqt.exe non trovato nel kit Qt."

# -----------------------------------------------------------------------------
# GStreamer + pkg-config
# -----------------------------------------------------------------------------
Assert-Exists $GstDir "Directory GStreamer non trovata. Installa Runtime + Development MSVC x86_64 oppure usa -GstDir."
$GstDir = (Resolve-Path $GstDir).Path

$gstBin = Join-Path $GstDir "bin"
$gstPkgConfigDir = Join-Path $GstDir "lib\pkgconfig"
Assert-Exists $gstBin "Directory bin di GStreamer non trovata."
Assert-Exists $gstPkgConfigDir "File di sviluppo GStreamer non trovati (lib\pkgconfig). Installa anche il pacchetto Development."

$pkgConfigCandidates = @(
    (Join-Path $gstBin "pkg-config.exe"),
    (Join-Path $gstBin "pkgconf.exe")
)
$pkgConfig = $pkgConfigCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $pkgConfig) {
    $systemPkgConfig = Get-Command pkg-config.exe -ErrorAction SilentlyContinue
    if (-not $systemPkgConfig) {
        $systemPkgConfig = Get-Command pkgconf.exe -ErrorAction SilentlyContinue
    }
    if ($systemPkgConfig) {
        $pkgConfig = $systemPkgConfig.Source
    }
}

if (-not $pkgConfig) {
    throw @"
pkg-config/pkgconf non trovato.
Installa il pacchetto GStreamer Development MSVC x86_64 oppure specifica un pkg-config valido nel PATH.
"@
}

# Necessari sia durante la configurazione/build sia per i test/avvio locale.
$env:PATH = "$QtDir\bin;$GstDir\bin;$env:PATH"
$env:PKG_CONFIG_PATH = $gstPkgConfigDir

Write-Host "pkg-config: $pkgConfig" -ForegroundColor Green

# -----------------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Pulizia build: $BuildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

if (-not $SkipBuild) {
    Write-Host "`nConfigurazione CMake..." -ForegroundColor Cyan

    $cmakeArgs = @(
        "-S", $projectDir,
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_PREFIX_PATH=$QtDir",
        "-DQt6_DIR=$QtDir\lib\cmake\Qt6",
        "-DPKG_CONFIG_EXECUTABLE=$pkgConfig",
        "-DBUILD_TESTING=OFF"
    )

    Invoke-Checked $cmake.Source @cmakeArgs

    Write-Host "`nCompilazione client..." -ForegroundColor Cyan
    Invoke-Checked $cmake.Source "--build" $BuildDir "--target" "streaming_app" "--config" "Release" "--parallel" "4"

    Write-Host "`nCompilazione signaling server..." -ForegroundColor Cyan
    Invoke-Checked $cmake.Source "--build" $BuildDir "--target" "signaling_server" "--config" "Release" "--parallel" "4"
}

$binary = Join-Path $BuildDir "streaming_app.exe"
if (-not (Test-Path $binary)) {
    $binary = Join-Path $BuildDir "Release\streaming_app.exe"
}
Assert-Exists $binary "streaming_app.exe non trovato. Esegui la build oppure rimuovi -SkipBuild."

$serverBinary = Join-Path $BuildDir "signaling_server.exe"
if (-not (Test-Path $serverBinary)) {
    $serverBinary = Join-Path $BuildDir "Release\signaling_server.exe"
}
Assert-Exists $serverBinary "signaling_server.exe non trovato. Esegui la build oppure rimuovi -SkipBuild."

# -----------------------------------------------------------------------------
# Staging / deployment
# -----------------------------------------------------------------------------
$dist = Join-Path $projectDir "dist"
$stage = Join-Path $dist "StreamingApp-windows-x64"
$serverStage = Join-Path $dist "StreamingApp-signaling-windows-x64"
$marker = Join-Path $stage ".streaming-package"
$serverMarker = Join-Path $serverStage ".streaming-signaling-package"

if (Test-Path $stage) {
    if (-not (Test-Path $marker)) {
        throw "Rifiuto di sostituire una directory non riconosciuta: $stage"
    }
    Remove-Item -Recurse -Force $stage
}
if (Test-Path $serverStage) {
    if (-not (Test-Path $serverMarker)) {
        throw "Rifiuto di sostituire una directory non riconosciuta: $serverStage"
    }
    Remove-Item -Recurse -Force $serverStage
}

New-Item -ItemType Directory -Force $dist | Out-Null
New-Item -ItemType Directory -Force $stage | Out-Null
New-Item -ItemType File -Force $marker | Out-Null
Copy-Item $binary (Join-Path $stage "streaming_app.exe")

New-Item -ItemType Directory -Force $serverStage | Out-Null
New-Item -ItemType File -Force $serverMarker | Out-Null
Copy-Item $serverBinary (Join-Path $serverStage "signaling_server.exe")

Write-Host "`nDeploy Qt..." -ForegroundColor Cyan
Invoke-Checked $windeployqt "--release" "--no-compiler-runtime" "--dir" $stage (Join-Path $stage "streaming_app.exe")

Write-Host "Deploy Qt signaling server..." -ForegroundColor Cyan
Invoke-Checked $windeployqt "--release" "--no-compiler-runtime" "--dir" $serverStage (Join-Path $serverStage "signaling_server.exe")

# GStreamer non viene distribuito da windeployqt.
Write-Host "Deploy GStreamer..." -ForegroundColor Cyan
$dlls = Get-ChildItem $gstBin -Filter *.dll
if (-not $dlls) {
    throw "Nessuna DLL GStreamer trovata in $gstBin"
}
$dlls | Copy-Item -Destination $stage
$dlls | Copy-Item -Destination $serverStage

foreach ($destination in @($stage, $serverStage)) {
    foreach ($dll in (Get-ChildItem $crtDir.FullName -Filter *.dll)) {
        $target = Join-Path $destination $dll.Name
        # GStreamer puo' includere un CRT piu' recente: non retrocederlo.
        if (Test-Path $target) {
            $oldVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($target)
            $newVersion = $dll.VersionInfo
            $oldNumber = [version]::new($oldVersion.FileMajorPart, $oldVersion.FileMinorPart, $oldVersion.FileBuildPart, $oldVersion.FilePrivatePart)
            $newNumber = [version]::new($newVersion.FileMajorPart, $newVersion.FileMinorPart, $newVersion.FileBuildPart, $newVersion.FilePrivatePart)
            if ($oldNumber -gt $newNumber) { continue }
        }
        Copy-Item $dll.FullName $target -Force
    }
    Set-Content -Path (Join-Path $destination "qt.conf") -Value "[Paths]`nPrefix=.`nPlugins=." -Encoding ASCII
    Copy-Item (Join-Path $PSScriptRoot "check-windows-runtime.ps1") $destination
}

$gstPluginsSource = Join-Path $GstDir "lib\gstreamer-1.0"
Assert-Exists $gstPluginsSource "Directory plugin GStreamer non trovata."
Copy-Item -Recurse $gstPluginsSource (Join-Path $stage "gstreamer-1.0")
Copy-Item -Recurse $gstPluginsSource (Join-Path $serverStage "gstreamer-1.0")

$scanner = Get-ChildItem $GstDir -Recurse -Filter "gst-plugin-scanner.exe" | Select-Object -First 1
if (-not $scanner) {
    throw "gst-plugin-scanner.exe non trovato nell'installazione GStreamer."
}

$scannerDir = Join-Path $stage "libexec\gstreamer-1.0"
New-Item -ItemType Directory -Force $scannerDir | Out-Null
Copy-Item $scanner.FullName $scannerDir

$serverScannerDir = Join-Path $serverStage "libexec\gstreamer-1.0"
New-Item -ItemType Directory -Force $serverScannerDir | Out-Null
Copy-Item $scanner.FullName $serverScannerDir

foreach ($folder in @("gstreamer-1.0", "licenses")) {
    $source = Join-Path $GstDir "share\$folder"
    if (Test-Path $source) {
        $shareDir = Join-Path $stage "share"
        New-Item -ItemType Directory -Force $shareDir | Out-Null
        Copy-Item -Recurse $source (Join-Path $shareDir $folder)

        $serverShareDir = Join-Path $serverStage "share"
        New-Item -ItemType Directory -Force $serverShareDir | Out-Null
        Copy-Item -Recurse $source (Join-Path $serverShareDir $folder)
    }
}

# Avvio con directory locale in PATH: utile anche al plugin scanner GStreamer.
$launcher = @'
@echo off
setlocal
set "PATH=%~dp0;%PATH%"
set "GST_PLUGIN_PATH_1_0=%~dp0gstreamer-1.0"
start "" "%~dp0streaming_app.exe"
endlocal
'@
Set-Content -Path (Join-Path $stage "Avvia StreamingApp.cmd") -Value $launcher -Encoding ASCII

$distributionReadme = Join-Path $projectDir "packaging\DISTRIBUTION.md"
if (Test-Path $distributionReadme) {
    Copy-Item $distributionReadme (Join-Path $stage "LEGGIMI.md")
}

# Helper per il signaling server.
$serverHelper = @'
STREAMINGAPP - SIGNALING SERVER
===============================

TEST LOCALE SULLO STESSO PC
----------------------------
1. Apri un terminale in questa cartella.
2. Crea un file token contenente una chiave casuale. Se hai OpenSSL:

   openssl rand -hex 32 > streaming-token.txt

   In alternativa crea manualmente un file streaming-token.txt con una stringa
   casuale lunga e non condividerla pubblicamente.

3. Avvia il server locale:

   signaling_server.exe --insecure-local --port 8443 --token-file streaming-token.txt

4. Nel client usa:

   Server: ws://127.0.0.1:8443
   Chiave del server: contenuto di streaming-token.txt

La modalita --insecure-local accetta solo connessioni loopback ed e adatta ai
test con due istanze del client sullo stesso PC.

USO SU RETE / INTERNET
----------------------
Per esporre il signaling ad altri PC usa TLS e un certificato attendibile:

   signaling_server.exe --port 8443 --token-file streaming-token.txt ^
     --cert C:\percorso\cert.pem --key C:\percorso\key.pem

Nel client usa quindi:

   wss://nome-host:8443

Il certificato deve essere valido per il nome host usato dal client e deve
essere considerato attendibile dal sistema.

NOTE
----
- Il signaling inoltra SDP e candidati ICE; lo stream WebRTC non passa
  normalmente attraverso questo server.
- Per reti con NAT/firewall difficili potrebbe essere necessario configurare
  anche STUN/TURN nel client.
- La chiave del server autorizza la creazione delle stanze.
- Conserva token e chiavi private fuori dagli ZIP che condividi pubblicamente.
'@
Set-Content -Path (Join-Path $serverStage "Readme.txt") -Value $serverHelper -Encoding UTF8

$serverLauncher = @'
@echo off
setlocal
set "PATH=%~dp0;%PATH%"
set "GST_PLUGIN_PATH_1_0=%~dp0gstreamer-1.0"
if not exist "%~dp0streaming-token.txt" (
  echo ERRORE: streaming-token.txt non trovato.
  echo Leggi Readme.txt per creare il token.
  pause
  exit /b 1
)
"%~dp0signaling_server.exe" --insecure-local --port 8443 --token-file "%~dp0streaming-token.txt"
endlocal
'@
Set-Content -Path (Join-Path $serverStage "Avvia Signaling Locale.cmd") -Value $serverLauncher -Encoding ASCII

# Verifica i file effettivamente distribuiti, senza PATH o cache dello sviluppatore.
# Un kit GStreamer incompleto (per esempio senza libnice) deve fermare il packaging.
Write-Host "`nVerifica runtime del client e del server..." -ForegroundColor Cyan
& (Join-Path $stage "check-windows-runtime.ps1") -PackageDir $stage
& (Join-Path $serverStage "check-windows-runtime.ps1") -PackageDir $serverStage -Server

# -----------------------------------------------------------------------------
# ZIP + hash
# -----------------------------------------------------------------------------
$zip = Join-Path $dist "StreamingApp-windows-x64.zip"
$serverZip = Join-Path $dist "StreamingApp-signaling-windows-x64.zip"
foreach ($archive in @($zip, $serverZip)) {
    if (Test-Path $archive) {
        Remove-Item -Force $archive
    }
}

Compress-Archive -Path $stage -DestinationPath $zip -Force
Compress-Archive -Path $serverStage -DestinationPath $serverZip -Force

$digest = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path "$zip.sha256" -Value "$digest  StreamingApp-windows-x64.zip" -Encoding ASCII

$serverDigest = (Get-FileHash $serverZip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path "$serverZip.sha256" -Value "$serverDigest  StreamingApp-signaling-windows-x64.zip" -Encoding ASCII

Write-Host "`nBuild e packaging completati." -ForegroundColor Green
Write-Host "Client cartella : $stage"
Write-Host "Client ZIP      : $zip"
Write-Host "Client SHA256   : $zip.sha256"
Write-Host "Server cartella : $serverStage"
Write-Host "Server ZIP      : $serverZip"
Write-Host "Server SHA256   : $serverZip.sha256"
Write-Host "`nClient: usa 'Avvia StreamingApp.cmd' oppure streaming_app.exe."
Write-Host "Server: leggi 'Readme.txt' nella cartella del signaling."

if ($Run) {
    Write-Host "`nAvvio StreamingApp..." -ForegroundColor Cyan
    Start-Process -FilePath (Join-Path $stage "streaming_app.exe") -WorkingDirectory $stage
}
