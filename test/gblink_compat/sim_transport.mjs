// SimTransport -- netplay-bridge's ILinkTransport (src/engine/usb/device.ts)
// backed by build/gblink_sim instead of libusb.
//
// The engine sees exactly what it would see from a GB-Link on USB: commands go
// out on EP 0x01, 64-byte data on EP 0x02, status words arrive from EP 0x81 and
// data from EP 0x82. The simulator is gbagate's firmware build of GB-Link's own
// adapter firmware, so this is their host code against our device.
import { EventEmitter } from 'node:events';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const SIM = path.resolve(HERE, '../../build/gblink_sim/gblink_sim');

export class SimDevice extends EventEmitter {
  constructor({ flash } = {}) {
    super();
    this.proc = spawn(SIM, flash ? [flash] : [], { stdio: ['pipe', 'pipe', 'inherit'] });
    this.buf = Buffer.alloc(0);
    this.words = [];
    this.proc.stdout.on('data', (d) => this.onData(d));
    this.ready = new Promise((resolve) => this.once('ready', resolve));
  }

  onData(d) {
    this.buf = Buffer.concat([this.buf, d]);
    while (this.buf.length >= 4) {
      const n = this.buf.readUInt16LE(2);
      if (this.buf.length < 4 + n) return;
      const kind = String.fromCharCode(this.buf[0]);
      const ep = this.buf[1];
      const body = Buffer.from(this.buf.subarray(4, 4 + n));
      this.buf = this.buf.subarray(4 + n);
      this.emit('record', kind, ep, body);
      if (kind === 'U') this.emit('usb', ep, body);
      else if (kind === 'E') {
        if (ep === 9) this.emit('ready');
        if (ep === 7) this.words.push([body.readUInt16LE(0), body.readUInt16LE(2)]);
        this.emit('event', ep, body);
      }
    }
  }

  send(kind, ep, body = Buffer.alloc(0)) {
    const hdr = Buffer.from([kind.charCodeAt(0), ep, body.length & 0xff, body.length >> 8]);
    this.proc.stdin.write(Buffer.concat([hdr, body]));
  }

  gba(...bytes) { this.send('G', 0, Buffer.from(bytes)); }

  kill() {
    try { this.send('Q', 0); } catch { /* already gone */ }
    setTimeout(() => this.proc.kill(), 200).unref();
  }
}

export class SimTransport extends EventEmitter {
  constructor(device) {
    super();
    this.dev = device;
    this.open_ = false;
    this.dev.on('usb', (ep, body) => {
      if (!this.open_) return;
      if (ep === 0x81) this.emit('status-bytes', body);
      else if (ep === 0x82) this.emit('data-bytes', body);
    });
  }
  async open() { await this.dev.ready; this.open_ = true; }
  async sendCommand(bytes) { this.dev.send('U', 0x01, Buffer.from(bytes)); }
  async sendData(buf) {
    if (buf.length !== 64) throw new Error(`data OUT endpoint takes exactly 64 bytes, got ${buf.length}`);
    this.dev.send('U', 0x02, Buffer.from(buf));
  }
  async close() { this.open_ = false; }
  present() { return true; }
}
