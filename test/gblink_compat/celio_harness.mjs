// celio_harness.mjs -- two simulated boards, each its own Celio client
// (src/celio.c in build/gblink_sim), linked through a real Celio-Server, a
// GBA behind each. Used by test_celio_device.mjs (a clean network) and
// test_celio_latency.mjs (a bad one).
//
// This script plays the board's network driver: it carries WebSocket frames
// between each simulator and the server, and parses none of them except to
// set up the simulated GBAs' roles. Latency, when asked for, is added on
// exactly that leg -- the board's WiFi -- and nowhere else: not between the
// server and itself, not on the link cable.
import { spawn } from 'node:child_process';
import { SimDevice } from './sim_transport.mjs';

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const STATE = ['idle', 'connecting', 'entering', 'waiting', 'starting', 'linked', 'closed', 'failed'];

/** A small seeded generator, so a run with the same settings is the same run. */
function rng(seed) {
  let s = seed >>> 0 || 1;
  return () => { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; return (s >>> 0) / 4294967296; };
}

/**
 * A one-way delay line with TCP's one rule: frames come out in the order
 * they went in. Each frame waits `ms` plus up to `jitter` more; a stall of
 * `stallMs` every `stallEvery` ms holds everything (a WiFi rescan, a busy
 * access point). With all zero it is a straight wire.
 */
export function delayLine({ ms = 0, jitter = 0, stallEvery = 0, stallMs = 0, seed = 1 } = {}) {
  const r = rng(seed);
  const t0 = Date.now();
  let last = 0;
  return (deliver) => {
    const now = Date.now();
    let at = now + ms + r() * jitter;
    if (stallEvery && stallMs) {
      const into = (at - t0) % stallEvery;
      if (into < stallMs) at += stallMs - into;   /* inside a stall: after it */
    }
    at = Math.max(at, last);                      /* never overtake */
    last = at;
    if (at <= now) deliver(); else setTimeout(deliver, at - now);
  };
}

/** The board's network driver: a WebSocket per connect(true), each way through a delay line. */
export function network(dev, url, log, lat = {}, seed = 1) {
  let ws = null;
  const tell = (...a) => { try { dev.send(...a); } catch { /* the simulator has gone */ } };
  const up = delayLine({ ...lat, seed: seed * 2 + 1 });
  const down = delayLine({ ...lat, seed: seed * 2 + 2 });
  dev.on('record', (kind, ep, body) => {
    if (kind !== 'W') return;
    if (ep === 1 && body[0] === 1 && !ws) {
      const sock = new WebSocket(url);
      ws = sock;
      sock.onopen = () => down(() => tell('W', 1));
      sock.onmessage = (m) => {
        const t = String(m.data);
        if (process.env.CELIO_TRACE) log.push(`<- ${t.slice(0, 120)}\n`);
        down(() => {
          // Celio picks the roles: the first board to report AwaitMode is
          // master. Give each simulated GBA the part that fits, before the
          // board hears it -- what two players' Cable Club visits would be.
          if (t.includes('"deviceCommand"') && !dev.role) {
            if (t.includes('"command":16')) { dev.role = 'master'; dev.gba(0x06, 1); dev.gba(0x01, 1); }
            if (t.includes('"command":17')) { dev.role = 'slave'; dev.gba(0x06, 1); dev.gba(0x01, 2); dev.gba(0x03, 6); }
          }
          tell('W', 0, Buffer.from(t));
        });
      };
      sock.onclose = () => { if (ws === sock) ws = null; down(() => tell('W', 2)); };
      sock.onerror = (e) => log.push(`ws error ${e.message || e}\n`);
    } else if (ep === 1 && body[0] === 0 && ws) {
      ws.close();
    } else if (ep === 0 && ws) {
      const sock = ws, t = body.toString();
      if (process.env.CELIO_TRACE) log.push(`-> ${t.slice(0, 120)}\n`);
      up(() => { if (sock.readyState === 1) sock.send(t); });
    }
  });
  return { close: () => ws?.close() };
}

