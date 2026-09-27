# StreamingApp

Progetto didattico C++20 per condividere lo schermo su Linux e Windows.
L’app cattura un monitor a scelta e l’audio del PC, comprime il video in
**H.264** e l’audio in **Opus stereo 48 kHz / 192 kbit/s**, e li trasporta tra
due client con **WebRTC**. Entrambi possono condividere, a turno.

- Framerate selezionabile: **30 o 60 FPS**, predefinito 60.
- Risoluzione massima: 720p o 1080p, predefinita 1080p.
- Bitrate obiettivo: da 1 a 50 Mbit/s, predefinito 12 Mbit/s.
- Statistiche di cattura, decodifica, ritardo locale e frame saltati.
- Stanze da due partecipanti, signaling WebSocket/WSS e scelta del client che
  pubblica lo schermo.
- DTLS-SRTP per il media WebRTC; il signaling inoltra solo SDP e candidati ICE.
- Audio di sistema opzionale, sincronizzazione sul clock della pipeline e volume in ricezione.
- Stanze generate dal signaling, con un unico invito da copiare e incollare.
- Selezione monitor Windows/X11, selezione tramite portale su Wayland.

Registrazione e accelerazione hardware non sono ancora incluse. Il codec video è software.

## Requisiti

- CMake 3.21+, Ninja, compilatore C++20 e pkg-config (oppure pkgconf).
- Qt 6.5+ con Widgets, Multimedia, MultimediaWidgets e WebSockets.
  Su Linux serve anche Qt DBus; i test del portale richiedono `dbus-run-session`.
- Backend **FFmpeg di Qt Multimedia**, con le dipendenze runtime.
- GStreamer 1.20+, header e librerie di sviluppo `gstreamer-1.0`,
  `gstreamer-app-1.0`, `gstreamer-video-1.0`.
- Plugin runtime: `appsrc`, `appsink`, `videoconvert`, `x264enc`, `h264parse`,
  `avdec_h264`, `webrtcbin`, `rtph264pay`, `rtph264depay`, `nice` e i plugin
  DTLS/SRTP (famiglie base, good, bad, ugly e libav).
- Per l’audio: `opusenc`, `opusdec`, `rtpopuspay`, `rtpopusdepay`,
  `audioconvert`, `audioresample`, `volume` e `valve`.
- Linux: `pulsesrc`, `pulsesink` (plugin GStreamer PulseAudio), `pactl` con
  output JSON e PulseAudio oppure PipeWire con `pipewire-pulse`. Non è
  supportata la cattura diretta da un dispositivo ALSA senza questo servizio.
- Windows 10+: `wasapi2src` e `wasapi2sink` (plugin WASAPI2), con loopback.
- Su Linux/Wayland: PipeWire, xdg-desktop-portal e un backend del portale
  compatibile con il desktop e con l’interfaccia ScreenCast.

Con Qt Online Installer seleziona anche Qt Multimedia e un kit compatibile
con il compilatore. Su Linux puoi usare i pacchetti della distribuzione se
forniscono le versioni richieste.

## Plugin e dipendenze runtime su Linux

Per eseguire il client compilato localmente servono i seguenti componenti
GStreamer, sia sul PC che condivide sia su quello che riceve. I comandi sotto
installano il runtime multimediale; restano necessari Qt 6.5+ e il backend
FFmpeg di Qt Multimedia indicati nei requisiti. Per compilare servono anche
compilatore, CMake e i pacchetti di sviluppo.

