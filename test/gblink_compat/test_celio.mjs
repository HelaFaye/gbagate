// test_celio.mjs -- an online Pokemon link through Celio, GB-Link's relay:
// Celio-Server (unmodified) in the middle, Celio-Client's own link-session
// code (LinkExchangeSession, CommandEmitterSocketIO, WebSocketService,
// LinkDeviceUtils -- unmodified) at each end, and a gbagate adapter with a GBA
// behind each client.
//
// Celio assigns roles itself: the first adapter to report AwaitMode is told
// to be master, the second slave, and it relays ConnectLink between them. So
// the first adapter gets a child GBA and the second a parent GBA, which is
// what two players' Cable Club visits look like to it.
import { spawn } from 'node:child_process';
import path from 'node:path';
import { SimDevice } from './sim_transport.mjs';

const out = process.argv[2];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const C = await import(`${out}/celio_client.mjs`);
const { LinkExchangeSession, CommandEmitterSocketIO, StatusEmitterAbstract, LinkDeviceUtils,
        WebSocketService, LinkMode } = C;

/** Celio-Client's StatusEmitterLinkDevice, with the WebUSB service swapped for the simulator. */
class StatusEmitterSim extends StatusEmitterAbstract {
  constructor(dev) {
    super();
    this.dev = dev;
    this.onUsb = (ep, body) => {
      if (ep === 0x81) for (let i = 0; i + 1 < body.length; i += 2) this.statusSubject.next(body.readUInt16LE(i));
      else if (ep === 0x82 && body.length === 64) {
        const words = [];
        for (let i = 0; i < 32; i++) words.push(body.readUInt16LE(i * 2));
        this.dataSubject.next(words);
      }
    };
    dev.on('usb', this.onUsb);
  }
  async receiveCommand(command, args = new Uint8Array(0)) {
    this.dev.send('U', 0x01, Buffer.from([command, ...args]));
    return true;
  }
  async receiveData(data) {
    const b = Buffer.alloc(64);
    data.forEach((w, i) => b.writeUInt16LE(w & 0xffff, i * 2));
    this.dev.send('U', 0x02, b);
    return true;
  }
  destroy() { this.dev.off('usb', this.onUsb); }
}

const server = spawn(process.execPath, [path.join(out, 'celio_server.mjs')], { stdio: ['ignore', 'pipe', 'pipe'] });
const serverLog = [];
server.stdout.on('data', (d) => serverLog.push(d.toString()));
server.stderr.on('data', (d) => serverLog.push(d.toString()));
await new Promise((resolve, reject) => {
  const t = setTimeout(() => reject(new Error('Celio-Server did not start')), 8000);
  server.stdout.on('data', (d) => { if (d.toString().includes('listening')) { clearTimeout(t); resolve(); } });
});

const devA = new SimDevice();
const devB = new SimDevice();
devA.gba(0x06, 1); devA.gba(0x01, 1);                     // child GBA: A will be master
devB.gba(0x06, 1); devB.gba(0x01, 2); devB.gba(0x03, 6);  // parent GBA: B will be slave

let ok = false;
const sessions = [];
// Celio-Client logs every packet it relays; keep this test's output to its verdict.
const quiet = { log: console.log, warn: console.warn };
console.log = () => {}; console.warn = () => {};
try {
  const wsA = new WebSocketService();
  const wsB = new WebSocketService();
  if (!(await wsA.connect()) || !(await wsB.connect())) throw new Error('socket.io connect failed');

  // As Celio-Client's PlayerSessionService.enterSession(): emit with an ack.
  const enter = (ws, event, arg) => new Promise((resolve) => ws.emit(event, arg, resolve));
  const created = await enter(wsA, 'sessionCreate', undefined);
  if (created?.variant !== 'Ok') throw new Error(`sessionCreate: ${JSON.stringify(created)}`);
  const joined = await enter(wsB, 'sessionJoin', created.value.id);
  if (joined?.variant !== 'Ok') throw new Error(`sessionJoin: ${JSON.stringify(joined)}`);

  const seA = new StatusEmitterSim(devA);
  const seB = new StatusEmitterSim(devB);
  sessions.push(new LinkExchangeSession(new CommandEmitterSocketIO(wsA), seA));
  sessions.push(new LinkExchangeSession(new CommandEmitterSocketIO(wsB), seB));

  await LinkDeviceUtils.tryEnableLinkMode(seA, LinkMode.onlineLink);
  await sleep(300);
  await LinkDeviceUtils.tryEnableLinkMode(seB, LinkMode.onlineLink);

  const frame = (tag) => [0xbb, 0xbb, tag & 0xff, tag >> 8, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  const deadline = Date.now() + 25000;
  let sent = false;
  while (Date.now() < deadline) {
    if (!sent && devA.words.length > 50 && devB.words.length > 50) {
      for (let i = 0; i < 6; i++) { devA.gba(0x02, ...frame(0xce01)); devB.gba(0x02, ...frame(0xce02)); }
      sent = true;
    }
    if (devA.words.some(([tx]) => tx === 0xce02) && devB.words.some(([tx]) => tx === 0xce01)) { ok = true; break; }
    await sleep(200);
  }
  for (const s of sessions) s.destroy();
  wsA.disconnect(); wsB.disconnect();
} catch (e) {
  serverLog.push(`exception ${e.stack || e}`);
} finally {
  console.log = quiet.log; console.warn = quiet.warn;
  devA.kill(); devB.kill();
  server.kill();
}

if (!ok) {
  console.log(serverLog.join('').split('\n').slice(-30).join('\n'));
  console.log(`A saw 0xce02: ${devA.words.some(([tx]) => tx === 0xce02)}, B saw 0xce01: ${devB.words.some(([tx]) => tx === 0xce01)}`);
}
console.log(`celio relay        ${ok ? 'GBA <-> GBA through Celio-Server and Celio-Client: frames delivered both ways' : 'FAILED'}`);
process.exit(ok ? 0 : 1);
