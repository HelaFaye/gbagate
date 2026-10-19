// test_netplay_e2e.mjs -- two GBAs, two gbagate adapters, one RetroArch
// netplay session, every byte of it run by GB-Link's netplay bridge engine
// (GBLinkEngine, unmodified): the host side's GBA is link parent, the joining
// side's GBA is child, exactly as on two players' desks.
//
//   GBA(parent) <-> gbagate sim <-> GBLinkEngine host --TCP netplay--
//       GBLinkEngine join <-> gbagate sim <-> GBA(child)
//
// Pass: each GBA's block-transfer frame arrives at the other GBA's wire.
import { SimDevice, SimTransport } from './sim_transport.mjs';

const engine = await import(`${process.argv[2]}/engine.mjs`);
const { GBLinkEngine, GBLinkAdapter } = engine;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const port = 56000 + Math.floor(Math.random() * 2000);

const devHost = new SimDevice();
const devJoin = new SimDevice();
devHost.gba(0x06, 1); devHost.gba(0x01, 2); devHost.gba(0x03, 6); // parent GBA
devJoin.gba(0x06, 1); devJoin.gba(0x01, 1);                       // child GBA

const log = [];
const host = new GBLinkEngine(() => new GBLinkAdapter(new SimTransport(devHost)));
const join = new GBLinkEngine(() => new GBLinkAdapter(new SimTransport(devJoin)));
for (const [name, e] of [['host', host], ['join', join]]) {
  e.on('link-phase', (p) => log.push(`${name} phase ${p}`));
  e.on('session', (s) => log.push(`${name} session ${s.phase}`));
  e.on('warn', (m) => log.push(`${name} warn ${m}`));
  e.on('error', (err) => log.push(`${name} error ${err.message}`));
}

let ok = false;
try {
  await host.start({ mode: 'host', port, nick: 'Brock', game: 'gen3' });
  await sleep(300);
  await join.start({ mode: 'join', host: '127.0.0.1', port, nick: 'Ash', game: 'gen3' });

  // Wait for both links, then have each GBA start a block transfer.
  const deadline = Date.now() + 25000;
  const phases = () => log.filter((l) => l.includes('phase'));
  while (Date.now() < deadline && !(phases().some((l) => l.startsWith('host') && l.endsWith('connected'))
                                    && phases().some((l) => l.startsWith('join') && l.endsWith('connected')))) {
    await sleep(100);
  }
  const frame = (tag) => [0xbb, 0xbb, tag & 0xff, tag >> 8, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  for (let i = 0; i < 6; i++) { devHost.gba(0x02, ...frame(0x4e01)); devJoin.gba(0x02, ...frame(0x4e02)); }

  while (Date.now() < deadline) {
    const hostGot = devHost.words.some(([tx]) => tx === 0x4e02);
    const joinGot = devJoin.words.some(([tx]) => tx === 0x4e01);
    if (hostGot && joinGot) { ok = true; break; }
    await sleep(200);
  }
} catch (e) {
  log.push(`exception ${e.stack || e}`);
} finally {
  await join.stop().catch(() => {});
  await host.stop().catch(() => {});
  devHost.kill(); devJoin.kill();
}

if (!ok) {
  console.log(log.slice(-40).join('\n'));
  console.log(`host GBA saw 0x4e02: ${devHost.words.some(([tx]) => tx === 0x4e02)}, join GBA saw 0x4e01: ${devJoin.words.some(([tx]) => tx === 0x4e01)}`);
}
console.log(`netplay end-to-end  ${ok ? 'GBA <-> GBA over RetroArch netplay: frames delivered both ways' : 'FAILED'}`);
process.exit(ok ? 0 : 1);