| Funzione / elementi GStreamer | Arch Linux / EndeavourOS | Debian / Ubuntu |
| --- | --- | --- |
| Runtime, `valve`, strumenti `gst-inspect-1.0` e `gst-launch-1.0` | `gstreamer` | `gstreamer1.0-tools` |
| `appsrc`, `appsink`, `videoconvert`, `audioconvert`, `audioresample`, `volume`, `opusenc`, `opusdec` | `gst-plugins-base` | `gstreamer1.0-plugins-base` |
| RTP: `rtpbin`, `rtph264pay`, `rtph264depay`, `rtpopuspay`, `rtpopusdepay` | `gst-plugins-good` | `gstreamer1.0-plugins-good` |
| WebRTC, parsing H.264 e cifratura: `webrtcbin`, `h264parse`, `dtlssrtpenc`, `dtlssrtpdec`, `srtpenc`, `srtpdec` | `gst-plugins-bad` | `gstreamer1.0-plugins-bad` |
| Codifica H.264: `x264enc` | `gst-plugins-ugly` | `gstreamer1.0-plugins-ugly` |
| Decodifica H.264: `avdec_h264` | `gst-libav` | `gstreamer1.0-libav` |
| Connessioni ICE: `nicesrc`, `nicesink` | `libnice` | `gstreamer1.0-nice` |
| Cattura e riproduzione audio: `pulsesrc`, `pulsesink` | `gst-plugins-good` | `gstreamer1.0-pulseaudio` (nelle versioni recenti incluso in `gstreamer1.0-plugins-good`) |
| Comando `pactl` per trovare il monitor dell’uscita audio | `libpulse` | `pulseaudio-utils` |

### Arch Linux / EndeavourOS

```sh
sudo pacman -S --needed \
  gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad \
  gst-plugins-ugly gst-libav libnice libpulse
```

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  gstreamer1.0-nice gstreamer1.0-pulseaudio pulseaudio-utils
```

Su Ubuntu alcuni pacchetti sono nel repository `universe`, che deve essere
abilitato. Installare soltanto `ffmpeg` o le librerie di sviluppo GStreamer
non installa tutti gli elementi elencati.

### Servizi audio e cattura Wayland

Per **Audio del PC** deve essere attivo un server PulseAudio oppure PipeWire
con il servizio di compatibilità `pipewire-pulse`. Il plugin `pulsesrc`
funziona in entrambi i casi. Su un desktop che usa PipeWire, i pacchetti
coinvolti sono `pipewire`, `pipewire-pulse` e il gestore di sessione
`wireplumber`; se l’audio funziona già con PulseAudio, non occorre sostituirlo.
`pactl` da solo non avvia né sostituisce il server audio.

Su **Wayland** servono inoltre `xdg-desktop-portal` e il backend ScreenCast
del proprio desktop: per esempio `xdg-desktop-portal-kde` su KDE Plasma,
`xdg-desktop-portal-gnome` su GNOME o `xdg-desktop-portal-wlr` sui compositor
wlroots compatibili. Installa quello adatto alla tua sessione. Il backend
GTK da solo non fornisce la cattura dello schermo.

### Verifica prima dell’avvio

Il client può controllare TLS e i plugin senza aprire la finestra:

```sh
./build/streaming_app --check-runtime
```

Per controllare un elemento specifico e l’accesso al server audio, esegui
dal terminale della sessione desktop, senza `sudo`:

```sh
gst-inspect-1.0 nicesrc
gst-inspect-1.0 rtpbin
gst-inspect-1.0 pulsesrc
pactl get-default-sink
pactl --format=json list sinks
```

Il controllo runtime verifica i plugin; i comandi `pactl` verificano anche
che il servizio audio sia raggiungibile e che esista un’uscita con monitor.
Dopo aver installato un plugin, riavvia il client.

L’**AppImage** include già Qt, i plugin GStreamer e `pactl`: sul PC destinatario
restano necessari i servizi audio, PipeWire/portale su Wayland e i driver.
Per verificare il contenuto usa `./StreamingApp-x86_64.AppImage --check-runtime`;
vedi la [guida alla distribuzione](packaging/DISTRIBUTION.md).

Riferimenti: [GStreamer su Arch](https://wiki.archlinux.org/title/GStreamer),
[libpulse e pactl su Arch](https://archlinux.org/packages/extra/x86_64/libpulse/files/),
[plugin PulseAudio su Ubuntu](https://packages.ubuntu.com/noble/gstreamer1.0-pulseaudio),
[plugin libnice](https://wiki.freedesktop.org/nice/GStreamer/).

## Compilazione su Linux

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/streaming_app
```

