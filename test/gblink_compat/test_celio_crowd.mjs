// test_celio_crowd.mjs -- many players on one Celio server at once.
//
// A Celio session is two players, one link cable; a server holds many
// sessions. Each case puts several pairs of boards (the board's own Celio
// client in the GB-Link simulator) on ONE server at the same time, each pair
// on its own line, plus a fifth device that tries to join a full session.
// Run against the pinned upstream server and against it with
// backport/Celio-Link-Server applied (celio_server_patched.mjs).
//
// Checked, for every session that must link:
//   - it links and frames cross both ways;
//   - no frame from another session ever arrives (tokens are per pair);
//   - the intruder is refused ("Session is full") and the session plays on;
//   - on the patched server, a fast pair beside a slow one is not slowed:
//     the ack timeout is per client, so one slow line cannot stretch another's.
//
//   node test_celio_crowd.mjs <build>
import path from 'node:path';
import fs from 'node:fs';
import { runCrowd } from './celio_harness.mjs';

const out = process.argv[2];
const SERVERS = [
  { name: 'upstream', file: 'celio_server.mjs', slowLinks: false },
  { name: 'patched', file: 'celio_server_patched.mjs', slowLinks: true },
];
const LAN = { ms: 2, jitter: 3 };
const CASES = [
  { name: '4 players: LAN + LAN', pairs: [LAN, LAN] },
  { name: '4 players: LAN + 1.2 s', pairs: [LAN, { ms: 600, jitter: 150 }] },
  { name: '8 players, mixed lines', pairs: [LAN, { ms: 40, jitter: 20 }, { ms: 300, jitter: 200 }, { ms: 1000, jitter: 300 }] },
];
const FAST_MS = 6000;   // a LAN pair links and crosses frames well inside this alone

const fmt = (ms) => (ms === undefined ? '-' : `${(ms / 1000).toFixed(1)}s`);
let fail = 0;
for (const srv of SERVERS) {
  const file = path.join(out, srv.file);
  if (!fs.existsSync(file)) { console.log(`celio crowd       FAIL ${srv.file} not built`); fail++; continue; }
  for (const [ci, c] of CASES.entries()) {
    const r = await runCrowd(file, { pairs: c.pairs, seed: 7 + ci });
    console.log(`celio crowd       ${srv.name.padEnd(9)} ${c.name}`);
    const failBefore = fail;
    for (const [i, p] of r.pairs.entries()) {
      const rtt = 2 * (c.pairs[i].ms || 0);
      // the upstream server cannot hold a line slower than ~1 s round trip
      const must = srv.slowLinks || rtt <= 600;
      let bad = !p.ok && must;
      if (p.ok && i === 0 && srv.slowLinks && p.framesMs > FAST_MS) {
        bad = true;
        p.why = `the LAN pair took ${fmt(p.framesMs)} beside the others`;
      }
      if (/another session/.test(p.why)) bad = true;          // never acceptable
      if (bad) fail++;
      console.log(`    ${bad ? 'FAIL' : p.ok ? 'ok  ' : 'gone'}  pair ${i + 1}  RTT ${String(rtt).padStart(4)}ms  linked ${fmt(p.linkMs).padStart(5)}  frames ${fmt(p.ok ? p.framesMs : undefined).padStart(5)}  resent ${(p.a?.resent ?? 0) + (p.b?.resent ?? 0)}` +
                  (p.ok ? '' : `   (${p.stage}: ${p.why})`));
    }
    const intr = r.intruder;
    const refused = intr && intr.state === 'failed' && /full/i.test(intr.error);
    if (!refused) fail++;
    console.log(`    ${refused ? 'ok  ' : 'FAIL'}  a fifth device joining pair 1's session: ${intr ? `${intr.state} "${intr.error}"` : 'never answered'}`);
    if (fail > failBefore) {                                  // the evidence, for any required failure
      console.log(r.log.join('').split('\n').filter((l) => l !== '-> 3' && l !== '<- 2').slice(-20).join('\n'));
    }
  }
}
console.log(`celio crowd       ${fail ? `${fail} check(s) FAILED` : 'every session linked alone, the intruder was refused, nothing crossed'}`);
process.exit(fail ? 1 : 0);
