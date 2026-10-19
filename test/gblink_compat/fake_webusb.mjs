// fake_webusb.mjs -- a WebUSB navigator.usb for Node, whose one device is the
// gbagate firmware: its descriptors are the bytes the firmware serves
// (build/gblink_sim/usbdesc_dump, from src/gblink_usbdesc.c) and its
// endpoints are the simulator's.
//
// The GB-Link browser apps find their endpoints by walking
// device.configuration, so building that object from the real configuration
// descriptor -- not from a hand-written copy -- is what makes this a test of
// the descriptors as well as of the firmware.
//
// WebUSB behaviours kept because the apps depend on them:
//   - transfers fail unless the interface is claimed;
//   - transferIn(ep, n) resolves with at most one packet, and a packet longer
//     than n is a 'babble' (gb-pokemon-web notes asking for less than 64 fails);
//   - a closed device rejects every pending and future transfer.
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { SimDevice } from './sim_transport.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DUMP = path.resolve(HERE, '../../build/gblink_sim/usbdesc_dump');

export function firmwareDescriptors() {
  const j = JSON.parse(execFileSync(DUMP, { encoding: 'utf8' }));
  return Object.fromEntries(Object.entries(j).map(([k, v]) => [k, Buffer.from(v, 'hex')]));
}

/** Parse a configuration descriptor into WebUSB's USBConfiguration shape. */
export function parseConfiguration(cfg) {
  const interfaces = new Map();
  let cur = null;
  for (let off = 0; off < cfg.length; off += cfg[off]) {
    const len = cfg[off], type = cfg[off + 1];
    if (len === 0) throw new Error('zero-length descriptor');
    if (type === 0x04) {
      const num = cfg[off + 2];
      cur = { alternateSetting: cfg[off + 3], interfaceClass: cfg[off + 5],
              interfaceSubclass: cfg[off + 6], interfaceProtocol: cfg[off + 7], endpoints: [] };
      if (!interfaces.has(num)) interfaces.set(num, { interfaceNumber: num, claimed: false, alternates: [] });
      const itf = interfaces.get(num);
      itf.alternates.push(cur);
      itf.alternate = itf.alternates[0];
    } else if (type === 0x05 && cur) {
      const addr = cfg[off + 2], attr = cfg[off + 3];
      cur.endpoints.push({
        endpointNumber: addr & 0x0f,
        direction: addr & 0x80 ? 'in' : 'out',
        type: ['control', 'isochronous', 'bulk', 'interrupt'][attr & 3],
        packetSize: cfg.readUInt16LE(off + 4),
      });
    }
  }
  return { configurationValue: cfg[5], configurationName: null, interfaces: [...interfaces.values()] };
}

