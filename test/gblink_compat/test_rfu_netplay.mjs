// test_rfu_netplay.mjs -- the Wireless Adapter (mode 0x07) across a netplay
// session, run by GB-Link's netplay bridge engine (GBLinkEngine, unmodified,
// game 'rfu': GBLinkRfuDevice + RfuLinkBridge) at both ends, with pret's
// librfu as the game on each simulated GBA (test/gblink_sim/rfu_game.cpp).
//
//   librfu <-> gbagate sim <-> GBLinkEngine host --TCP netplay--
//       GBLinkEngine join <-> gbagate sim <-> librfu
//
// Pass: the joining GBA finds the host's room, connects, and a payload
// crosses each way. Skips (exit 0) when librfu was not fetched, unless
// GBLINK_NO_SKIP is set.
import { SimDevice, SimTransport } from './sim_transport.mjs';

const engine = await import(`${process.argv[2]}/engine.mjs`);
const { GBLinkEngine, GBLinkAdapter } = engine;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const port = 58000 + Math.floor(Math.random() * 2000);

const EV_RFU = 11;
const REQ = { RESET: 0x10, SYSTEM_STATUS: 0x13, GAME_CONFIG: 0x16, SYSTEM_CONFIG: 0x17,
  SC_START: 0x19, SC_POLL: 0x1a, SP_START: 0x1c, SP_POLL: 0x1d, SP_END: 0x1e,
  CP_START: 0x1f, CP_POLL: 0x20, CP_END: 0x21, DATA_TX: 0x24, DATA_RX: 0x26, MS_CHANGE: 0x27 };

class Skip extends Error {}

/** librfu on one simulated GBA: the same ops test_rfu.py uses. */
class Game {
  constructor(dev) {
    this.dev = dev;
    this.events = [];
    this.waiters = [];
    dev.on('event', (ep, body) => {
      if (ep !== EV_RFU) return;
      this.events.push(body);
      for (const w of this.waiters.splice(0)) w();
    });
  }
  async next(pred, timeoutMs, from) {
    const end = Date.now() + timeoutMs;
    for (;;) {
      for (let i = from; i < this.events.length; i++) {
        if (this.events[i][0] === 0) throw new Skip('librfu not built in (test/gblink_sim/fetch_librfu.sh)');
        if (pred(this.events[i])) return this.events[i];
      }
      const left = end - Date.now();
      if (left <= 0) throw new Error('librfu: no reply');
      await new Promise((r) => { this.waiters.push(r); setTimeout(r, left); });
    }
  }
  async checkId() {
    const mark = this.events.length;
    this.dev.gba(0x20, 8);
    return (await this.next((e) => e[0] === 1, 15000, mark)).readInt32LE(1);
  }
  async ok(id, args = []) {
    const mark = this.events.length;
    this.dev.gba(0x21, id, ...args);
    const e = await this.next((b) => b[0] === 2 && b[1] === id, 5000, mark);
    const err = e.readUInt16LE(2);
    if (err) throw new Error(`REQ 0x${id.toString(16)} error ${err}`);
    const words = [];
    for (let i = 4; i + 4 <= e.length; i += 4) words.push(e.readUInt32LE(i));
    return words.slice(1);
  }
  async waitData(timeoutMs = 5000) {
    const end = Date.now() + timeoutMs;
    while (Date.now() < end) {
      const mark = this.events.length;
      await this.ok(REQ.MS_CHANGE);
      const e = await this.next((b) => b[0] === 3, 5000, mark);
      if (e[1] === 0x28 && e[2] === 0) return;   // a data event, not "no response"
    }
    throw new Error('no data event');
  }
}

const u16 = (v) => [v & 0xff, v >> 8];
const u32 = (v) => [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff];
const bytesOf = (words) => Buffer.concat(words.map((w) => Buffer.from(u32(w))));

const devHost = new SimDevice();
const devJoin = new SimDevice();
const gHost = new Game(devHost);
const gJoin = new Game(devJoin);

