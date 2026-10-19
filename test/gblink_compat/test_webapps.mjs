// test_webapps.mjs -- the GB-Link browser apps' own USB drivers, unmodified,
// against the gbagate firmware through a WebUSB stand-in (fake_webusb.mjs)
// built from the firmware's real descriptors.
//
//   gblink-ereader    src/link/gblink.js + ereader-mode.js: e-Reader mode
//                     entry, card preload and scan start; Gen 3 slave entry
//   gb-pokemon-web    src/services/UsbConnection.js: GB link mode and a
//                     Gen 2 trade-room handshake exchange
//   gblink-distributor src/link/gblink.js: GB link entry and byte exchange
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { installFakeWebUsb } from './fake_webusb.mjs';

const out = process.argv[2];
const SRC = process.env.GBLINK_SRC || path.join(out, 'src');
const mod = (repo, file) => import(pathToFileURL(path.join(SRC, repo, file)).href);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The apps log progress to the console; keep the verdicts readable.
const quiet = { log: console.log, warn: console.warn, error: console.error };
const hush = () => { console.log = () => {}; console.warn = () => {}; console.error = () => {}; };
const speak = () => Object.assign(console, quiet);

let fails = 0;
const results = [];
async function test(name, fn) {
  const t0 = Date.now();
  let sim;
  hush();
  try {
    await fn((s) => { sim = s; });
    speak();
    results.push(`  ok    ${name.padEnd(46)} ${((Date.now() - t0) / 1000).toFixed(2)}s`);
  } catch (e) {
    speak();
    fails++;
    results.push(`  FAIL  ${name}: ${e.stack || e}`);
  } finally {
    sim?.kill();
  }
}
const assert = (c, m) => { if (!c) throw new Error(m); };

await test('gblink-ereader: e-Reader mode, preload, scan', async (own) => {
  const { sim } = installFakeWebUsb();
  own(sim);
  sim.gba(0x06, 1);
  const g = await mod('gblink-ereader', 'src/link/gblink.js');
  const er = await mod('gblink-ereader', 'src/link/ereader-mode.js');
  const progress = [];
  await g.connect({ linkMode: 'ereader', ereaderProfile: er.EREADER_PROFILE.POKEMON_RUBY,
                    cableOverride: 2, onProgress: (m) => progress.push(m) });
  assert(g.isConnected(), 'connected');
  assert(progress.some((m) => /PIO/.test(m)), `reached link enable: ${progress.join(' | ')}`);
  // A Mystery Event payload (bytes 5-10, 13-16 = 0xFF, >= 512 bytes).
  const card = new Uint8Array(512);
  for (const i of [5, 6, 7, 8, 9, 10, 13, 14, 15, 16]) card[i] = 0xff;
  const events = [];
  sim.on('usb', (ep, body) => {
    if (ep === 0x82 && body.subarray(0, 4).toString('latin1') === 'ERDR') events.push(body[4]);
  });
  await er.preloadCardToAdapter(card);
  await er.startScan();
  await sleep(500);
  // The firmware answers START_SCAN with PHASE (handshaking), and never with
  // ERROR 2 (card not loaded): the preload landed.
  assert(events.includes(er.ERDR_EVT.PHASE), `phase event after scan start: ${events}`);
  assert(!events.includes(er.ERDR_EVT.ERROR), `no error: ${events}`);
  await g.disconnect();
});

await test('gblink-ereader: Gen 3 link slave entry', async (own) => {
  const { sim } = installFakeWebUsb();
  own(sim);
  const g = await import(pathToFileURL(path.join(SRC, 'gblink-ereader/src/link/gblink.js')).href + '?gen3');
  const progress = [];
  await g.connect({ linkMode: 'gen3', cableOverride: 2, onProgress: (m) => progress.push(m) });
  assert(progress.some((m) => /Link listener ready/.test(m)), `ready: ${progress.join(' | ')}`);
  await g.disconnect();
});

await test('gb-pokemon-web: GB link mode, trade-room bytes', async (own) => {
  const { sim } = installFakeWebUsb();
  own(sim);
  const { UsbConnection, MODE } = await mod('gb-pokemon-web', 'src/services/UsbConnection.js');
  const c = new UsbConnection();
  await c.connect();
  assert(c.isNewFirmware, 'recognised as GB-Link unified firmware (VID 0x2FE3)');
  assert(c.cmdEndpointOut === 1 && c.endpointOut === 2 && c.cmdEndpointIn === 1 && c.endpointIn === 2,
         `endpoints cmd ${c.cmdEndpointOut}/${c.cmdEndpointIn} data ${c.endpointOut}/${c.endpointIn}`);
  await c.setVoltage('5v');
  await c.setMode(MODE.GB_LINK ?? 0x02);
  await sleep(100);
  await c.drain();
  // The Game Boy answers the Gen 2 room-entry sequence 01 61 D1 00 FE.
  sim.gba(0x04, 0x02, 0x61, 0xd1, 0x00, 0xfe);
  const got = [];
  for (const b of [0x01, 0x61, 0xd1, 0x00, 0xfe]) {
    await c.writeByte(b);
    got.push(await c.readByte(1000));
  }
  assert(got.join(',') === [0x02, 0x61, 0xd1, 0x00, 0xfe].join(','), `GB replies ${got}`);
  await c.disconnect();
});

await test('gblink-distributor: GB link entry, sendByte/receiveByte', async (own) => {
  const { sim } = installFakeWebUsb();
  own(sim);
  const d = await mod('gblink-distributor', 'src/link/gblink.js');
  await d.connect();
  assert(d.isConnected(), 'connected');
  await sleep(100);
  sim.gba(0x04, 0x60);
  await d.sendByte(0x01);
  const r = await d.receiveByte();
  assert(r === 0x60, `GB reply ${r}`);
  await d.disconnect();
});

console.log(results.join('\n'));
console.log(`GB-Link web apps   ${fails ? `${fails} FAILED` : `all ${results.length} checks passed`}`);
process.exit(fails ? 1 : 0);
