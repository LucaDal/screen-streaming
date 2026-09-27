# Signaling and TURN server

Run one signaling server on a Linux machine reachable by both clients.
It manages rooms, starts coturn, and supplies temporary TURN credentials.
Clients can run on Linux or Windows.

## Setup

Install coturn using your distribution's package manager:

```sh
sudo apt install coturn       # Debian / Ubuntu
# or: sudo pacman -S coturn   # Arch / EndeavourOS
```

Use a domain with one DNS A record pointing directly to the server's public
IPv4 address, without an HTTP proxy/CDN. Behind a router, forward the ports
listed below. Behind CGNAT without port forwarding, use a publicly reachable server.

Create `signaling.ini` outside the repository:

```ini
server-url=wss://stream.example.com:8443
token=REPLACE_WITH_YOUR_RANDOM_TOKEN
cert=/path/to/fullchain.pem
key=/path/to/privkey.pem
```

Replace the domain and paths. Generate a token with `openssl rand -hex 32`
and paste the result into `token`. It must contain 16–1024 characters.
Alternatively, replace `token` with `token-file=/path/to/token`; use only one.

The certificate must be trusted by the clients and match the domain.
Use a matching RSA or EC private key in PEM format without a passphrase.
The user running the server must be able to read both files.

From the project directory, after building:

```sh
chmod 600 /path/to/signaling.ini
./build/signaling_server --config /path/to/signaling.ini
```

Relative paths in the INI are relative to its directory. `~` and environment
variables are not expanded. Command-line options override INI values.
Comments may start with `#` or `;` on separate lines.
See [signaling.ini.example](signaling.ini.example) for optional settings,
or run `./build/signaling_server --help`.

## Network ports

| Port | Protocol | Purpose |
| --- | --- | --- |
| 8443 | TCP | WSS signaling |
| 3478 | UDP and TCP | TURN connections |
| 49160–49260 | UDP | TURN relay traffic |

Allow these ports in the server firewall, any cloud firewall, and the router.
Forward TURN ports without changing their numbers. Participants do not need
to forward ports on their routers. Avoid running another coturn instance on
the same ports.

The signaling port comes from `server-url` (443 if omitted); `port` overrides
the local listening port. Optional INI settings `turn-port`, `turn-min-port`,
and `turn-max-port` change the TURN ports. Set `turn-relay-ip` if you need to
select a specific local interface.

## Automatic TURN and dynamic IP

At startup, the server resolves the domain, detects its local IPv4 address,
and generates a private `turnserver.conf` with a random TURN secret.
It starts and stops coturn automatically.

**After a public IP change, update your dynamic DNS and restart the signaling
server.** The app does not update DNS or reload it during a session. Recreate
the room after restarting.

The generated file is in `/tmp/signaling_server-XXXXXX/turnserver.conf` on
Linux and is deleted on normal shutdown. The startup log shows the directory
in the PID file path. It contains a secret; do not share the full file.

## Connect and check

1. In the first client, enter the WSS URL and the same `token` as in the INI.
2. Leave TURN empty on both clients. Enable relay-only mode on both to test coturn.
3. Create a room, share its invitation, and test screen sharing in both directions.
4. Disable relay-only mode afterward to allow direct connections too.

TURN credentials last 24 hours. For longer sessions, stop and restart sharing
before they expire.

- **WSS does not connect:** check DNS, TCP 8443, certificate paths, and trust.
- **Video times out:** check the public IP, TURN ports, and router forwarding.
- **Coturn warns about `turn_server_cert.pem` or `turn_server_pkey.pem`:** these
  default TURN certificates are unused in automatic mode. Coturn 4.18 may still
  warn about them; they are separate from the signaling certificate.
- **The log mentions the private part of `external-ip`:** this is the local
  server address. The mapping is `external-ip=PUBLIC_IP/LOCAL_IP`.

Automatic mode uses `turn://`; WebRTC audio/video remains encrypted with
DTLS-SRTP, and signaling uses WSS.

## Custom TURN configuration

For custom coturn settings, start from [turnserver.conf.example](turnserver.conf.example).
Replace its secret placeholder with a separate `openssl rand -hex 32` value.
Behind NAT, set `relay-ip` and `external-ip=PUBLIC_IP/LOCAL_IP`.

In the signaling INI, remove `server-url` and use:

```ini
port=8443
turn-config=/path/to/turnserver.conf
turn-url=turn://stream.example.com:3478?transport=udp, turn://stream.example.com:3478?transport=tcp
```

Keep the token and WSS certificate settings. Protect the coturn file with
`chmod 600` and use absolute paths inside it.
For TURN over TLS, remove `no-tls`, add `tls-listening-port=5349`, `cert`, and
`pkey` to the coturn file, open TCP 5349, and include
`turns://stream.example.com:5349?transport=tcp` in `turn-url`.
