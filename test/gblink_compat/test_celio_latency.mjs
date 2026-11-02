// test_celio_latency.mjs -- online play through a bad network: the board's
// own Celio client at each end, the real Celio-Server between, and latency,
// jitter and stalls added to the boards' WiFi only (celio_harness.mjs).
//
// Each case is a whole session -- create, join, link, frames both ways,
// leave -- and the table says how long each stage took, how many packets
// the board had to send again, and the slowest acknowledgement it saw. The
// cases up to MUST_PASS are the conditions the board promises to ride out;
// the ones past it find where the link breaks down, and only report.
//
//   node test_celio_latency.mjs <build> [one-way ms ...]   one case per delay
import path from 'node:path';
import { runPair } from './celio_harness.mjs';

const out = process.argv[2];
const server = path.join(out, 'celio_server.mjs');

// one-way delay, each direction, on each board's WiFi; jitter on top of it
const CASES = [
  { name: 'LAN',                 lat: { ms: 2, jitter: 3 } },
  { name: 'good internet',       lat: { ms: 40, jitter: 20 } },
  { name: 'far server',          lat: { ms: 150, jitter: 60 } },
  { name: 'mobile hotspot',      lat: { ms: 300, jitter: 200 } },
  { name: 'stalls (1 s in 6)',   lat: { ms: 60, jitter: 40, stallEvery: 6000, stallMs: 1000 } },
  { name: 'satellite',           lat: { ms: 600, jitter: 150 } },
  { name: 'very bad',            lat: { ms: 1000, jitter: 300 } },
  { name: 'breaking',            lat: { ms: 1500, jitter: 300 } },
];
const MUST_PASS = 5;   /* LAN through the stalls: a board that fails these is broken */

const args = process.argv.slice(3).map(Number).filter((n) => n >= 0);
const cases = args.length
  ? args.map((ms) => ({ name: `${ms} ms`, lat: { ms, jitter: ms / 4 } }))
  : CASES;

const fmt = (ms) => (ms === undefined ? '-' : `${(ms / 1000).toFixed(1)}s`);
console.log('celio latency      case                RTT    session  linked  frames   resent  lost  slowest ack');
let fail = 0;
for (const [i, c] of cases.entries()) {
  const r = await runPair(server, { lat: c.lat, seed: i + 1, budgetMs: 40000 });
  const rtt = `${Math.round(2 * c.lat.ms)}ms`;
  const resent = (r.a?.resent ?? 0) + (r.b?.resent ?? 0);
  const lost = (r.a?.givenUp ?? 0) + (r.b?.givenUp ?? 0) + (r.a?.lost ?? 0) + (r.b?.lost ?? 0);
  const slow = Math.max(r.a?.maxRttMs ?? 0, r.b?.maxRttMs ?? 0);
  const must = !args.length && i < MUST_PASS;
  console.log(`  ${r.ok ? 'ok  ' : must ? 'FAIL' : 'gone'}             ${c.name.padEnd(20)}${rtt.padStart(6)}  ${fmt(r.sessionMs).padStart(7)} ${fmt(r.linkMs).padStart(7)} ${fmt(r.ok ? r.framesMs : undefined).padStart(7)}  ${String(resent).padStart(6)}  ${String(lost).padStart(4)}  ${slow}ms` +
              (r.ok ? '' : `   (${r.stage}: ${r.why})`));
  if (!r.ok && must) {
    fail++;
    console.log(r.log.join('').split('\n').filter((l) => l !== '-> 3' && l !== '<- 2').slice(-15).join('\n'));
  }
}
console.log(`celio latency      ${fail ? `${fail} required case(s) FAILED` : 'every required case linked; the rest report where it breaks'}`);
process.exit(fail ? 1 : 0);
