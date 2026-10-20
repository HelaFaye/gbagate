// test_launcher.mjs -- GB-Link's launcher page (launcher.gblink.io), in
// Chromium, against the gbagate firmware.
//
// The page runs unmodified from a local HTTP server. Its navigator.usb is a
// stand-in whose one device is the simulator: the descriptors are the bytes the
// firmware serves (src/gblink_usbdesc.c, via usbdesc_dump), the strings are the
// ones usb_gblink_esp32s3.c serves, and every transfer goes to build/gblink_sim
// through a Playwright binding. So the launcher's own detection code decides
// what it is looking at.
//
// Two launchers: upstream as it is, and with backport/gblink-launcher applied.
// Upstream takes gbagate for a GBLink 2.2.7 and offers it the RP2040 update
// (which cannot work on an ESP32-S3); patched, it names the board, offers no
// update, and labels all seven LED slots.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { SimDevice } from './sim_transport.mjs';
import { firmwareDescriptors, parseConfiguration } from './fake_webusb.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const BUILD = process.argv[2];
const SRC = process.env.GBLINK_SRC || path.join(BUILD, 'src');
const LAUNCHER = path.join(SRC, 'gblink-launcher');
const CHROMIUM = process.env.CHROMIUM || '/opt/pw-browsers/chromium';

// GBLINK_NO_SKIP (set in CI): a test that cannot run is a failure.
const skip = (why) => {
  const required = !!process.env.GBLINK_NO_SKIP;
  console.log(`launcher            ${required ? 'FAILED: required, but' : 'skipped:'} ${why}`);
  process.exit(required ? 1 : 0);
};
let chromium;
try {
  ({ chromium } = await import(`${BUILD}/node_modules/playwright-core/index.mjs`));
} catch {
  skip('playwright-core not installed');
}
if (!fs.existsSync(CHROMIUM)) skip(`no Chromium at ${CHROMIUM} (set CHROMIUM)`);
if (!fs.existsSync(path.join(LAUNCHER, 'index.html'))) skip('no gblink-launcher checkout');

/** A worktree of the launcher at upstream's head, optionally with our patches. */
function tree(name, patched) {
  const dir = path.join(BUILD, `launcher-${name}`);
  fs.rmSync(dir, { recursive: true, force: true });
  execFileSync('git', ['-C', LAUNCHER, 'worktree', 'prune']);
  // run.sh's pin (backport/README.md's base), or upstream's head when it says.
  const base = process.env.GBLINK_LAUNCHER_BASE || 'origin/HEAD';
  const head = execFileSync('git', ['-C', LAUNCHER, 'rev-parse', base]).toString().trim();
  execFileSync('git', ['-C', LAUNCHER, 'worktree', 'add', '-q', '--detach', dir, head]);
  if (patched) {
    const pdir = path.join(ROOT, 'backport/gblink-launcher');
    const patches = fs.readdirSync(pdir).filter((f) => f.endsWith('.patch')).sort().map((f) => path.join(pdir, f));
    execFileSync('git', ['-C', dir, '-c', 'user.name=t', '-c', 'user.email=t@t', 'am', '-q', ...patches]);
  }
  return dir;
}