class FakeUSBDevice {
  constructor(sim, desc) {
    this.sim = sim;
    const d = desc.device;
    this.usbVersionMajor = d[3]; this.usbVersionMinor = d[2] >> 4; this.usbVersionSubminor = d[2] & 15;
    this.deviceClass = d[4]; this.deviceSubclass = d[5]; this.deviceProtocol = d[6];
    this.vendorId = d.readUInt16LE(8);
    this.productId = d.readUInt16LE(10);
    const bcd = d.readUInt16LE(12);
    this.deviceVersionMajor = bcd >> 8; this.deviceVersionMinor = (bcd >> 4) & 15; this.deviceVersionSubminor = bcd & 15;
    this.manufacturerName = 'gbagate';
    this.productName = 'GBLink USB';
    this.serialNumber = 'SIM000000001';
    this.config = parseConfiguration(desc.config);
    this.configurations = [this.config];
    this.configuration = null;
    this.opened = false;
    this.url = desc.url;
    this.queues = new Map();   // IN endpoint number -> packets
    this.waiters = new Map();  // IN endpoint number -> resolvers
    sim.on('usb', (ep, body) => {
      const n = ep & 0x0f;
      const w = this.waiters.get(n);
      if (w && w.length) w.shift()(body);
      else { if (!this.queues.has(n)) this.queues.set(n, []); this.queues.get(n).push(body); }
    });
  }
  claimedFor(ep, dir) {
    const itf = this.config.interfaces.find((i) => i.alternate.endpoints.some((e) => e.endpointNumber === ep && e.direction === dir));
    if (!itf) throw new DOMException(`endpoint ${ep} ${dir} not found`, 'NotFoundError');
    if (!itf.claimed) throw new DOMException('interface not claimed', 'InvalidStateError');
  }
  async open() { this.opened = true; this.configuration = this.config; }
  async close() {
    this.opened = false;
    for (const i of this.config.interfaces) i.claimed = false;
    for (const [, ws] of this.waiters) while (ws.length) ws.shift()(null);
  }
  async reset() { if (!this.opened) throw new DOMException('not open', 'InvalidStateError'); }
  async selectConfiguration(v) { if (v !== this.config.configurationValue) throw new DOMException('bad configuration', 'NotFoundError'); this.configuration = this.config; }
  async claimInterface(n) { const i = this.config.interfaces.find((x) => x.interfaceNumber === n); if (!i) throw new DOMException('no interface', 'NotFoundError'); i.claimed = true; }
  async releaseInterface(n) { const i = this.config.interfaces.find((x) => x.interfaceNumber === n); if (i) i.claimed = false; }
  async selectAlternateInterface(n, alt) { if (alt !== 0) throw new DOMException('no alternate', 'NotFoundError'); }
  async transferOut(ep, data) {
    if (!this.opened) throw new DOMException('device closed', 'InvalidStateError');
    this.claimedFor(ep, 'out');
    const buf = Buffer.from(data.buffer ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : data);
    if (ep === 1 || ep === 2) this.sim.send('U', ep, buf);
    return { status: 'ok', bytesWritten: buf.length };
  }
  async transferIn(ep, length) {
    if (!this.opened) throw new DOMException('device closed', 'InvalidStateError');
    this.claimedFor(ep, 'in');
    const q = this.queues.get(ep);
    const body = q && q.length ? q.shift()
      : await new Promise((resolve) => { if (!this.waiters.has(ep)) this.waiters.set(ep, []); this.waiters.get(ep).push(resolve); });
    if (body === null) throw new DOMException('transfer cancelled', 'AbortError');
    if (body.length > length) return { status: 'babble', data: new DataView(new ArrayBuffer(0)) };
    const ab = body.buffer.slice(body.byteOffset, body.byteOffset + body.length);
    return { status: 'ok', data: new DataView(ab) };
  }
  async controlTransferOut(setup) {
    // CDC SET_CONTROL_LINE_STATE (0x22), which the TinyUSB-firmware path sends.
    return { status: 'ok', bytesWritten: 0 };
  }
  async controlTransferIn(setup, length) {
    if (setup.requestType === 'vendor' && setup.request === 0x01 && setup.index === 0x02) {
      return { status: 'ok', data: new DataView(this.url.buffer.slice(this.url.byteOffset, this.url.byteOffset + this.url.length)) };
    }
    return { status: 'stall', data: new DataView(new ArrayBuffer(0)) };
  }
}

/** Install navigator.usb with one gbagate device behind it. Returns {sim, device}. */
export function installFakeWebUsb(opts = {}) {
  const sim = new SimDevice(opts);
  const device = new FakeUSBDevice(sim, firmwareDescriptors());
  const matches = (f) => (f.vendorId === undefined || f.vendorId === device.vendorId)
                      && (f.productId === undefined || f.productId === device.productId);
  globalThis.navigator ??= {};
  globalThis.navigator.usb = {
    granted: false,
    async requestDevice({ filters = [] } = {}) {
      if (!filters.some(matches)) throw new DOMException('No device selected.', 'NotFoundError');
      this.granted = true;
      return device;
    },
    async getDevices() { return this.granted ? [device] : []; },
    addEventListener() {}, removeEventListener() {},
  };
  return { sim, device };
}