La build Release è consigliata per misurare le prestazioni. Per il debug usa
`-DCMAKE_BUILD_TYPE=Debug`. Se Qt non viene trovato aggiungi
`-DCMAKE_PREFIX_PATH=/percorso/al/kit/Qt`, indicando la directory che contiene
`lib/cmake/Qt6`.

## Compilazione su Windows

Installa Visual Studio 2022 con gli strumenti C++, CMake, Ninja, pkg-config
o pkgconf e un kit Qt per MSVC a 64 bit. Installa anche **runtime e development
package GStreamer MSVC x86_64**, includendo i plugin richiesti. Tutti i kit
devono avere architettura e compilatore compatibili.

Dal terminale **Developer PowerShell for VS 2022**, adatta i percorsi:

```powershell
$qtKit = "C:/Qt/6.11.2/msvc2022_64"
$gstKit = "C:/gstreamer/1.0/msvc_x86_64"
$env:PATH = "$qtKit/bin;$gstKit/bin;$env:PATH"
$env:PKG_CONFIG_PATH = "$gstKit/lib/pkgconfig"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qtKit"
cmake --build build
ctest --test-dir build --output-on-failure
& "$qtKit/bin/windeployqt.exe" --release build/streaming_app.exe
./build/streaming_app.exe
```

Se CMake non trova pkgconf, specifica anche
`-DPKG_CONFIG_EXECUTABLE=C:/percorso/pkgconf.exe`.
`windeployqt` distribuisce i componenti Qt; GStreamer richiede il proprio
runtime e i plugin. Questi comandi avviano l’app nell’ambiente di sviluppo,
non producono ancora un installer autonomo. La build Windows resta da
verificare su Windows.

## Pacchetti da condividere

Su Linux puoi creare un’AppImage con:

```sh
python3 scripts/package-appimage.py
```

Su Windows, da Developer PowerShell con kit Qt/GStreamer MSVC x64:

```powershell
./scripts/package-windows.ps1 -QtDir 'C:\Qt\6.11.2\msvc2022_64' -GstDir 'C:\gstreamer\1.0\msvc_x86_64'
```

Invia il file AppImage ai PC Linux compatibili oppure lo ZIP completo ai PC
Windows, insieme all’invito della stanza. Il signaling resta su un server
raggiungibile da entrambi. La cartella `dist/` contiene gli artefatti e gli
hash; non include chiavi o impostazioni personali.
Lo ZIP Windows include Qt/FFmpeg, GStreamer con i plugin audio/video/WebRTC
e le DLL del runtime Visual C++: chi lo riceve estrae tutto e avvia
`streaming_app.exe`, senza installare dipendenze. Lo script verifica il runtime
distribuito prima di creare lo ZIP e si ferma se manca un componente.

Vedi [guida alla distribuzione](packaging/DISTRIBUTION.md) per requisiti,
compatibilità glibc, verifica dei plugin e istruzioni per il destinatario.
La versione Linux costruita su Arch non è automaticamente compatibile con
Ubuntu/Debian meno recenti; la versione Windows va compilata su Windows.

## Utilizzo e significato dei 60 FPS

1. Chi crea la stanza configura **Server** e **Chiave del server** in
   **Impostazioni**, poi preme **Crea stanza**. Il signaling genera nome e
   chiave casuale della stanza.
2. Premi **Copia invito** e invialo all’altra persona. Il destinatario incolla
   l’intero invito nel campo **Invito** e preme **Entra**: server, stanza e
   credenziale sono già inclusi. L’invito va incollato nell’app; non è ancora
   registrato come protocollo apribile dal browser.