/** gblink_celio_view(), through a 'K' record. */
export function view(dev) {
  return new Promise((resolve) => {
    const on = (code, b) => {
      if (code !== 13) return;
      dev.off('event', on);
      const z1 = b.indexOf(0, 31);
      const z2 = b.indexOf(0, z1 + 1);
      resolve({
        state: STATE[b[0]] || b[0], status: b.readUInt16LE(1), up: b.readUInt32LE(3),
        down: b.readUInt32LE(7), lost: b.readUInt32LE(11), reconnects: b.readUInt32LE(15),
        resent: b.readUInt32LE(19), givenUp: b.readUInt32LE(23), maxRttMs: b.readUInt32LE(27),
        session: b.subarray(31, z1).toString(), error: b.subarray(z1 + 1, z2).toString(),
      });
    };
    dev.on('event', on);
    dev.send('K', 0);
  });
}

export async function until(pred, ms, what) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const v = await pred();
    if (v) return v;
    await sleep(100);
  }
  throw new Error(`timed out: ${what}`);
}

/** Start a Celio-Server bundle; resolves once it listens. */
export async function startServer(serverJs, log) {
  const server = spawn(process.execPath, [serverJs], { stdio: ['ignore', 'pipe', 'pipe'] });
  server.stdout.on('data', (d) => log.push(d.toString()));
  server.stderr.on('data', (d) => log.push(d.toString()));
  await new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`Celio-Server did not start:\n${log.join('')}`)), 15000);
    server.on('exit', (c) => { clearTimeout(t); reject(new Error(`Celio-Server exited ${c}:\n${log.join('')}`)); });
    server.stdout.on('data', (d) => { if (d.toString().includes('listening')) { clearTimeout(t); resolve(); } });
  });
  return server;
}

const frame = (tag) => [0xbb, 0xbb, tag & 0xff, tag >> 8, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];

/**
 * One online session: A creates, B joins by A's code, both link, and a
 * frame from each GBA must reach the other. Times each stage. lat applies
 * to both boards' WiFi; budget is how long the frames may take once linked.
 */
/*
 * One session between two devices already on a running server: A creates,
 * B joins by the code A shows, both link, each sends its own frames (tokA,
 * tokB) and must receive the other's, then A leaves. Several of these can
 * run at once on one server: the tokens are per pair, so a frame crossing
 * into another pair's link is caught (`foreign`).
 */
export async function playSession(devA, devB, { budgetMs = 25000, leave = true, tokA = 0xce01, tokB = 0xce02, foreign = [] } = {}) {
  const r = { ok: false, why: '', stage: 'connect' };
  const t0 = Date.now();
  try {
    devA.send('c', 0);                                       // create
    const a = await until(async () => { const v = await view(devA); if (v.state === 'failed') throw new Error(`A failed: ${v.error}`); return v.state === 'waiting' && v; },
                          30000, 'A in a session');
    if (!/^\d{4}$/.test(a.session)) throw new Error(`session code "${a.session}"`);
    r.session = a.session;
    r.sessionMs = Date.now() - t0;
    r.stage = 'link';
    devB.send('c', 0, Buffer.from(a.session));               // join by the code A shows
    await until(async () => {
      const [va, vb] = [await view(devA), await view(devB)];
      for (const [n, v] of [['A', va], ['B', vb]]) if (v.state === 'failed' || v.state === 'closed') throw new Error(`${n} ${v.state}: ${v.error}`);
      return va.state === 'linked' && vb.state === 'linked';
    }, 30000, 'both linked');
    r.linkMs = Date.now() - t0;
    r.stage = 'frames';
    const deadline = Date.now() + budgetMs;
    let sent = false;
    while (Date.now() < deadline) {
      for (const [n, d] of [['A', devA], ['B', devB]]) {
        const v = await view(d);
        if (v.state === 'failed' || v.state === 'closed') throw new Error(`${n} ${v.state} mid-link: ${v.error}`);
      }
      if (!sent && devA.words.length > 50 && devB.words.length > 50) {
        for (let i = 0; i < 6; i++) { devA.gba(0x02, ...frame(tokA)); devB.gba(0x02, ...frame(tokB)); }
        sent = true;
        r.sentAt = Date.now();
      }
      if (devA.words.some(([tx]) => tx === tokB) && devB.words.some(([tx]) => tx === tokA)) { r.ok = true; break; }
      await sleep(200);
    }
    r.framesMs = Date.now() - t0;
    if (!r.ok) throw new Error('frames did not cross in time');
    const leak = [devA, devB].some((d) => d.words.some(([tx]) => foreign.includes(tx)));
    if (leak) { r.ok = false; throw new Error('a frame from another session arrived here'); }
    if (!devA.role || !devB.role || devA.role === devB.role) { r.ok = false; throw new Error(`roles ${devA.role}/${devB.role}`); }
    if (leave) {
      r.stage = 'leave';
      devA.send('c', 1);
      await until(async () => ['closed', 'waiting'].includes((await view(devB)).state), 20000, 'B told the session is over');
    }
  } catch (e) {
    r.ok = false;
    r.why = String(e.message || e);
  }
  r.roles = `${devA.role}/${devB.role}`;
  return r;
}

