import dgram from 'node:dgram';
const PORT = 20556, LOCAL = process.argv[2] || '192.168.1.3', WAIT = Number(process.argv[3] || 3000);
const MODE = ['static', 'dhcp', 'link-local'];
const frame = (op, txid) => { const h = Buffer.alloc(16); h.write('PLCD', 0, 'ascii'); h[4] = 1; h[5] = op; h.writeUInt16BE(txid, 6); return h; };
const rx = dgram.createSocket({ type: 'udp4', reuseAddr: true });
const seen = new Map();
rx.on('message', (m, ri) => {
  if (m.length < 16 || m.toString('ascii', 0, 4) !== 'PLCD' || m[5] !== 0x81) return;
  const mac = [...m.subarray(8, 14)].map(b => b.toString(16).padStart(2, '0')).join(':'), p = m.subarray(16);
  if (p.length < 38) return;
  seen.set(mac, {
    mac, from: ri.address, product: '0x' + p.readUInt32BE(0).toString(16), hw: `${p[4]}.${p[5]}`, ver: `${p[6]}.${p[7]}`,
    net: MODE[p[8]] ?? p[8], inBL: p[9] === 1, ip: `${p[10]}.${p[11]}.${p[12]}.${p[13]}`, mask: `${p[14]}.${p[15]}.${p[16]}.${p[17]}`,
    name: p.subarray(22, 38).toString('ascii').replace(/\0.*$/, ''),
  });
});
rx.bind(PORT, () => {
  rx.setBroadcast(true);
  const tx = dgram.createSocket({ type: 'udp4', reuseAddr: true });
  tx.bind(0, LOCAL, () => { tx.setBroadcast(true); for (let i = 0; i < 3; i++) setTimeout(() => tx.send(frame(1, 0x4000 + i), PORT, '255.255.255.255'), i * 300); });
  setTimeout(() => { for (const v of seen.values()) console.log(JSON.stringify(v)); console.log(`${seen.size} device(s)`); process.exit(0); }, WAIT);
});