3. Scegli il monitor e lascia selezionato **Audio del PC** per condividere
   l’uscita audio predefinita. Su Wayland il sistema mostra il selettore dello
   schermo all’avvio. Su Windows e X11 il menu elenca i monitor collegati.
4. Premi **Condividi schermo**. Chi trasmette vede la propria anteprima, chi
   riceve vede il video e può regolare il **Volume** (zero per silenziare).
5. Entrambi possono premere **Ferma condivisione**; poi l’altra persona può
   iniziare a condividere. Non serve cambiare stanza.
6. Doppio click sul video per entrare/uscire da tutto schermo; **Esc** e il
   pulsante di uscita funzionano anche sulla superficie video nativa di Qt.

Qualità, FPS e bitrate sono in **Impostazioni**; la sessione rispetta il minore
dei limiti scelti dai due partecipanti. Per cambiarli esci e rientra. STUN,
TURN e statistiche sono in **Rete e diagnostica**. L’app ricorda server e
qualità. Per memorizzare anche la chiave, seleziona **Ricorda la chiave su
questo PC** e premi **Salva impostazioni**. Il salvataggio è locale e non
cifrato; togliere la spunta rimuove la chiave salvata. Gli inviti non vengono
salvati nelle preferenze.

L’invito include una chiave casuale da 256 bit, valida solo per quella stanza.
La chiave del server autorizza la creazione e non è contenuta nell’invito.
La stanza e il suo invito scadono quando esce l’ultimo partecipante o quando
si riavvia il signaling. Chi possiede l’invito può entrare finché c’è posto.
Client e signaling vanno aggiornati insieme: il vecchio accesso con nome
arbitrario e token globale non è più accettato.

### Audio e sincronizzazione

Su Windows viene usato il loopback WASAPI2 dell’uscita predefinita; su Linux
`pactl` individua il monitor dell’uscita predefinita, passato esplicitamente a
`pulsesrc`. Non viene mai selezionato un microfono come ripiego. L’uscita viene
scelta all’avvio della condivisione: dopo aver cambiato dispositivo, ferma e
riavvia. Se il backend manca l’app mostra un errore; puoi disattivare **Audio
del PC** per condividere solo il video.

I timestamp video conservano il momento di consegna del frame catturato,
prima del ridimensionamento e della codifica, e vengono convertiti nel tempo
della pipeline WebRTC. Audio e video condividono il clock monotono del
mittente; RTP/RTCP e i sink sincronizzati usano il clock comune del ricevitore.
L’audio viene inoltrato dal primo keyframe video utile, anche se il selettore
Wayland viene confermato in ritardo. La GUI preleva il frame pronto ogni 8 ms:
questa cadenza e la presentazione del desktop aggiungono un piccolo ritardo.
La latenza precedente alla consegna da Qt e quella dei dispositivi fisici
richiedono verifica sui PC reali; non promettiamo sincronizzazione perfetta
su qualsiasi hardware o rete.

## Provare WebRTC in locale

Genera un token e avvia il signaling server sul computer locale:

```sh
openssl rand -hex 32 > /tmp/streaming-token
./build/signaling_server --insecure-local --port 8443 --token-file /tmp/streaming-token
```

Avvia due istanze di `streaming_app`. Nella prima configura
`ws://127.0.0.1:8443` e il contenuto del file token nelle impostazioni, poi
**Crea stanza → Copia invito**. Nella seconda incolla l’invito e premi
**Entra**. Quando sono presenti due partecipanti, **Condividi schermo** è
disponibile per entrambi. Gli inviti con localhost funzionano solo sullo
stesso computer.
Se i plugin RTP sono nella directory locale `.local/gstreamer-1.0`, il client
la rileva automaticamente; in alternativa puoi impostare esplicitamente
`GST_PLUGIN_PATH_1_0="$PWD/.local/gstreamer-1.0"`.
Il server locale accetta solo loopback. Per una rete reale usa WSS con un
certificato e una chiave:

