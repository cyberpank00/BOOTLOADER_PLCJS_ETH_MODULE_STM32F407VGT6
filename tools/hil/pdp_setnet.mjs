import dgram from 'node:dgram';
// usage: node pdp_setnet.mjs <localNic> <mac> <ip> [mask] [gw]
const [LOCAL, MAC, IP, MASK = '255.255.255.0', GW = '192.168.1.1'] = process.argv.slice(2);
const PORT = 20556;
const oct = s => s.split('.').map(Number);
const mac = MAC.split(':').map(x => parseInt(x, 16));
const pl = Buffer.from([0, ...oct(IP), ...oct(MASK), ...oct(GW)]);   // mode 0 = static
const f = Buffer.alloc(16 + pl.length);
f.write('PLCD', 0, 'ascii'); f[4] = 1; f[5] = 0x02; f.writeUInt16BE(0x5E71, 6);
Buffer.from(mac).copy(f, 8); f.writeUInt16BE(pl.length, 14); pl.copy(f, 16);
const rx = dgram.createSocket({ type: 'udp4', reuseAddr: true });
let ok = null;
rx.on('message', m => { if (m.length >= 17 && m.toString('ascii', 0, 4) === 'PLCD' && m[5] === 0x82 && m[6] === 0x5E && m[7] === 0x71) ok = m[16] === 0; });
rx.bind(PORT, () => {
  rx.setBroadcast(true);
  const tx = dgram.createSocket({ type: 'udp4', reuseAddr: true });
  tx.bind(0, LOCAL, () => { tx.setBroadcast(true); for (let i = 0; i < 3; i++) setTimeout(() => tx.send(f, PORT, '255.255.255.255'), i * 250); });
  setTimeout(() => { console.log(`${MAC} -> ${IP}: ${ok === null ? 'no reply' : ok ? 'OK' : 'REJECTED'}`); process.exit(0); }, 2000);
});
