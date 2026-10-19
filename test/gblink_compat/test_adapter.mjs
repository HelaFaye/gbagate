// test_adapter.mjs -- netplay-bridge's GBLinkAdapter (src/engine/usb/adapter.ts)
// driving the gbagate firmware: every call the bridge makes on a real session.
import { SimDevice, SimTransport } from './sim_transport.mjs';

const engine = await import(`${process.argv[2]}/engine.mjs`);
const { GBLinkAdapter, FwStatus, fwAtLeast } = engine;

let fails = 0;
const results = [];
async function test(name, fn) {
  const dev = new SimDevice();
  const t0 = Date.now();
  try {
    await fn(dev);
    results.push(`  ok    ${name.padEnd(44)} ${((Date.now() - t0) / 1000).toFixed(2)}s`);
  } catch (e) {
    fails++;
    results.push(`  FAIL  ${name}: ${e.stack || e}`);
  } finally {
    dev.kill();
  }
}
const assert = (c, m) => { if (!c) throw new Error(m); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = (emitter, ev, pred, ms = 5000) => new Promise((resolve, reject) => {
  const t = setTimeout(() => { emitter.off(ev, f); reject(new Error(`timeout waiting for ${ev}`)); }, ms);
  const f = (...a) => { if (pred(...a)) { clearTimeout(t); emitter.off(ev, f); resolve(a); } };
  emitter.on(ev, f);
});

await test('getFirmwareInfo / gen3 minimum', async (dev) => {
  const a = new GBLinkAdapter(new SimTransport(dev));
  await a.start();
  const fw = await a.getFirmwareInfo();
  assert(fw.major === 2 && fw.minor === 2 && fw.patch === 3, `version ${JSON.stringify(fw)}`);
  assert(fw.landingPage === true, 'landing page');
  assert(fw.cableSelection === 'gbc', `cable selection ${fw.cableSelection}`);
  // linkBridge.ts GEN3_MIN_FW: the bridge refuses older firmware for gen3.
  assert(fwAtLeast(fw, 2, 2, 3), 'meets the bridge gen3 minimum');
  await a.stop();
});

await test('setCableSelection + getCableType', async (dev) => {
  dev.gba(0x06, 0);
  const a = new GBLinkAdapter(new SimTransport(dev));
  await a.start();
  await a.setCableSelection('auto');
  const fw = await a.getFirmwareInfo();
  assert(fw.cableSelection === 'auto', `persisted ${fw.cableSelection}`);
  await a.initLinkMode('master', 'gen3');
  const t = await a.getCableType();
  assert(t === 0, `GBA cable detected, got ${t}`);
  await a.stop();
});

await test('initLinkMode master -> handshake -> rounds', async (dev) => {
  dev.gba(0x01, 1); // a child GBA in a Cable Club
  const a = new GBLinkAdapter(new SimTransport(dev));
  await a.start();
  const statuses = [];
  a.on('status', (s) => statuses.push(s));
  await a.initLinkMode('master', 'gen3');
  await until(a, 'status', (s) => s === FwStatus.HandshakeReceived);
  await a.startHandshake();
  await sleep(150); // linkBridge.ts CONNECT_GRACE_MS
  await a.connectLink();
  await until(a, 'status', (s) => s === FwStatus.LinkConnected);
  const [round] = await until(a, 'round', (r) => r[0] === 0xcafe || r[0] !== 0);
  assert(round.length === 8, 'rounds are 8 words');
  // Host -> GBA: four rounds of eight words.
  const buf = Buffer.alloc(64);
  for (let r = 0; r < 4; r++) buf.writeUInt16LE(0x2222, r * 16);
  buf.writeUInt16LE(0x1234, 2);
  await a.writeRounds(buf);
  await sleep(400);
  assert(dev.words.some(([tx]) => tx === 0x1234), 'round reached the GBA');
  assert(statuses.includes(FwStatus.DeviceReady) && statuses.includes(FwStatus.AwaitMode), 'ready/await seen');
  await a.cancel();
  await until(a, 'status', (s) => s === FwStatus.LinkClosed);
  await a.stop();
});

await test('initLinkMode slave (bridge hosting)', async (dev) => {
  dev.gba(0x06, 1);
  dev.gba(0x01, 2); // the GBA is parent
  dev.gba(0x03, 4);
  const a = new GBLinkAdapter(new SimTransport(dev));
  await a.start();
  await a.initLinkMode('slave', 'gen3');
  await until(a, 'status', (s) => s === FwStatus.HandshakeReceived);
  await a.startHandshake();
  await until(a, 'status', (s) => s === FwStatus.LinkConnected);
  await until(a, 'round', () => true);
  await a.stop();
});

await test('Advance Wars mode entry (aw2, timing override)', async (dev) => {
  dev.gba(0x01, 3);
  const a = new GBLinkAdapter(new SimTransport(dev));
  await a.start();
  await a.initLinkMode('master', 'aw2', { pxgUs: 0, syncUs: 0 });
  await until(a, 'status', (s) => s === FwStatus.HandshakeReceived);
  await a.startHandshake();
  await until(a, 'status', (s) => s === FwStatus.LinkConnected);
  await a.stop();
});

await test('probeWireless reports no RFU (2.2.3 has none)', async (dev) => {
  const a = new GBLinkAdapter(new SimTransport(dev));
  await a.start();
  const rfu = await a.probeWireless();
  assert(rfu === false, 'no wireless mode claimed');
  await a.stop();
});

console.log(results.join('\n'));
console.log(`netplay-bridge adapter ${fails ? `${fails} FAILED` : `all ${results.length} checks passed`}`);
process.exit(fails ? 1 : 0);