const URL_ = 'ws://127.0.0.1:443/socket.io/?EIO=4&transport=websocket';

export async function runPair(serverJs, { lat = {}, budgetMs = 25000, seed = 1, leave = true } = {}) {
  const log = [];
  const server = await startServer(serverJs, log);
  const devA = new SimDevice();
  const devB = new SimDevice();
  await devA.ready; await devB.ready;
  const netA = network(devA, URL_, log, lat, seed);
  const netB = network(devB, URL_, log, lat, seed + 100);
  let r;
  try {
    r = await playSession(devA, devB, { budgetMs, leave });
  } finally {
    r = r || { ok: false, why: 'harness', stage: 'connect' };
    r.log = log;
    try { r.a = await Promise.race([view(devA), sleep(1000)]); r.b = await Promise.race([view(devB), sleep(1000)]); } catch { /* gone */ }
    netA.close(); netB.close();
    devA.kill(); devB.kill();
    server.kill();
    await sleep(500);   // the port, freed
  }
  return r;
}

/*
 * Several sessions at once on one server: pairs[i] is that pair's latency.
 * With `intruder`, one more device tries to join pair 0's session once it is
 * full, and must be refused while pair 0 plays on.
 */
export async function runCrowd(serverJs, { pairs = [{}, {}], budgetMs = 40000, seed = 1, intruder = true } = {}) {
  const log = [];
  const server = await startServer(serverJs, log);
  const devs = [], nets = [];
  const out = { log, pairs: [], intruder: null };
  try {
    for (const [i, lat] of pairs.entries()) {
      const a = new SimDevice(), b = new SimDevice();
      await a.ready; await b.ready;
      nets.push(network(a, URL_, log, lat, seed + 10 * i), network(b, URL_, log, lat, seed + 10 * i + 5));
      devs.push([a, b]);
    }
    const toks = pairs.map((_, i) => [0xc100 + 2 * i, 0xc101 + 2 * i]);
    const runs = devs.map(([a, b], i) => playSession(a, b, {
      budgetMs, leave: false, tokA: toks[i][0], tokB: toks[i][1],
      foreign: toks.filter((_, j) => j !== i).flat(),
    }));
    let intr;
    if (intruder) {
      intr = new SimDevice();
      await intr.ready;
      nets.push(network(intr, URL_, log, {}, seed + 999));
      devs.push([intr]);
      // once pair 0 is linked, try to take its place
      await until(async () => (await view(devs[0][0])).state === 'linked' || null, 60000, 'pair 0 linked').catch(() => {});
      const code = (await view(devs[0][0])).session;
      intr.send('c', 0, Buffer.from(code));
      out.intruder = await until(async () => { const v = await view(intr); return ['failed', 'linked', 'closed'].includes(v.state) && v; }, 30000, 'intruder answered')
        .catch((e) => ({ state: 'timeout', error: String(e.message || e) }));
    }
    out.pairs = await Promise.all(runs);
    for (const [i, [a, b]] of devs.slice(0, pairs.length).entries()) {
      out.pairs[i].a = await view(a); out.pairs[i].b = await view(b);
    }
  } finally {
    for (const n of nets) n.close();
    for (const d of devs.flat()) d.kill();
    server.kill();
    await sleep(500);
  }
  return out;
}