```sh
./build/signaling_server --port 8443 --token-file /percorso/token \
  --cert /percorso/cert.pem --key /percorso/key.pem
```

Sono supportate chiavi private RSA ed EC in formato PEM senza passphrase.
Il server ricava l'algoritmo dal primo certificato di `--cert`: se il file
contiene una catena, metti prima il certificato del server e poi gli intermedi.
La chiave privata deve corrispondere al certificato del server.
Con Certbot usa `--cert /percorso/live/dominio/fullchain.pem` e
`--key /percorso/live/dominio/privkey.pem`, così il server invia anche gli intermedi.

Nel client inserisci `wss://nome-host:8443`. Il certificato deve essere
attendibile dal sistema. WebRTC prova i candidate ICE diretti. Nei campi STUN e
TURN della finestra puoi aggiungere i server necessari per reti con NAT o
firewall e attivare **Usa solo TURN** quando il traffico diretto non è consentito.
La chiave del server autorizza la creazione delle stanze; le chiavi degli
inviti proteggono l’accesso. Audio e video sono cifrati con DTLS-SRTP. Per
condividere l’invito con un altro PC, crea la stanza usando il nome WSS del
server raggiungibile da entrambi.

Il valore selezionato è il **limite di elaborazione**, non una promessa di
frame disponibili dalla cattura. Una sorgente a 30 FPS non diventa 60 FPS;
non vengono duplicati frame per riempire gli intervalli. Su alcune piattaforme
lo schermo statico può produrre pochi frame o nessuno. Il backend Qt controlla
la frequenza di cattura: il selettore limita il lavoro successivo.

La risoluzione è un rettangolo massimo: l’immagine conserva le proporzioni,
non viene ingrandita e viene arrotondata a dimensioni pari per I420.
Per esempio, una sorgente 1200×2400 con limite 1080p diventa 540×1080.
Il bitrate è un obiettivo dell’encoder, non una misura del traffico effettivo.

- **Cattura:** frame ricevuti da Qt al secondo.
- **Decodifica:** frame effettivamente decodificati al secondo; la velocità di
  presentazione dipende anche dal desktop e dall’interfaccia.
- **Ritardo locale:** tempo dell’ultimo frame dalla consegna da Qt alla fine
  della decodifica, inclusa l’attesa nel nostro buffer. Esclude la latenza
  precedente della cattura, la visualizzazione e un eventuale futuro trasporto.
- **Frame saltati:** frame in attesa sostituiti da uno più recente, sia per
  rispettare il limite FPS sia per evitare accumulo quando l’encoder rallenta.
- **Tempi per fase:** preparazione del frame, codifica, decodifica e uscita
  dal percorso locale (esclusa la visualizzazione remota). Codifica e decodifica includono le conversioni di formato adiacenti.
  Sono i tempi dell’ultimo frame elaborato; per sostenere 60 FPS il totale deve
  rimanere sotto circa 16,7 ms. Se la cattura fornisce meno frame, questo limite
  da solo non basta a raggiungere 60 FPS.

La conversione usa RAM e CPU, H.264 con perdita e campionamento colore 4:2:0.
Non preserva HDR o qualità lossless.

## Come leggere il codice

- `CMakeLists.txt`: dipendenze, build e test.
- `src/main.cpp`: backend Qt FFmpeg, applicazione ed event loop.
- `src/MainWindow.*`: controlli video e aggiornamenti dell’interfaccia.
- `src/ScreenCaptureController.*`: cattura Qt, stati, errori e statistiche.
- `src/VideoPipeline.*`: scambio dei frame tra thread e pipeline GStreamer.
- `src/network/WebRtcPeer.*`: pipeline WebRTC H.264/Opus, cattura/riproduzione audio, clock, ICE e DTLS-SRTP.
- `src/network/RoomClient.*`: stanza, signaling WebSocket e ciclo di condivisione.
- `src/network/SignalingServer.*`: server di signaling con token e limite a due peer.
- `tests/VideoPipelineTest.cpp`: prove del percorso reale di encoding/decoding.