const log = [];
const host = new GBLinkEngine(() => new GBLinkAdapter(new SimTransport(devHost)));
const join = new GBLinkEngine(() => new GBLinkAdapter(new SimTransport(devJoin)));
for (const [name, e] of [['host', host], ['join', join]]) {
  e.on('link-phase', (p) => log.push(`${name} phase ${p}`));
  e.on('session', (s) => log.push(`${name} session ${s.phase}`));
  e.on('warn', (m) => log.push(`${name} warn ${m}`));
  e.on('log', (m) => log.push(`${name} log ${m}`));
  e.on('error', (err) => log.push(`${name} error ${err.message}`));
}

let verdict = 'FAILED';
let code = 1;
try {
  await host.start({ mode: 'host', port, nick: 'Brock', game: 'rfu' });
  await sleep(300);
  await join.start({ mode: 'join', host: '127.0.0.1', port, nick: 'Ash', game: 'rfu' });
  await sleep(1500);   // GBLinkRfuDevice: firmware check, probe, SetMode 0x07

  if (await gHost.checkId() !== 0x8001 || await gJoin.checkId() !== 0x8001) throw new Error('checkID');
  await gHost.ok(REQ.RESET);
  await gHost.ok(REQ.SYSTEM_CONFIG, [...u16(0), 4, 32]);
  const name = Buffer.from('GBAGATE NET\0\0\0', 'latin1');
  const user = Buffer.from('BROCK\0\0\0', 'latin1');
  await gHost.ok(REQ.GAME_CONFIG, [...u16(0x0202), ...name, ...user]);
  await gHost.ok(REQ.SC_START);

  await gJoin.ok(REQ.RESET);
  await gJoin.ok(REQ.SP_START);
  let found = [];
  for (let i = 0; i < 40 && found.length < 7; i++) { await sleep(250); found = await gJoin.ok(REQ.SP_POLL); }
  if (found.length < 7) throw new Error('the joining GBA found no room');
  if (!bytesOf(found.slice(1, 7)).subarray(2, 13).equals(name.subarray(0, 11))) throw new Error('room name');
  await gJoin.ok(REQ.SP_END);
  await gJoin.ok(REQ.CP_START, u16(found[0] & 0xffff));
  let st = 0x01000000;
  for (let i = 0; i < 40 && st === 0x01000000; i++) { await sleep(100); [st] = await gJoin.ok(REQ.CP_POLL); }
  if (st & 0xff000000) throw new Error(`connect: 0x${st.toString(16)}`);
  const slot = (st >> 16) & 0xff;
  await gJoin.ok(REQ.CP_END);

  const down = Buffer.from('HOST->JOIN 2.2.7', 'latin1');
  await gHost.ok(REQ.DATA_TX, [...u32(down.length), ...down]);
  let rx = [0];
  for (let i = 0; i < 12 && rx[0] !== down.length; i++) { await gJoin.waitData(); rx = await gJoin.ok(REQ.DATA_RX); }
  if (rx[0] !== down.length || !bytesOf(rx.slice(1)).subarray(0, down.length).equals(down)) throw new Error('host -> join payload');

  const up = Buffer.from('JOIN->HOST ok!', 'latin1');
  await gJoin.ok(REQ.DATA_TX, [...u32(up.length << (8 + slot * 5)), ...up]);
  let n = 0;
  for (let i = 0; i < 12 && n !== up.length; i++) {
    await gHost.waitData();
    rx = await gHost.ok(REQ.DATA_RX);
    n = (rx[0] >> (8 + slot * 5)) & 0x1f;
  }
  if (n !== up.length || !bytesOf(rx.slice(1)).subarray(0, n).equals(up)) throw new Error('join -> host payload');

  verdict = 'room found, joined, payloads both ways over RetroArch netplay';
  code = 0;
} catch (e) {
  // GBLINK_NO_SKIP (set in CI): a test that cannot run is a failure.
  if (e instanceof Skip && !process.env.GBLINK_NO_SKIP) { verdict = `skipped: ${e.message}`; code = 0; }
  else if (e instanceof Skip) { verdict = `FAILED: required, but ${e.message}`; }
  else log.push(`exception ${e.stack || e}`);
} finally {
  await join.stop().catch(() => {});
  await host.stop().catch(() => {});
  devHost.kill(); devJoin.kill();
}

if (code) console.log(log.slice(-40).join('\n'));
console.log(`rfu netplay         ${verdict}`);
process.exit(code);
