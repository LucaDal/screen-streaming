# Distribuire StreamingApp

## Cosa inviare

| Destinazione | File da condividere | Avvio |
| --- | --- | --- |
| Linux, stessa architettura e glibc compatibile | `dist/StreamingApp-x86_64.AppImage` (oppure aarch64) | Rendere eseguibile e aprire |
| Windows 10/11 x64 | `dist/StreamingApp-windows-x64.zip` | Estrarre **tutta** la cartella e aprire `streaming_app.exe` |

Invia anche l’**invito alla stanza**, copiato dal client. Il destinatario lo
incolla e preme Entra. Non gli servono sorgenti, CMake, kit Qt, cartella build,
chiave del server o `token-file`. Il signaling gira su un solo server
raggiungibile da entrambi: non deve essere avviato su ogni PC.

Per PC diversi usa un indirizzo **WSS raggiungibile da entrambi** con un
certificato attendibile. Un invito contenente `127.0.0.1` o `localhost` funziona
solo sulla macchina del server. Su Internet possono servire STUN/TURN,
configurabili in Impostazioni → Rete e diagnostica.

## Salvare la chiave

Nelle impostazioni seleziona **Ricorda la chiave su questo PC** e premi
**Salva impostazioni**. L’opzione è disattivata inizialmente. La chiave viene
riletta al prossimo avvio; togliere la spunta rimuove subito la copia salvata.
Le preferenze vengono salvate anche alla chiusura dell’app.

Il salvataggio usa le preferenze locali dell’utente, **senza cifratura**:
`~/.config/StreamingApp/StreamingApp.conf` su Linux (o la directory
`XDG_CONFIG_HOME`) e il registro utente su Windows. Su Linux il file con la
chiave ha permessi riservati al proprietario. I pacchetti vengono creati solo
da eseguibili, librerie e risorse: non includono queste preferenze o gli inviti.

## Creare l’AppImage su Linux

Servono le dipendenze di compilazione indicate nel README, Python 3, `readelf`,
`gst-inspect-1.0`, `pactl`, `qmake6` e i file di sviluppo PipeWire/SPA. Il primo
avvio scarica `linuxdeploy` e il runtime AppImage dai repository ufficiali
in `.local/appimage-tools`. Non richiede installazioni globali o root.

Dalla directory del progetto:

```sh
python3 scripts/package-appimage.py
```

Lo script compila in Release, prepara `build-appimage/AppDir`, include runtime
Qt/FFmpeg, plugin Qt per X11/Wayland/TLS, plugin GStreamer H.264/Opus/WebRTC,
scanner, `pactl` e il client PipeWire, quindi scrive il file in `dist/`.
È possibile usare `--build-dir altro-build` o `--skip-build` per una build
Release già pronta. `QMAKE=/percorso/qmake6` seleziona un kit Qt specifico,
che deve essere lo stesso usato da CMake. Le librerie e gli strumenti devono
avere l’architettura della macchina di compilazione.

Il desktop del destinatario deve comunque fornire i propri servizi:
PipeWire/xdg-desktop-portal con ScreenCast su Wayland, PulseAudio o
PipeWire-Pulse per l’audio, driver grafici e D-Bus. L’AppImage include le
librerie client, non avvia un secondo server audio o un portale privato.

Per avviare e controllare il runtime incluso:

```sh
chmod +x dist/StreamingApp-x86_64.AppImage
./dist/StreamingApp-x86_64.AppImage --check-runtime
./dist/StreamingApp-x86_64.AppImage
```

Senza FUSE:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./dist/StreamingApp-x86_64.AppImage
```

### Compatibilità Linux

Un’AppImage non rende automaticamente compatibili binari costruiti su una
distribuzione più recente. Lo script scrive in `dist/build-info.json` la
versione minima **glibc** effettivamente richiesta dagli ELF inclusi, le
versioni Qt/GStreamer e gli hash degli strumenti. Lo SHA-256 dell’AppImage è
nel file `.AppImage.sha256` adiacente.

Per supportare Linux meno recenti, esegui **compilazione e packaging** in una
VM/container basata sulla distribuzione più vecchia da supportare, con
Qt >= 6.5 e le dipendenze richieste. Non basta spostare in quel container il
binario compilato su Arch. Prima di distribuire, prova su un PC/VM pulito:
avvio, creazione/accesso stanza, cattura, audio, fullscreen e stop dal portale.
`--check-runtime` controlla il caricamento dei plugin; non sostituisce la
prova di cattura sul desktop.

Riferimenti: [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy),
[plugin Qt](https://github.com/linuxdeploy/linuxdeploy-plugin-qt),
[compatibilità AppImage](https://docs.appimage.org/reference/best-practices.html),
[percorsi GStreamer](https://gstreamer.freedesktop.org/documentation/gstreamer/running.html).

## Creare il pacchetto Windows

L’AppImage è solo per Linux. Su Windows usa Visual Studio 2022, CMake/Ninja,
pkgconf, un kit Qt MSVC x64 e GStreamer **MSVC x86_64 runtime + development**
con tutti i plugin. Apri Developer PowerShell for VS 2022:

```powershell
./scripts/package-windows.ps1 -QtDir 'C:\Qt\6.11.2\msvc2022_64' -GstDir 'C:\gstreamer\1.0\msvc_x86_64'
```

Lo script compila in Release e usa `windeployqt` per Qt e il runtime MSVC;
aggiunge DLL GStreamer, plugin, scanner e risorse runtime. Il risultato è
`dist/StreamingApp-windows-x64.zip`. Con `-SkipBuild -BuildDir percorso` puoi
impacchettare una build già pronta con gli stessi kit.

Il destinatario deve estrarre l’intero ZIP: **il solo `.exe` non basta**.
Se `windeployqt` include un installer del Visual C++ Redistributable anziché
le DLL, eseguilo sul PC destinatario. Il file `Avvia StreamingApp.cmd` avvia
con il PATH del pacchetto; l’app configura anche i propri percorsi GStreamer.

Questo script va eseguito e collaudato su Windows; non produce un installer
firmato e la build Windows non è verificabile dal solo ambiente Linux.
[Documentazione windeployqt](https://doc.qt.io/qt-6/windows-deployment.html).

Le librerie incluse mantengono le rispettive licenze. Per pubblicare i
pacchetti conserva gli avvisi e i sorgenti corrispondenti alle versioni
redistribuite; H.264/x264 e altri componenti possono avere licenze diverse.