```text
QScreenCapture → QVideoSink → buffer con il frame più recente
                                      ↓ worker C++
                     lettura immagine + ridimensionamento Qt
                                      ↓
             appsrc → videoconvert (I420) → x264enc → h264parse
                                      ├→ rtph264pay → WebRTC/DTLS-SRTP → peer remoto
                                      └→ avdec_h264 → videoconvert (RGBA) → appsink
```

Il callback di cattura passa solo il riferimento al frame a un buffer protetto
da mutex. Conversione, ridimensionamento e attese del codec avvengono nel
worker. Manteniamo un frame in attesa, uno in elaborazione e l’ultimo risultato
disponibile: una GUI lenta non accumula una coda di segnali contenenti immagini.
Il worker limita la cadenza con un clock monotono. L’interfaccia preleva il
risultato con un timer e aggiorna le statistiche ogni secondo; la visualizzazione
del video avviene nel client remoto e nell’anteprima del mittente.

I formati RGB a 32 bit compatibili vengono passati a GStreamer senza una
conversione preventiva in RGBA. Il buffer mantiene in vita l’immagine Qt e
riutilizza la sua memoria in sola lettura, evitando una copia completa per
frame. Quando non serve ridimensionare, l’immagine è una vista in sola lettura
del frame catturato e ne mantiene attiva la mappatura finché necessario.
Per il ridimensionamento manteniamo il percorso di conversione e scaling Qt.
Le conversioni di formato usano fino a quattro thread e il decoder
usa quattro thread con parallelismo per slice, mantenendo la bassa latenza.
Il preset x264 resta `veryfast` e il bitrate scelto viene conservato.

Con Qt 6.8+ il frame decodificato usa un `QAbstractVideoBuffer` che trattiene
il sample GStreamer: non alloca né copia un altro frame RGBA. La memoria resta valida
anche se il worker si ferma o la pipeline viene ricreata; viene rilasciata
con l’ultimo riferimento Qt. Su Qt 6.5–6.7 resta il percorso compatibile con
copia. La conversione colore RGBA resta presente per preservare il risultato.

`VideoPipeline::stop()` sveglia il worker, ne attende la conclusione e scarta
i risultati. I wrapper RAII rilasciano le risorse GStreamer e riportano la
pipeline allo stato NULL anche in caso di errore. La finestra distrugge il
controller prima dei widget. `QPointer` protegge i riferimenti ai monitor.

Su Wayland `PortalSessionGuard` osserva la risposta di creazione sullo stesso
bus D-Bus usato da Qt e chiude esplicitamente la sessione con
[`org.freedesktop.portal.Session.Close`](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Session.html).
Questo compensa il backend PipeWire di Qt 6.11.2, che rilascia lo stream senza
chiudere la sessione del portale. Il controllo è limitato agli handle del nostro
client D-Bus: non riavvia il portale e non chiude catture di altre applicazioni.
Gestisce anche la risposta di creazione arrivata dopo lo stop, l’annullamento
del selettore e lo stop dal desktop. L’app usa una sola sessione ScreenCast
alla volta. Una nuova richiesta attende la conclusione di una creazione
precedente ancora pendente.

I callback di stato/errore della cattura sono accodati e associati al singolo
avvio. Lo stop distrugge gli oggetti Qt prima di tornare allo stato pronto;
non rimane una distruzione differita che possa saltare il rilascio o colpire
una condivisione successiva. Il test `portal_session_cleanup` usa un bus D-Bus
isolato e un portale simulato per verificare dodici cicli avvio/stop, risposte
tardive, annullamento, errori, stop esterno e distruzione. La scomparsa visiva
dell’indicatore va verificata nel desktop Wayland reale.

