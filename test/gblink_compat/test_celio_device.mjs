// test_celio_device.mjs -- online play with no PC: the board's own Celio
// client (src/celio.c, run by port/gblink/celio_glue.cpp) at each end, the
// real Celio-Server in the middle, a GBA behind each simulated board.
//
// What a browser did in test_celio.mjs -- Celio-Client's link session, its
// Socket.IO client, WebUSB -- is all the firmware's here. celio_harness.mjs
// only carries WebSocket frames between each simulator and the server, the
// way the board's network driver would.
//
// Runs against Celio-Link/Celio-Server, the server Celio-Client's own
// production setting points at (run.sh pins it): acked emits both ways,
// data in batches, four-digit session codes. A clean network here;
// test_celio_latency.mjs makes it a bad one.
import path from 'node:path';
import { runPair } from './celio_harness.mjs';

const out = process.argv[2];
const r = await runPair(path.join(out, 'celio_server.mjs'));
if (!r.ok) {
  console.log(`${r.why} (stage ${r.stage})\nA ${JSON.stringify(r.a)}\nB ${JSON.stringify(r.b)}`);
  console.log(r.log.join('').split('\n').filter((l) => l !== '-> 3' && l !== '<- 2').slice(-40).join('\n'));
}
console.log(`celio on-board     Celio-Link server: ${r.ok ? 'GBA <-> GBA, the board is the Celio client: frames delivered both ways' : 'FAILED'}`);
process.exit(r.ok ? 0 : 1);
