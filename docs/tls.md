# TLS

Modern TLS is done by terminating it at a reverse proxy in front of the bridge,
not on the ESP32 itself. The bridge speaks plain HTTP and WebSocket on the
trusted LAN; the proxy holds a real certificate and speaks HTTPS/WSS to the
browser.

## Why not TLS on the device

Three reasons, in order of how much they matter:

1. **The jog dead-man.** A TLS handshake on an ESP32 takes roughly 0.5–2 s of
   CPU. This firmware is a single cooperative loop, and the one path that can
   drive the rotator into its end stop is the jog dead-man timer — it must
   issue a stop within ~0.5 s of the panel going quiet. A handshake blocking
   the loop would delay exactly that. On-device TLS would trade a real safety
   margin for encryption the LAN mostly does not need.
2. **The stack cannot do it.** `ESPAsyncWebServer` on ESP32 has no working TLS:
   its `beginSecure()` is guarded by `ASYNC_TCP_SSL_ENABLED`, which the ESP32
   `AsyncTCP` does not implement (verified in the installed sources).
   Upstream issue [#899](https://github.com/me-no-dev/ESPAsyncWebServer/issues/899)
   confirms TLS there was an ESP8266-only path needing patched forks and
   1024-bit RSA — archived, and the opposite of modern.
3. **Certificates.** A device-generated self-signed cert means browser
   warnings and no real trust; a proxy gets a genuine Let's Encrypt cert with
   TLS 1.3 for free.

If on-device TLS is ever truly required, it means moving the web layer to
`esp32_https_server` (mbedTLS) and running its handshakes off the control
loop — a substantial change that should be weighed against reason 1.

## What the firmware already does for it

- The session cookie is `HttpOnly; SameSite=Strict`, and gains `Secure` when
  the request arrives with `X-Forwarded-Proto: https` — i.e. through a TLS
  proxy. On a plain-HTTP LAN install the flag is omitted so login still works.
- The WebSocket authenticates from the handshake cookie, which the browser
  sends over `wss` and the proxy forwards. The panel picks `wss` automatically
  when it is served over `https`.

So once the proxy is in place, nothing in the firmware needs changing.

## nginx (Synology / any host)

```nginx
server {
    listen 443 ssl;
    http2 on;
    server_name rotator.example.org;

    ssl_certificate     /etc/letsencrypt/live/rotator.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/rotator.example.org/privkey.pem;

    # Modern profile: TLS 1.3 (+1.2 fallback), no legacy ciphers.
    ssl_protocols       TLSv1.3 TLSv1.2;
    ssl_ciphers         ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384;
    ssl_prefer_server_ciphers off;
    add_header Strict-Transport-Security "max-age=63072000" always;

    location / {
        proxy_pass http://192.168.1.60;          # the bridge on the LAN
        proxy_set_header Host              $host;
        proxy_set_header X-Forwarded-Proto https; # tells the bridge to mark the cookie Secure
        proxy_set_header X-Forwarded-For   $remote_addr;

        # WebSocket upgrade, so wss reaches /ws
        proxy_http_version 1.1;
        proxy_set_header   Upgrade    $http_upgrade;
        proxy_set_header   Connection "upgrade";
        proxy_read_timeout 3600s;                 # the status socket is long-lived
    }
}
```

On Synology the same thing is available without editing nginx by hand: **Control
Panel → Login Portal → Advanced → Reverse Proxy**, create an entry from
`https://rotator.example.org` to `http://<bridge-ip>:80`, and enable *Custom
Header → WebSocket* so the `Upgrade`/`Connection` headers are passed. Add the
`X-Forwarded-Proto: https` custom header so the cookie is marked `Secure`. DSM
manages the certificate for you.

## Keeping the bridge off the open internet

Terminating TLS does not by itself make the bridge safe to expose publicly. It
still has a single session, an eight-character-minimum password and login
throttling, but no second factor. Keep it reachable only from the LAN or a VPN;
put the proxy on the same trusted network, not on a public port.