function serve(dir) {
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.svg': 'image/svg+xml' };
  const server = http.createServer((req, res) => {
    const p = path.join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
    const file = p.endsWith('/') ? path.join(p, 'index.html') : p;
    if (!file.startsWith(dir) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'content-type': types[path.extname(file)] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

/** The page's navigator.usb: transfers go to window.__sim (a binding). */
function fakeUsb({ device, config, url }) {
  const ifaces = config.interfaces.map((i) => ({
    interfaceNumber: i.interfaceNumber, claimed: false,
    alternate: i.alternate, alternates: i.alternates,
  }));
  const configuration = { configurationValue: config.configurationValue, configurationName: null, interfaces: ifaces };
  const dv = (bytes) => new DataView(new Uint8Array(bytes).buffer);
  const dev = {
    ...device, opened: false, configuration: null, configurations: [configuration],
    async open() { this.opened = true; this.configuration = configuration; },
    async close() { this.opened = false; },
    async reset() {},
    async forget() {},
    async selectConfiguration() { this.configuration = configuration; },
    async claimInterface(n) { ifaces.find((i) => i.interfaceNumber === n).claimed = true; },
    async releaseInterface(n) { const i = ifaces.find((x) => x.interfaceNumber === n); if (i) i.claimed = false; },
    async selectAlternateInterface() {},
    async clearHalt() {},
    async transferOut(ep, data) {
      const bytes = Array.from(new Uint8Array(data.buffer ? data.buffer.slice(data.byteOffset ?? 0, (data.byteOffset ?? 0) + data.byteLength) : data));
      await window.__sim('out', ep, bytes);
      return { status: 'ok', bytesWritten: bytes.length };
    },
    async transferIn(ep, length) {
      const bytes = await window.__sim('in', ep, length);
      if (bytes === null) throw new DOMException('transfer cancelled', 'AbortError');
      if (bytes.length > length) return { status: 'babble', data: dv([]) };
      return { status: 'ok', data: dv(bytes) };
    },
    async controlTransferOut() { return { status: 'ok', bytesWritten: 0 }; },
    async controlTransferIn(setup) {
      if (setup.requestType === 'vendor' && setup.request === 0x01 && setup.index === 0x02) return { status: 'ok', data: dv(url) };
      return { status: 'stall', data: dv([]) };
    },
  };
  const listeners = {};
  Object.defineProperty(navigator, 'usb', { configurable: true, value: {
    async getDevices() { return [dev]; },
    async requestDevice() { return dev; },
    addEventListener(t, f) { (listeners[t] ||= []).push(f); },
    removeEventListener() {},
  } });
}

async function run(name, dir) {
  const sim = new SimDevice();
  await sim.ready;
  const queues = new Map();
  const waiters = new Map();
  sim.on('usb', (ep, body) => {
    const n = ep & 0x0f;
    const w = waiters.get(n);
    if (w && w.length) w.shift()(Array.from(body));
    else { if (!queues.has(n)) queues.set(n, []); queues.get(n).push(Array.from(body)); }
  });

  const desc = firmwareDescriptors();
  const d = desc.device;
  const bcd = d.readUInt16LE(12);
  const device = {
    vendorId: d.readUInt16LE(8), productId: d.readUInt16LE(10),
    deviceClass: d[4], deviceSubclass: d[5], deviceProtocol: d[6],
    usbVersionMajor: d[3], usbVersionMinor: d[2] >> 4, usbVersionSubminor: d[2] & 15,
    deviceVersionMajor: bcd >> 8, deviceVersionMinor: (bcd >> 4) & 15, deviceVersionSubminor: bcd & 15,
    // usb_gblink_esp32s3.c s_strings
    manufacturerName: 'gbagate', productName: 'GBLink USB', serialNumber: 'SIM000000001',
  };

  const server = await serve(dir);
  const browser = await chromium.launch({ executablePath: CHROMIUM });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.exposeBinding('__sim', async (_src, op, ep, arg) => {
    if (op === 'out') { if (ep === 1 || ep === 2) sim.send('U', ep, Buffer.from(arg)); return null; }
    const q = queues.get(ep);
    if (q && q.length) return q.shift();
    return new Promise((resolve) => {
      if (!waiters.has(ep)) waiters.set(ep, []);
      const list = waiters.get(ep);
      const timer = setTimeout(() => { const i = list.indexOf(resolve); if (i >= 0) list.splice(i, 1); resolve(null); }, 2000);
      list.push((b) => { clearTimeout(timer); resolve(b); });
    });
  });
  await page.addInitScript(fakeUsb, {
    device, config: parseConfiguration(desc.config), url: Array.from(desc.url),
  });
  // A real Chrome only exposes navigator.usb in a secure context; localhost is one.
  await page.goto(`http://localhost:${server.address().port}/`);
  if (await page.locator('#device-connected').isHidden()) await page.click('#device-connect').catch(() => {});
  await page.waitForFunction(() => {
    const f = document.querySelector('#device-firmware');
    return f && f.textContent && f.textContent !== 'Unknown' && !document.querySelector('#device-connected').hidden;
  }, null, { timeout: 15000 });
  await page.waitForTimeout(1500);   // the LED query and the updater settle
  const firmware = (await page.textContent('#device-firmware')).trim();
  const updater = await page.locator('#device-update').isVisible();
  const leds = await page.$$eval('#device-led-rows .device-led-label', (els) => els.map((e) => e.textContent));

  await browser.close();
  server.close();
  sim.kill();
  return { name, firmware, updater, leds, errors };
}

const results = [];
let fails = 0;
const check = (cond, what) => { results.push(`  ${cond ? 'ok  ' : 'FAIL'}  ${what}`); if (!cond) fails++; };

const up = await run('upstream', tree('upstream', false));
check(/2\.2\.7/.test(up.firmware), `upstream: identified as GBLink 2.2.7 ("${up.firmware}")`);
check(up.updater, 'upstream: offers the RP2040 firmware update (what backport/gblink-launcher 0002 stops)');
check(up.leds.length === 7 && up.leds[6] === 'Mode 6', `upstream: seven LED slots, the last unlabelled (${up.leds[6]})`);

const pt = await run('patched', tree('patched', true));
check(/^gbagate \(GBLink v2\.2\.7 protocol\)$/.test(pt.firmware), `patched: named as gbagate ("${pt.firmware}")`);
check(!pt.updater, 'patched: no firmware updater');
check(pt.leds.length === 7 && pt.leds[6] === 'Battle Chip Gate', `patched: LED slot 6 labelled (${pt.leds[6]})`);
check(!up.errors.length && !pt.errors.length, `no page errors ${JSON.stringify([...up.errors, ...pt.errors])}`);

console.log(results.join('\n'));
console.log(`launcher            ${fails ? `${fails} FAILED` : `all ${results.length} checks passed`}`);
process.exit(fails ? 1 : 0);
