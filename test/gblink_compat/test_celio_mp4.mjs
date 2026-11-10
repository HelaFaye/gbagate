// test_celio_mp4.mjs -- a Multi-Play link through the board's own Celio
// client, end to end: two simulated boards, one with three GBAs on its
// chain and one with one, four players in all, through the real Celio-Server
// with backport/Celio-Link-Server 0002.
//
// What runs for real: the board's Celio client in its multi mode
// (src/celio.c), the Multi-Play glue (src/mp_online.c) and the relay
// (src/mp_relay.c through src/mp_link.c), on the firmware's thread
// (port/gblink/celio_glue.cpp), and the server. What is simulated: the bus
// and the GBAs on it (test/gblink_sim/sim_mp4.c), at the games' protocol:
// handshake, 8FFF, frames of eight words with a shared checksum that every
// GBA checks. test/test_mpbus.c covers the bus itself.
//
// Checked: the board ready first gets link ID 0 and the other the slots
// after it (A 0-2 and B 3, or B 0 and A 1-3); every GBA reaches frames;
// each GBA's command reaches every other GBA, on its own cable and across
// the server; no GBA sees a checksum it disagrees with.
//
//   node test_celio_mp4.mjs <build>
import path from 'node:path';
import { SimDevice } from './sim_transport.mjs';
import { network, sleep, startServer, until, view } from './celio_harness.mjs';

const out = path.resolve(process.argv[2]);
const URL_ = 'ws://127.0.0.1:443/socket.io/?EIO=4&transport=websocket';
const PHASE = ['handshake', 'checksum slot', 'frames'];

/** The simulator's Multi-Play bus: layout and each GBA's state ('m' record). */
function mp4(dev) {
  return new Promise((resolve) => {
    const on = (code, b) => {
      if (code !== 15) return;
      dev.off('event', on);
      const gbas = [];
      for (let i = 0; i < b[2]; i++) {
        gbas.push({ slot: b[3] + i, phase: PHASE[b[8 + 4 * i]] ?? b[8 + 4 * i], got: b[9 + 4 * i], checksumErrors: b.readUInt16LE(10 + 4 * i) });
      }
      resolve({ on: !!b[0], players: b[1], locals: b[2], first: b[3], transfers: b.readUInt32LE(4), gbas });
    };
    dev.on('event', on);
    dev.send('m', 0);
  });
}

const log = [];
const server = await startServer(path.join(out, 'celio_server_patched.mjs'), log);
const A = new SimDevice(), B = new SimDevice();
await A.ready; await B.ready;
const netA = network(A, URL_, log), netB = network(B, URL_, log);
let ok = true;
const fail = (why) => { ok = false; console.log(`  FAIL ${why}`); };
try {
  A.send('c', 2, Buffer.from([4, 3]));                   // create: four GBAs, three ours
  const a = await until(async () => { const v = await view(A); if (v.state === 'failed') throw new Error(`A: ${v.error}`); return v.state === 'waiting' && v; },
                        30000, 'A in a session');
  B.send('c', 2, Buffer.concat([Buffer.from([2, 1]), Buffer.from(a.session)]));   // join with one
  await until(async () => (await mp4(A)).on && (await mp4(B)).on, 30000, 'both buses running');
  const [ma, mb] = [await mp4(A), await mp4(B)];
  // The server gives link ID 0 to whichever board reports AwaitMode first:
  // A (its GBAs 0-2, B's 3) or B (its GBA 0, A standing in for no one and
  // taking 1-3). Either is a right layout; nothing else is.
  const aFirst = ma.first === 0 && mb.first === 3, bFirst = mb.first === 0 && ma.first === 1;
  if (ma.locals !== 3 || mb.locals !== 1 || ma.players !== 4 || mb.players !== 4 || !(aFirst || bFirst))
    fail(`layouts A ${JSON.stringify(ma)} B ${JSON.stringify(mb)}`);
  else console.log(`  ok    ${aFirst ? "A's port 1 GBA" : "B's GBA"} is the parent (A slots ${ma.first}-${ma.first + 2}, B slot ${mb.first})`);

  const all = [0, 1, 2, 3].reduce((m, s) => m | (1 << s), 0);
  const done = await until(async () => {
    const [x, y] = [await mp4(A), await mp4(B)];
    for (const [n, d] of [['A', A], ['B', B]]) { const v = await view(d); if (v.state === 'failed' || v.state === 'closed') throw new Error(`${n} ${v.state}: ${v.error}`); }
    const gbas = [...x.gbas, ...y.gbas];
    return gbas.every((g) => g.phase === 'frames' && (g.got | (1 << g.slot)) === all) && { x, y };
  }, 60000, 'every GBA hearing every other').catch((e) => { fail(e.message); return null; });
  if (done) {
    for (const g of [...done.x.gbas, ...done.y.gbas])
      if (g.checksumErrors) fail(`slot ${g.slot}: ${g.checksumErrors} checksum errors`);
  }
  const [va, vb] = [await view(A), await view(B)];
  console.log(`  A: ${JSON.stringify(await mp4(A))}\n  B: ${JSON.stringify(await mp4(B))}`);
  if (va.lost || vb.lost) fail(`lost packets: A ${va.lost}, B ${vb.lost}`);
} catch (e) {
  fail(e.message || e);
  console.log(log.join('').split('\n').slice(-30).join('\n'));
} finally {
  netA.close(); netB.close();
  A.kill(); B.kill();
  server.kill();
  await sleep(300);
}
console.log(`celio Multi-Play   3 GBAs + 1 on two boards, through Celio-Server: ${ok ? 'every GBA hears every other, no checksum errors' : 'FAILED'}`);
process.exit(ok ? 0 : 1);
