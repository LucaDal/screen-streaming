# Signaling e TURN sulla stessa macchina

Il signaling può avviare **coturn**, distribuire credenziali temporanee ai
partecipanti autenticati e fermare coturn alla propria uscita. Non occorre
configurare il campo TURN sui due client. Installa coturn sul server Linux:

```sh
# Debian / Ubuntu
sudo apt install coturn
# Arch / EndeavourOS
sudo pacman -S coturn
```

Se l'installazione ha attivato `coturn.service`, fermalo prima di usare la
modalità gestita: due istanze non possono occupare le stesse porte.
Non fermare un'istanza già usata da altri servizi; assegna invece porte e
intervalli relay distinti alla nuova istanza.

## Configurazione

Copia `packaging/server/turnserver.conf.example` fuori dal repository e
proteggi il file con `chmod 600`. Genera un segreto con `openssl rand -hex 32`
e sostituisci `REPLACE_WITH_64_HEX_DIGITS` con il risultato, senza virgolette.
Il segreto TURN è diverso dalla chiave usata per creare stanze.

Se la macchina ha direttamente un IP pubblico, non occorre `external-ip`.
Se è dietro un router/NAT, imposta `relay-ip` all'IP locale del server e
`external-ip=IP_PUBBLICO/IP_LOCALE`. Sul router inoltra le porte indicate
sotto verso il server, mantenendo invariati i numeri delle porte.
Un server dietro CGNAT senza port forwarding richiede un VPS con IP pubblico
o un'altra macchina pubblicamente raggiungibile.

Esempio di avvio dalla directory del progetto, adattando dominio e percorsi:

```sh
./build/signaling_server --port 8443 --token-file /percorso/token \
  --cert /percorso/fullchain.pem --key /percorso/privkey.pem \
  --turn-config /percorso/turnserver.conf \
  --turn-url 'turn://stream.example.com:3478?transport=udp' \
  --turn-url 'turn://stream.example.com:3478?transport=tcp'
```

Il dominio TURN deve risolvere all'IP pubblico del server, senza un proxy
HTTP/CDN davanti. Può essere lo stesso dominio del signaling. Non inserire
username o password in `--turn-url`: vengono generati dal server.
`--turn-executable /percorso/turnserver` permette un eseguibile fuori da PATH.
I client continuano a funzionare su Linux e Windows; il deployment coturn
qui documentato è Linux. Su un host Windows usa una VM Linux con rete e
porte pubblicamente raggiungibili per signaling e coturn.

Il signaling rifiuta configurazioni senza autenticazione REST, segreti non
validi e modalità daemon. Legge una copia privata della configurazione:
usa percorsi **assoluti** per eventuali file referenziati e riavvia il servizio
dopo una modifica. L'avvio fallisce se coturn manca. Se coturn termina durante
l'esecuzione, termina anche il signaling con errore. SIGINT/SIGTERM arrestano
entrambi; per l'avvio al boot si può gestire il comando con systemd,
`Restart=on-failure` e `KillMode=control-group`.

## Porte da aprire

| Porta | Protocollo | Uso |
| --- | --- | --- |
| 8443 | TCP | Signaling WSS |
| 3478 | UDP e TCP | Accesso dei client a TURN/STUN |
| 49160–49260 | UDP | Allocazioni relay coturn |

Aprile sia nel firewall della macchina sia in eventuali security group e
router. Aprire solo 3478 non basta. Il traffico in uscita e le risposte UDP
devono essere consentiti. Il relay inoltra tutto il media: dimensiona banda
in ingresso e in uscita e traffico mensile rispetto al bitrate selezionato.
**Non serve aprire porte sui router dei partecipanti**: entrambi i client
iniziano connessioni in uscita verso il server TURN.

Per reti che bloccano anche TCP 3478 puoi aggiungere TURN su TLS: rimuovi
`no-tls`, aggiungi `tls-listening-port=5349`, `cert=/percorso/fullchain.pem` e
`pkey=/percorso/privkey.pem`, apri TCP 5349 e aggiungi
`--turn-url 'turns://stream.example.com:5349?transport=tcp'`.
Serve un certificato valido per quel dominio. Puoi scegliere 443 se libera
e se il processo ha i permessi necessari per una porta privilegiata.
TURN non passa attraverso un normale reverse proxy HTTP.

## Verifica dai due PC

1. Ricompila e aggiorna **signaling ed entrambi i client**.
2. Crea la stanza usando `wss://stream.example.com:8443` e invia l'invito.
3. Su entrambi i client lascia il campo **TURN** vuoto e seleziona
   **Usa solo TURN** prima di entrare nella stanza.
4. Prova la condivisione in entrambe le direzioni. Se funziona, disattiva
   **Usa solo TURN** per consentire anche connessioni dirette.

Un TURN manuale nel client sostituisce quello distribuito dal server.
Le credenziali vengono emesse all'avvio di ogni condivisione, anche se la
stanza è rimasta inattiva a lungo; non sono salvate né incluse nell'invito.
Durano 24 ore: per sessioni più lunghe ferma e riavvia la condivisione prima
della scadenza. Non è ancora implementato il rinnovo durante una sessione.
Uscire dalla stanza non revoca immediatamente le credenziali già emesse.

Se il signaling funziona ma WebRTC va in timeout, verifica IP pubblico,
`external-ip`, intervallo relay, segreto e log coturn. Se WSS non si collega,
verifica prima DNS, certificato e TCP 8443: TURN non risolve errori TLS del
signaling o plugin multimediali mancanti.

Il media resta cifrato DTLS-SRTP anche con `turn://`; `turns://` aggiunge TLS
alla tratta client–TURN. Il relay inoltra i pacchetti cifrati senza decodificare
audio/video. Il server di signaling rimane un componente fidato.

## Test automatici

Con coturn installato, CTest esegue `room_turn_udp` e `room_turn_tcp`: due
client senza TURN manuale ricevono le credenziali dal signaling, forzano il
relay, scambiano frame H.264 e si alternano come mittente. Se coturn manca,
questi due test vengono saltati. Per un eseguibile fuori da PATH:

```sh
STREAMING_TEST_TURNSERVER=/percorso/turnserver \
  ctest --test-dir build --output-on-failure -R 'turn_'
```

Il test delle credenziali verifica anche HMAC, scadenza, escaping degli URL
e rifiuto di configurazioni senza autenticazione. I test di rete richiedono
socket disponibili e un'interfaccia IPv4 diversa dal loopback; non sostituiscono
la verifica di firewall e NAT dalle vostre reti reali.

Riferimenti: [configurazione coturn](https://github.com/coturn/coturn/blob/master/examples/etc/turnserver.conf),
[credenziali e URL GStreamer](https://gstreamer.freedesktop.org/documentation/webrtc/).
