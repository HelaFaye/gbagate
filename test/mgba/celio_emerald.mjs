// celio_emerald.mjs -- two Pokemon Emeralds linking online through our
// firmware: each game on mGBA (link_child.py), on a Multi-Player cable
// (test/gblink_sim/mp_wire.h) to its own simulated board running the
// firmware's Gen 3 link mode and the board's own Celio client, the two
// boards in one session on the real Celio-Server.
//
//   node test/mgba/celio_emerald.mjs <emerald.gba> <out-dir> [one-way ms]
//
// Checked: the boards make the session and link; both games count two
// players; the parent connects; both receive the other's link player
// record, so each holds both trainer IDs in link order; no link errors and
// nothing lost. Then each board's Link: Online page is drawn, with its live
// counters, by the device's own renderer (build/online_screen).
import path from 'node:path';
import fs from 'node:fs';
import os from 'node:os';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { startServer, network, view, until, sleep } from '../gblink_compat/celio_harness.mjs';
import { SimDevice } from '../gblink_compat/sim_transport.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const BUILD = path.join(ROOT, 'build/gblink_compat');
const [rom, out = path.join(ROOT, 'build/emerald'), msArg] = process.argv.slice(2);
if (!rom || !fs.existsSync(rom)) { console.log('emerald online  skipped: no ROM'); process.exit(77); }
fs.mkdirSync(out, { recursive: true });
const lat = { ms: Number(msArg ?? 20), jitter: Number(msArg ?? 20) / 4 };
const URL_ = 'ws://127.0.0.1:443/socket.io/?EIO=4&transport=websocket';

let fails = 0;
const check = (what, ok) => { console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${what}`); if (!ok) fails++; };

function child(wire, delay, tag) {
  const p = spawn('python3', ['-I', path.join(HERE, 'link_child.py'), rom, wire, String(delay), '90',
                              path.join(out, `online-${tag}-game.png`)], { stdio: ['ignore', 'pipe', 'inherit'] });
  const lines = [];
  let buf = '';
  p.stdout.on('data', (d) => {
    buf += d;
    let i;
    while ((i = buf.indexOf('\n')) >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); if (l.startsWith('{')) lines.push(JSON.parse(l)); }
  });
  const done = new Promise((r) => p.on('exit', r));
  return { p, lines, done };
}

console.log(`emerald online, through our firmware and Celio (${lat.ms} ms each way)`);
const log = [];
const server = await startServer(path.join(BUILD, 'celio_server.mjs'), log);
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mpwire-'));
const wires = ['a', 'b'].map((t) => path.join(tmp, `${t}.wire`));
const devs = wires.map((w) => new SimDevice({ env: { GBLINK_MP_WIRE: w } }));
for (const d of devs) await d.ready;
const nets = devs.map((d, i) => network(d, URL_, log, lat, 5 + i));
const kids = [child(wires[0], 0, 'a'), child(wires[1], 97, 'b')];
let views = [];
try {
  await until(() => kids.every((k) => k.lines.some((l) => l.ready)), 120000, 'both games at the overworld');
  const ids = kids.map((k) => k.lines.find((l) => l.ready).trainer_id);
  check(`two different trainers (${ids})`, ids[0] !== ids[1]);

  devs[0].send('c', 0);                                    // Link: Online, a new session
  const a = await until(async () => { const v = await view(devs[0]); return v.state === 'waiting' && v; }, 30000, 'A in a session');
  devs[1].send('c', 0, Buffer.from(a.session));           // the other joins by its code
  await until(async () => (await view(devs[0])).state === 'linked' && (await view(devs[1])).state === 'linked', 30000, 'both boards linked');
  check(`the boards made session ${a.session} and linked`, true);

  await Promise.all(kids.map((k) => k.done));
  views = await Promise.all(devs.map((d) => view(d)));
  const res = kids.map((k) => k.lines.filter((l) => !l.ready).at(-1) || {});
  for (const [n, r] of res.entries()) {
    console.log(`    game ${'AB'[n]}: ${JSON.stringify(r)}`);
    console.log(`    board ${'AB'[n]}: ${JSON.stringify(views[n])}`);
  }
  check('both games counted two players', res.every((r) => r.status?.count === 2));
  check('exactly one is the parent', res.filter((r) => r.status?.master).length === 1);
  check('both received the other\'s link player record', res.every((r) => r.received));
  const order = res[0].players;
  check(`each holds both trainers, the same link order on both (${order})`,
        order && [...order].sort().join() === [...ids].sort().join() && JSON.stringify(res[1].players) === JSON.stringify(order));
  check('no link errors', res.every((r) => r.status && r.status.errors === 0));
  check('the cables ran, nothing unanswered once linked', res.every((r) => r.cable?.transfers > 0));
  check('the boards lost nothing', views.every((v) => (v.lost ?? 0) === 0 && (v.givenUp ?? 0) === 0));
  for (const [n, r] of res.entries()) console.log(`    game ${'AB'[n]} linked ${r.done_s?.toFixed(1)} s after OpenLink`);
} catch (e) {
  check(`no exception (${e.message})`, false);
  console.log(log.join('').split('\n').slice(-30).join('\n'));
} finally {
  for (const [n, d] of devs.entries()) {
    const v = views[n] || (await Promise.race([view(d), sleep(1000)])) || {};
    const st = { waiting: 3, starting: 4, linked: 5, closed: 6, failed: 7 }[v.state] ?? 1;
    const ppm = path.join(out, `online-${'ab'[n]}-board.ppm`);
    try {
      const lines = execFileSync(path.join(ROOT, 'build/online_screen'),
        [ppm, String(st), v.session || '', String(v.status ?? 0), String(v.up ?? 0), String(v.down ?? 0),
         String(v.resent ?? 0), String((v.lost ?? 0) + (v.givenUp ?? 0)), String(v.maxRttMs ?? 0)]).toString().trim();
      execFileSync('python3', [path.join(ROOT, 'tools/ppm2png.py'), ppm, ppm.replace(/\.ppm$/, '.png')]);
      console.log(`    board ${'AB'[n]}'s Link: Online page: ${lines.split('\n').join(' | ')}`);
    } catch (e) { console.log(`    (no screen: ${e.message})`); }
  }
  for (const k of kids) k.p.kill();
  for (const n of nets) n.close();
  for (const d of devs) d.kill();
  server.kill();
}
console.log(`emerald online  ${fails ? 'FAILURES' : 'all checks passed'}`);
process.exit(fails ? 1 : 0);
