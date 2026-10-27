# Online play and WiFi

The board can join a WiFi network and do two things on it with no PC:

- **Link: Online.** It plays a Gen 3 link (Pokémon trades and battles, the
  Cable Club) with a partner anywhere, through a
  [Celio](https://github.com/Celio-Link) server. The board is its own Celio
  client: what the Celio-Client web page does for a GB-Link adapter
  (Socket.IO to the server, the session, the adapter's mode) is in
  `src/celio.c`, and the adapter is the board's own GB-Link firmware.
- **`/gblink`.** It is a GB-Link adapter to any host on the same network,
  over a WebSocket, as it is one over USB.

Neither needs USB. Both are in the `link`, `gate` and `full` images (the ones
with the radio); `mini` has no WiFi.

## Setting up WiFi

Either way below writes the same settings. The board keeps them in NVS, so
it does this once.

### From a phone: the setup hotspot

Choose **Link → Online - Celio** on a board with no network stored. The
screen names a hotspot, `gbagate-XXXX`, where `XXXX` comes from the board's
MAC. Join it from a phone, open **http://192.168.4.1/**, and fill in the form:

| field | what |
|---|---|
| WiFi network | your network's name (up to 32 bytes) |
| Password | 8–63 characters, or 64 hex digits; empty for an open network. The page never shows a saved password. If you leave the box empty, the saved password is kept. |
| Celio server | `wss://celio-server.up.railway.app:443` (Celio-Client's own server) unless you run one |
| Join a session | your partner's code, or empty to start a session |
| Hotspot password | to stop anyone else using the setup page; empty leaves it open |

The board joins at once. If it cannot (a wrong password, a 5 GHz-only
network: the ESP32-S3 is 2.4 GHz only), the hotspot comes back after four
tries, so you can fix it from the same phone.

### From a PC: `wifi.txt`

Set **Settings → usb** to `drive`, restart, and the board is a USB drive.
Put a `wifi.txt` in its root:

```
# gbagate WiFi
ssid     = Home Network
password = hunter22
celio    = wss://celio-server.up.railway.app:443
session  = V1StGXR8_Z5jdHi6B-myT
```

- One `key = value` per line. Keys are case-insensitive. Blank lines and
  lines starting with `#` or `;` are ignored, and CRLF and a UTF-8 BOM are
  fine.
- Put a value in double quotes to keep spaces at its ends.
- A key the file leaves out keeps its stored value. A file with only a
  `session` line changes only the session.
- `hotspot_password` works here too.

The board reads the file at boot, and again whenever a PC ejects the drive.
It takes the file only when it has changed since the last time it took it, so
a later change made on the setup page is not overwritten at every boot.
Unknown keys and bad values are skipped. The serial log names the first line
it did not like.

The file keeps the password in plain text on the drive. Delete it once the
board has taken it if that matters to you.

## Playing online

1. **Both players** choose **Link → Online - Celio**.
2. **One** of you leaves *Join a session* empty. After *finding Celio...*
   that board shows *waiting for partner* and the session code, split over
   two lines.
3. **The other** puts that code in *Join a session* (on the setup page, at
   the address the screen shows, or in `wifi.txt`), then chooses Link: Online.
   A Celio-Client web page can join the same code: the two are compatible.
4. Both screens go to *partner joined*, then *playing online*. Go to the Cable
   Club on both GBAs.

The server decides which board is master: the first to report itself ready.
As with Celio-Client, the GBAs must suit the roles. That is what two players
going to the Cable Club together do.

A code joins once: the board forgets it when the link comes up. If a session
ends or fails, the screen says why, and **confirm** starts again.

What the board speaks: the Socket.IO protocol of Celio-Client before b057315,
the one the public server (Celio-Server 9f02d3e) answers. Status and data go
up without acknowledgements. Anything the server sends with an ack id is
acknowledged, so the patched server in `backport/Celio-Server/`, which serves
both kinds of client, works too.

## `/gblink`: an adapter on the network

While the board is on a network, `ws://<its address>/gblink` is a GB-Link
host (PATCHES.md P15). Binary WebSocket messages carry the CDC port's byte
stream exactly: the same `GB`-framed commands, data and status that WebSerial
carries. So anything that speaks GB-Link over a serial port can speak it here
by swapping the transport. One host at a time; a new connection replaces the
old one. The Link: USB page shows it as *via WiFi*.

**Browsers and `ws://`.** The GB-Link web apps are served over https, and a
page served over https may not open a plain `ws://` socket (mixed content).
So `/gblink` serves:

- tools that run outside a browser: the netplay bridge and Node scripts;
- web apps run from `http://localhost` (their dev servers), which browsers
  let open `ws://`.

Serving TLS from the board would need a certificate the browser trusts for a
LAN address, which it cannot have. That is a limit of browsers, not of the
board.

A WebSerial stand-in, for a page served over http:

```js
// Gives GB-Link code that expects a WebSerial port the board's /gblink.
function gbagatePort(host) {
  const ws = new WebSocket(`ws://${host}/gblink`);
  ws.binaryType = 'arraybuffer';
  const readable = new ReadableStream({
    start(c) { ws.onmessage = (m) => c.enqueue(new Uint8Array(m.data)); ws.onclose = () => c.close(); },
  });
  const writable = new WritableStream({ write(chunk) { ws.send(chunk); } });
  return { readable, writable, open: () => new Promise((r) => (ws.readyState === 1 ? r() : (ws.onopen = r))) };
}
```

## How it is tested

| what | where |
|---|---|
| The Celio client against scripted server frames: framing, create and join, Cancel/SetMode/DeviceReady, reordering, duplicates, a dropped socket, junk | `test/test_celio.c` |
| The same code, in the GB-Link simulator, linking two GBAs through the **real** Celio-Server, both the public build and the patched one | `test/gblink_compat/test_celio_device.mjs` |
| `wifi.txt` and the form: untidy files, every field's limits, the stored blob under every single-bit flip, the setup page escaped and never echoing a password | `test/test_wificfg.c` |
| The bounded JSON reader | `test/test_json.c` |
| `/gblink` as a GB-Link host: SetMode over it, replies on it and nowhere else | `wifi_host_runs_a_link_mode` in `test/gblink_sim/test_gblink.py` |
| Every Link: Online screen state | `test/test_screens.c` |

**Not yet run on hardware:** the ESP-IDF layer in
`port/esp32s3/net_esp32s3.c` (joining, the hotspot, the HTTP server, the TLS
socket). CI compiles it for every image.
