import test from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import { ModbusTcpClient, verifyBootloaderIdentity, normalizeMac, selectBootloader,
  pdpParseIdentify } from '../tools/fw_update.mjs';

const mac = '02:00:12:34:56:78';
function identity() {
  const regs = new Array(28).fill(0);
  regs[0] = 0xb007; regs[1] = 0x10ad; regs[3] = 0x0108;
  regs[25] = 0x0200; regs[26] = 0x1234; regs[27] = 0x5678;
  return regs;
}

async function withDevice(regs, action) {
  const requests = [];
  const server = net.createServer((socket) => {
    let input = Buffer.alloc(0);
    socket.on('data', (chunk) => {
      input = Buffer.concat([input, chunk]);
      while (input.length >= 7 && input.length >= 6 + input.readUInt16BE(4)) {
        const size = 6 + input.readUInt16BE(4);
        const request = input.subarray(0, size);
        input = input.subarray(size);
        requests.push(request[7]);
        assert.equal(request[7], 4, 'MAC validation must not write registers');
        const start = request.readUInt16BE(8), count = request.readUInt16BE(10);
        const response = Buffer.alloc(9 + 2 * count);
        request.copy(response, 0, 0, 7);
        response.writeUInt16BE(3 + 2 * count, 4);
        response[7] = 4; response[8] = count * 2;
        regs.slice(start, start + count).forEach((r, i) => response.writeUInt16BE(r, 9 + 2 * i));
        socket.write(response.subarray(0, 5));
        socket.write(response.subarray(5));
      }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = new ModbusTcpClient('127.0.0.1', server.address().port, 1, 1000);
  try {
    await client.connect();
    await action(client, requests);
  } finally {
    client.close();
    await new Promise((resolve) => server.close(resolve));
  }
}

test('MAC is checked on the connected TCP endpoint', async () => {
  await withDevice(identity(), async (c, requests) => {
    const regs = await verifyBootloaderIdentity(c, mac);
    assert.equal(regs[3], 0x0108);
    assert.deepEqual(requests, [4]);
  });
  const wrong = identity(); wrong[27]++;
  await withDevice(wrong, async (c, requests) => {
    await assert.rejects(verifyBootloaderIdentity(c, mac), /MAC mismatch/);
    assert.deepEqual(requests, [4]);
  });
});

test('legacy bootloader and invalid target fail closed', async () => {
  const old = identity(); old[3] = 0x0107; old.fill(0, 25);
  await withDevice(old, async (c) => {
    await assert.rejects(verifyBootloaderIdentity(c, mac), /does not support/);
  });
  for (const value of ['', '00:00:00:00:00:00', 'ff:ff:ff:ff:ff:ff', 'abc'])
    assert.throws(() => normalizeMac(value));
  assert.equal(normalizeMac('02-00-12-34-56-78'), mac);
});

test('discovery never picks an arbitrary module or ambiguous IP', () => {
  const a = {mac, ip: '169.254.86.120', inBoot: true};
  const b = {mac: '02:00:12:34:56:79', ip: '169.254.86.121', inBoot: true};
  assert.equal(selectBootloader([a], null), a);
  assert.throws(() => selectBootloader([a, b], null), /Multiple/);
  assert.equal(selectBootloader([a, b], mac), a);
  assert.equal(selectBootloader([b], mac), null);
  assert.throws(() => selectBootloader([a, {...b, ip: a.ip}], mac), /IP conflict/);
});

test('PDP fixed layout and protocol version are required', () => {
  const reply = Buffer.alloc(54);
  reply.write('PLCD'); reply[4] = 1; reply[5] = 0x81;
  Buffer.from(mac.split(':').map((b) => parseInt(b, 16))).copy(reply, 8);
  reply.writeUInt16BE(38, 14);
  reply[25] = 1;
  assert.equal(pdpParseIdentify(reply).mac, mac);
  reply[4] = 2;
  assert.equal(pdpParseIdentify(reply), null);
  reply[4] = 1;
  assert.equal(pdpParseIdentify(reply.subarray(0, 53)), null);
});