## Verifiche

I test CTest eseguono realmente encoder e decoder su immagini sintetiche:
30/60 FPS negoziati, colori, ridimensionamento, cambio di proporzioni a caldo,
limite FPS sotto carico, scarto dei frame in eccesso, stop/riavvio, impostazioni
non valide e messaggio per plugin mancanti. Non richiedono accesso allo schermo.
Verificano anche che un frame decodificato rimanga leggibile dopo stop,
riavvio e distruzione della pipeline, incluse mappature ripetute.

Per misurare la pipeline a risoluzioni realistiche su Linux:

```sh
QT_QPA_PLATFORM=offscreen ./build/video_pipeline_test --benchmark
```

Il benchmark alimenta immagini sintetiche variabili a 1080p, 1440p e 4K,
con uscita limitata a 1080p/60 e bitrate 12 Mbit/s. Stampa FPS e tempi medi
per fase. Non misura la cattura del desktop, il trasferimento dalla GPU o la
visualizzazione: serve a confrontare modifiche alla pipeline sulla stessa
macchina. Non è una garanzia di 60 FPS su ogni contenuto o dispositivo.

Nel desktop verifica inoltre:

- Video o finestre in movimento a 1080p/60 e poi 1080p/30, osservando FPS,
  ritardo e CPU nel monitor di sistema.
- Confronto di testo e movimento a diversi bitrate, per esempio 4 e 12 Mbit/s.
- Stop, cambio impostazioni, nuovo avvio e chiusura durante la cattura.
- Annullamento del dialogo Wayland e nuovo tentativo.
- Scelta e scollegamento di un monitor, se ne hai più di uno.
- Esecuzione su Windows e sui desktop Linux da supportare.

Per problemi di cattura controlla il messaggio dell’app e il servizio del
portale del desktop. Per log aggiuntivi su Linux:

```sh
QT_LOGGING_RULES='qt.multimedia.*=true' GST_DEBUG=2 ./build/streaming_app
```

## Test di integrazione e passaggi successivi

CTest include trasporto video e audio/video cifrato con sorgenti sintetiche,
confronto dei timestamp dopo un avvio video ritardato di 1,5 secondi, inviti
generati dal server e limitati alla stanza, rifiuto della chiave globale per
entrare, scadenza dell’invito, qualità negoziata, cambio mittente e doppio click
sulla finestra video nativa. I test WebRTC vengono saltati se non è disponibile
un’interfaccia di rete utilizzabile. Il test sintetico non misura la sincronia
fisica tra altoparlanti e monitor.

Per provare esplicitamente la cattura audio di sistema con ricezione locale
scartata (senza riprodurre l’audio e creare feedback):

```sh
QT_QPA_PLATFORM=offscreen GST_PLUGIN_PATH_1_0="$PWD/.local/gstreamer-1.0" \
  ./build/webrtc_test --system-audio
```

Restano da aggiungere riconnessione, encoding video hardware e un installer Windows.
La build Windows e la prova tra due PC con hardware reale restano da verificare
su quei dispositivi. Il server limita ogni stanza a due partecipanti.

Riferimenti: [QScreenCapture](https://doc.qt.io/qt-6/qscreencapture.html),
[appsrc](https://gstreamer.freedesktop.org/documentation/app/appsrc.html),
[appsink](https://gstreamer.freedesktop.org/documentation/app/appsink.html),
[x264enc](https://gstreamer.freedesktop.org/documentation/x264/index.html),
[loopback WASAPI2](https://gstreamer.freedesktop.org/documentation/wasapi2/wasapi2src.html),
[PulseAudio](https://gstreamer.freedesktop.org/documentation/pulseaudio/index.html),
[sincronizzazione RTP/RTCP](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpbin.html),
[GStreamer su Windows](https://gstreamer.freedesktop.org/documentation/installing/on-windows.html).
