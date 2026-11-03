// test_celio_4p.mjs -- four-player Celio sessions (backport/Celio-Link-Server
// 0002) end to end: the real server bundle, four socket.io clients, each on
// its own line (ordered delay each way, as celio_harness's delay line).
//
// Each client plays a board's part of the protocol -- create or join with
// {players: 4}, report AwaitMode / HandshakeReceived / LinkConnected as the
// server's commands come, ack everything the server sends -- and sends 30
// data packets. Checked:
//   - one master, three slaves; StartHandshake to all four only once all
//     four have reported a handshake; ConnectLink to the other three;
//   - every player receives each other player's 30 packets, marked with the
//     sender's slot, and none of its own (deduplicated by sender and
//     sequence, as a receiver does: a slow line gets resent copies);
//   - a fifth client is refused ("Session is full");
//   - a two-player session on the same server at the same time is as before:
//     its packets carry no `from`.
//
//   node test_celio_4p.mjs <build>
import path from 'node:path';
import { createRequire } from 'node:module';
import { sleep, startServer } from './celio_harness.mjs';

const out = path.resolve(process.argv[2]);
const require = createRequire(path.join(out, 'node_modules', 'x.js'));
const { io } = require('socket.io-client');
const URL_ = 'http://127.0.0.1:443';
const S = { AwaitMode: 0xFF02, HandshakeReceived: 0xFF03, LinkConnected: 0xFF05 };
const C = { SetModeMaster: 0x10, SetModeSlave: 0x11, StartHandshake: 0x12, ConnectLink: 0x13 };
const PACKETS = 30;

