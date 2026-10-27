// test_celio_device.mjs -- online play with no PC: the board's own Celio
// client (src/celio.c, run by port/gblink/celio_glue.cpp) at each end, the
// real Celio-Server in the middle, a GBA behind each simulated board.
//
// What a browser did in test_celio.mjs -- Celio-Client's link session, its
// Socket.IO client, WebUSB -- is all the firmware's here. This script only
// carries WebSocket frames between each simulator and the server, the way
// the board's network driver would, byte for byte: it parses none of them.
//
// Runs against the public server's code (9f02d3e, unmodified) and, when
// backport/verify-celio.sh has built it, backport/Celio-Server's patched one.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { SimDevice } from './sim_transport.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = process.argv[2];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const STATE = ['idle', 'connecting', 'entering', 'waiting', 'starting', 'linked', 'closed', 'failed'];

const servers = process.env.CELIO_ONLY === 'patched' ? [] : [['public', path.join(out, 'celio_server.mjs')]];
const patched = path.resolve(HERE, '../../build/backport/celio-patched/server/dist/index.js');
if (fs.existsSync(patched)) servers.push(['patched', patched]);

/** The board's network driver, played by node: a WebSocket per connect(true). */
function network(dev, url, log) {
  let ws = null;
  const tell = (...a) => { try { dev.send(...a); } catch { /* the simulator has gone */ } };
  dev.on('record', (kind, ep, body) => {
    if (kind !== 'W') return;
    if (ep === 1 && body[0] === 1 && !ws) {
      ws = new WebSocket(url);
      ws.onopen = () => tell('W', 1);
      ws.onmessage = (m) => {
        const t = String(m.data);
        if (process.env.CELIO_TRACE) log.push(`<- ${t.slice(0, 120)}\n`);
        // Celio picks the roles: the first board to report AwaitMode is
        // master. Give each simulated GBA the part that fits, before the
        // board hears it -- what two players' Cable Club visits would be.
        if (t.includes('"deviceCommand"') && !dev.role) {
          if (t.includes('"command":16')) { dev.role = 'master'; dev.gba(0x06, 1); dev.gba(0x01, 1); }
          if (t.includes('"command":17')) { dev.role = 'slave'; dev.gba(0x06, 1); dev.gba(0x01, 2); dev.gba(0x03, 6); }
        }
        tell('W', 0, Buffer.from(t));
      };
      ws.onclose = () => { ws = null; tell('W', 2); };
      ws.onerror = (e) => log.push(`ws error ${e.message || e}`);
    } else if (ep === 1 && body[0] === 0 && ws) {
      ws.close();
    } else if (ep === 0 && ws && ws.readyState === 1) {
      if (process.env.CELIO_TRACE) log.push(`-> ${body.toString().slice(0, 120)}\n`);
      ws.send(body.toString());
    }
  });
  return { close: () => ws?.close() };
}

/** gblink_celio_view(), through a 'K' record. */
function view(dev) {
  return new Promise((resolve) => {
    const on = (code, b) => {
      if (code !== 13) return;
      dev.off('event', on);
      const z1 = b.indexOf(0, 19);
      const z2 = b.indexOf(0, z1 + 1);
      resolve({
        state: STATE[b[0]] || b[0], status: b.readUInt16LE(1), up: b.readUInt32LE(3),
        down: b.readUInt32LE(7), lost: b.readUInt32LE(11), reconnects: b.readUInt32LE(15),
        session: b.subarray(19, z1).toString(), error: b.subarray(z1 + 1, z2).toString(),
      });
    };
    dev.on('event', on);
    dev.send('K', 0);
  });
}

async function until(pred, ms, what) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const v = await pred();
    if (v) return v;
    await sleep(100);
  }
  throw new Error(`timed out: ${what}`);
}

async function run(name, serverJs) {
  const server = spawn(process.execPath, [serverJs], { stdio: ['ignore', 'pipe', 'pipe'] });
  const log = [];
  server.stdout.on('data', (d) => log.push(d.toString()));
  server.stderr.on('data', (d) => log.push(d.toString()));
  await new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`Celio-Server (${name}) did not start:\n${log.join('')}`)), 15000);
    server.on('exit', (c) => { clearTimeout(t); reject(new Error(`Celio-Server exited ${c}:\n${log.join('')}`)); });
    server.stdout.on('data', (d) => { if (d.toString().includes('listening')) { clearTimeout(t); resolve(); } });
  });

  const url = 'ws://127.0.0.1:443/socket.io/?EIO=4&transport=websocket';
  const devA = new SimDevice();
  const devB = new SimDevice();
  await devA.ready; await devB.ready;
  const netA = network(devA, url, log);
  const netB = network(devB, url, log);

  let ok = false, why = '';
  try {
    devA.send('c', 0);                                       // create
    const a = await until(async () => { const v = await view(devA); return v.state === 'waiting' && v; },
                          10000, 'A in a session');
    if (a.session.length < 10) throw new Error(`session id "${a.session}"`);
    devB.send('c', 0, Buffer.from(a.session));               // join by the id A shows
    await until(async () => (await view(devA)).state === 'linked' && (await view(devB)).state === 'linked',
                10000, 'both linked');

    const frame = (tag) => [0xbb, 0xbb, tag & 0xff, tag >> 8, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    const deadline = Date.now() + 25000;
    let sent = false;
    while (Date.now() < deadline) {
      if (!sent && devA.words.length > 50 && devB.words.length > 50) {
        for (let i = 0; i < 6; i++) { devA.gba(0x02, ...frame(0xce01)); devB.gba(0x02, ...frame(0xce02)); }
        sent = true;
      }
      if (devA.words.some(([tx]) => tx === 0xce02) && devB.words.some(([tx]) => tx === 0xce01)) { ok = true; break; }
      await sleep(200);
    }
    const [va, vb] = [await view(devA), await view(devB)];
    if (!ok) why = `A (${devA.role}) ${JSON.stringify(va)} B (${devB.role}) ${JSON.stringify(vb)}`;
    else if (!devA.role || !devB.role || devA.role === devB.role) { ok = false; why = `roles ${devA.role}/${devB.role}`; }
    else if (va.lost || vb.lost) { ok = false; why = `frames lost: A ${va.lost}, B ${vb.lost}`; }

    // Leaving: A stops; the server closes the session for B.
    if (ok) {
      devA.send('c', 1);
      await until(async () => ['closed', 'waiting'].includes((await view(devB)).state), 10000,
                  'B told the session is over');
    }
  } catch (e) {
    ok = false;
    why = String(e.stack || e);
  } finally {
    netA.close(); netB.close();
    devA.kill(); devB.kill();
    server.kill();
  }
  if (!ok) console.log(`${why}\n${log.join('').split('\n').filter((l) => l !== '-> 3' && l !== '<- 2').slice(-60).join('\n')}`);
  console.log(`celio on-board     ${name} server: ${ok ? 'GBA <-> GBA, the board is the Celio client: frames delivered both ways' : 'FAILED'}`);
  return ok;
}

let all = true;
for (const [name, js] of servers) {
  if (!(await run(name, js))) all = false;
  await sleep(500);   // the port, freed
}
process.exit(all ? 0 : 1);
