// build.mjs -- bundle GB-Link's host software, unmodified, for the tests here.
//
//   node build.mjs <out-dir> <netplay-bridge> <celio-client> <celio-server>
//
// Only packages that need a browser, Angular or libusb are swapped for stubs
// (stubs/); every GB-Link source file is compiled as checked out.
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const [out, bridge, client, server] = process.argv.slice(2);
const HERE = path.dirname(fileURLToPath(import.meta.url));
const { build } = await import(path.join(out, 'node_modules/esbuild/lib/main.js'));

const redirect = (map) => ({
  name: 'redirect',
  setup(b) {
    b.onResolve({ filter: /.*/ }, (args) => {
      for (const [re, target] of map) {
        if (re.test(args.path)) {
          const p = typeof target === 'function' ? target(args) : target;
          return p ? { path: p } : undefined;
        }
      }
      return undefined;
    });
  },
});
const common = { bundle: true, platform: 'node', format: 'esm', logLevel: 'warning', nodePaths: [path.join(out, 'node_modules')] };

if (bridge) {
  await build({
    ...common,
    entryPoints: [path.join(bridge, 'src/engine/index.ts')],
    outfile: path.join(out, 'engine.mjs'),
    plugins: [redirect([
      [/^usb$/, path.join(HERE, 'stubs/usb.mjs')],
      [/^serialport$/, path.join(HERE, 'stubs/serialport.mjs')],
    ])],
  });
}

if (client) {
  await build({
    ...common,
    entryPoints: [path.join(HERE, 'celio_entry.ts')],
    outfile: path.join(out, 'celio_client.mjs'),
    // Angular's compiler lowers @Injectable as a legacy decorator; so must we.
    tsconfigRaw: { compilerOptions: { experimentalDecorators: true } },
    // npm packages (socket.io-client, rxjs, uuid) load from node_modules at
    // run time; bundling socket.io-client's CommonJS into ESM breaks it.
    packages: 'external',
    plugins: [redirect([
      [/^CELIO_CLIENT\//, (a) => path.join(client, a.path.slice('CELIO_CLIENT/'.length)) + '.ts'],
      [/^@angular\/core$/, path.join(HERE, 'stubs/angular-core.mjs')],
      [/environments\/environment$/, path.join(HERE, 'stubs/celio-environment.mjs')],
    ])],
  });
}

if (server) {
  // As Celio-Server's own "build" script: src bundled, packages external.
  await build({
    ...common,
    entryPoints: [path.join(server, 'src/index.ts')],
    outfile: path.join(out, 'celio_server.mjs'),
    packages: 'external',
  });
}