let fails = 0;
const check = (what, ok) => { console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${what}`); if (!ok) fails++; };

// An ordered delay line: f runs `ms` (+ jitter) later, never before an
// earlier one on the same line. One queue and one timer, drained in order:
// separate setTimeouts due in the same millisecond can fire out of order.
function line(ms, jitter, seed) {
  let s = seed, last = 0, timer = null;
  const q = [];
  const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff; };
  const drain = () => {
    timer = null;
    while (q.length && q[0].at <= Date.now()) q.shift().f();
    if (q.length) timer = setTimeout(drain, Math.max(0, q[0].at - Date.now()));
  };
  return (f) => {
    const at = Math.max(Date.now() + ms + jitter * rnd(), last);
    last = at;
    q.push({ at, f });
    if (!timer) timer = setTimeout(drain, Math.max(0, q[0].at - Date.now()));
  };
}

class Player {
  constructor(name, ms, seed) {
    this.name = name;
    this.up = line(ms, ms / 4, seed);           // to the server
    this.down = line(ms, ms / 4, seed + 7);     // from it
    this.commands = []; this.events = []; this.packets = []; this.uuid = 0;
    this.sock = io(URL_, { transports: ['websocket'], auth: { clientId: `${name}-${Date.now()}` }, reconnection: false });
    const on = (ev, f) => this.sock.on(ev, (data, ack) => this.down(() => { f(data); if (typeof ack === 'function') this.up(() => ack(true)); }));
    on('deviceCommand', (c) => {
      this.commands.push(c.command);
      if (c.command === C.SetModeMaster || c.command === C.SetModeSlave) this.status(S.HandshakeReceived);
      if (c.command === C.StartHandshake) this.status(S.LinkConnected);
    });
    on('deviceData', (batch) => this.packets.push(...batch));
    for (const ev of ['partnerJoined', 'partnerLeft', 'sessionClose']) on(ev, () => this.events.push(ev));
  }
  ready() { return new Promise((r, j) => { this.sock.on('connect', r); this.sock.on('connect_error', j); }); }
  // an emit with an ack, both ways through the line
  call(ev, ...args) {
    return new Promise((r) => this.up(() => this.sock.emit(ev, ...args, (res) => this.down(() => r(res)))));
  }
  status(linkStatus) { return this.call('deviceStatus', { uuid: `${this.name}-${this.uuid++}`, linkStatus }); }
  async sendData() {
    for (let seq = 0; seq < PACKETS; seq++) {
      const data = new Array(32).fill(0); data[0] = seq; data[1] = this.slot;
      await this.call('deviceData', [{ sequence: seq, data }]);
    }
  }
  close() { this.sock.close(); }
}

async function until(pred, ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) { if (pred()) return true; await sleep(50); }
  return false;
}

async function run(serverJs, lines) {
  const log = [];
  const server = await startServer(serverJs, log);
  const players = lines.map((ms, i) => new Player(`p${i}`, ms, 11 + i));
  const pair = [new Player('q0', 20, 91), new Player('q1', 20, 92)];
  let intruder;
  try {
    await Promise.all([...players, ...pair].map((p) => p.ready()));
    const made = await players[0].call('sessionCreate', { players: 4 });
    check(`created for four (${JSON.stringify(made.value)})`, made.variant === 'Ok' && made.value.players === 4 && made.value.slot === 0);
    players[0].slot = 0;
    const code = made.value.id;
    for (let i = 1; i < 4; i++) {
      const j = await players[i].call('sessionJoin', code);
      players[i].slot = j.value?.slot;
      check(`player ${i} joins as slot ${j.value?.slot}${j.value?.full ? ', full' : ''}`, j.variant === 'Ok' && j.value.slot === i && j.value.full === (i === 3));
    }
    intruder = new Player('p4', 20, 99);
    await intruder.ready();
    const no = await intruder.call('sessionJoin', code);
    check(`a fifth is refused (${no.error})`, no.variant === 'Err' && /full/i.test(no.error));

    // a two-player session at the same time, by an older client (no options)
    const pm = await pair[0].call('sessionCreate', null);
    const pj = await pair[1].call('sessionJoin', pm.value.id);
    pair[0].slot = 0; pair[1].slot = 1;   // for the packets' contents; the server sends them no `from`
    check('a two-player session beside it, created the old way', pm.variant === 'Ok' && pm.value.players === 2 && pj.value?.full === true);

    await until(() => players.slice(0, 3).every((p) => p.events.includes('partnerJoined')), 15000);
    check('the three waiting are told the session is complete', players.slice(0, 3).every((p) => p.events.includes('partnerJoined')));
    for (const p of [...players, ...pair]) await p.status(S.AwaitMode);
    await until(() => players.every((p) => p.commands.includes(C.StartHandshake)), 30000);
    const modes = players.map((p) => p.commands.find((c) => c === C.SetModeMaster || c === C.SetModeSlave));
    check(`one master, three slaves (${modes.map((m) => (m === C.SetModeMaster ? 'M' : 'S')).join('')})`,
          modes.filter((m) => m === C.SetModeMaster).length === 1 && modes.filter((m) => m === C.SetModeSlave).length === 3);
    check('StartHandshake reached all four', players.every((p) => p.commands.includes(C.StartHandshake)));
    await until(() => players.every((p) => p.commands.filter((c) => c === C.ConnectLink).length === 3), 30000);
    check('each got ConnectLink once for each of the other three',
          players.every((p) => p.commands.filter((c) => c === C.ConnectLink).length === 3));

    const t0 = Date.now();
    await Promise.all([...players, ...pair].map((p) => p.sendData()));
    await until(() => players.every((p) => p.packets.length >= 3 * PACKETS) && pair.every((p) => p.packets.length >= PACKETS), 60000);
    const ms = Date.now() - t0;
    // As a receiver does: dedupe by (sender, sequence), order by sequence. A
    // slow line gets resends (the server's ack wait ran out first), so
    // duplicates and late copies are the protocol working, not a fault.
    for (const p of players) {
      const others = players.filter((q) => q !== p).map((q) => q.slot);
      let dups = 0, late = 0;
      const ok = others.every((s) => {
        const seqs = p.packets.filter((k) => k.from === s).map((k) => k.sequence);
        const uniq = new Set(seqs);
        dups += seqs.length - uniq.size;
        let high = -1;
        for (const v of seqs) { if (v < high) late++; else high = v; }
        return uniq.size === PACKETS && [...uniq].every((v) => v >= 0 && v < PACKETS);
      });
      const own = p.packets.some((k) => k.from === p.slot);
      const tagged = p.packets.every((k) => k.data[1] === k.from && k.data[0] === k.sequence);
      check(`slot ${p.slot} (${lines[p.slot]} ms): all ${PACKETS} from each of the other three, none of its own,` +
            ` each marked with its sender (${dups} duplicate copies, ${late} behind a later one)`, ok && !own && tagged);
    }
    check('the two-player session got its partner\'s packets, unchanged (no `from`)',
          pair.every((p) => p.packets.length === PACKETS && p.packets.every((k) => !('from' in k))));
    console.log(`    data: ${4 * PACKETS} packets to 3 receivers each, ${(ms / 1000).toFixed(1)} s on lines of ${lines.join('/')} ms one way`);
  } catch (e) {
    check(`no exception (${e.message})`, false);
  } finally {
    for (const p of [...players, ...pair, intruder].filter(Boolean)) p.close();
    server.kill();
    await sleep(500);
  }
  if (fails) console.log(log.join('').split('\n').slice(-30).join('\n'));
}

console.log('celio 4p          four-player sessions, patched server');
await run(path.join(out, 'celio_server_patched.mjs'), [2, 40, 150, 600]);
console.log(`celio 4p          ${fails ? `${fails} check(s) FAILED` : 'all checks passed'}`);
process.exit(fails ? 1 : 0);
