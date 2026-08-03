#!/usr/bin/env node
// @ts-check
/*
 * fw_update.mjs - Bootloader OTA client for PLCJS ETH MODULE (STM32F407VGT6)
 *
 * Node.js port of fw_update.py. Speaks the same Modbus TCP register protocol
 * (port 502) used by the bootloader. No external dependencies: a minimal
 * Modbus TCP client is implemented on top of the built-in `node:net` socket.
 *
 * Requires Node.js 18+ (uses ES modules, top-level await, AbortController).
 *
 * Usage:
 *   node fw_update.mjs status                 # Read and print bootloader state
 *   node fw_update.mjs update <file.bin>      # Upload firmware binary
 *   node fw_update.mjs abort                  # Abort current update session
 *   node fw_update.mjs reboot                 # Software reset
 *   node fw_update.mjs app-bootloader         # Ask application to enter bootloader
 *   node fw_update.mjs app-reboot             # Reboot the application
 *   node fw_update.mjs app-factory-reset      # Reset application settings
 *
 * Options:
 *   --ip <addr>        Bootloader IP        (alias for --boot-ip)
 *   --boot-ip <addr>   Bootloader IP        (default 192.168.1.2)
 *   --app-ip <addr>    Application IP       (default 192.168.1.10)
 *   --port <n>         Modbus TCP port      (default 502)
 *   --version <hex>    FW version 0xMMmmpp  (default 0x00010000), for `update`
 */

import net from 'node:net';
import { readFile } from 'node:fs/promises';
import process from 'node:process';

// ---------------------------------------------------------------------------
// Target configuration
// ---------------------------------------------------------------------------
const BOOTLOADER_TARGET_IP = '192.168.1.2';
const APP_TARGET_IP = '192.168.1.10';
const TARGET_PORT = 502;
const UNIT_ID = 1;

const PRODUCT_ID_DEFAULT = 0x12d1d4a0;
const HW_REVISION_DEFAULT = 1;

const FW_MAX_BLOCK_SIZE = 240; // Must match FW_MAX_BLOCK_SIZE in flash_map.h
const STAGING_FLASH_SIZE = 256 * 1024; // 256 KB

// ---------------------------------------------------------------------------
// Input Register map (FC04, base 0x0000) - read-only
// ---------------------------------------------------------------------------
const IR_MAGIC_HI = 0x0000;
const IR_VERSION_HI = 0x0002;
const IR_BOOT_STATE = 0x0004;
const IR_APP_VALID = 0x0005;
const IR_APP_VER_HI = 0x0006;
const IR_PRODUCT_ID_HI = 0x0008;
// NOTE: HW revision is a 32-bit value (2 registers, 0x0A-0x0B) in the current
// bootloader, so every field from last_error onward is shifted +1 vs. the old
// 16-bit-hw layout. These offsets must match fw_proto_read_input_regs().
const IR_HW_REV = 0x000a;         // 0x0A-0x0B (major.minor.patch)
const IR_LAST_ERROR = 0x000c;
const IR_BLOCK_COUNT_HI = 0x000d;
const IR_RECV_BLOCKS_HI = 0x000f;
const IR_IMAGE_SIZE_HI = 0x0011;
const IR_IMAGE_CRC_HI = 0x0013;
const IR_CMD_STATUS = 0x0015;
const IR_STAGING_VALID = 0x0016;
const IR_APP_PRODUCT_ID_HI = 0x0017; // installed app product_id (0x17-0x18)
const IR_COUNT = 0x0019; // total registers to read (through 0x18)

// ---------------------------------------------------------------------------
// Holding Register map (FC03/FC16, base 0x0000) - read/write
// ---------------------------------------------------------------------------
const HR_CMD = 0x0000;
const HR_BLOCK_BASE = 0x0100;

// Commands (written to HR_CMD)
const CMD_BEGIN_UPDATE = 1;
const CMD_FINALIZE_UPDATE = 2;
const CMD_INSTALL_UPDATE = 3;
const CMD_ABORT_UPDATE = 4;
const CMD_REBOOT = 5;

const CMD_STATUS_IDLE = 0;
const CMD_STATUS_BUSY = 1;
const CMD_STATUS_OK = 2;
const CMD_STATUS_ERROR = 3;

const BOOTLOADER_MAGIC = 0xb00710ad;
const BOOT_STATE_WAIT_COMMAND = 2;

const APP_HR_TRIGGER = 118;
const APP_HR_FACTORY_RESET = 119;
const APP_CMD_REBOOT = 0xb00b;
const APP_CMD_BOOTLOADER = 0xb007;
const APP_CMD_FACTORY_RESET = 0xdead;

const BOOT_STATE_NAMES = {
  0: 'BOOT_START',
  1: 'BOOT_CHECK_ENTRY',
  2: 'BOOT_WAIT_COMMAND',
  3: 'BOOT_PREPARE_UPDATE',
  4: 'BOOT_RECEIVE_FW',
  5: 'BOOT_VERIFY_STAGING',
  6: 'BOOT_INSTALL_FW',
  7: 'BOOT_VERIFY_APP',
  8: 'BOOT_READY_TO_BOOT',
  9: 'BOOT_ERROR',
};

const ERROR_NAMES = {
  0: 'NONE',
  1: 'PRODUCT_MISMATCH',
  2: 'HW_REV_MISMATCH',
  3: 'IMAGE_TOO_LARGE',
  4: 'BLOCK_CRC',
  5: 'IMAGE_CRC',
  6: 'FLASH_ERASE',
  7: 'FLASH_WRITE',
  8: 'APP_VALIDATE',
  9: 'UPDATE_TIMEOUT',
  10: 'BLOCK_INDEX',
  11: 'BAD_PARAMS',
};

const CMD_STATUS_NAMES = { 0: 'IDLE', 1: 'BUSY', 2: 'OK', 3: 'ERROR' };

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3 / zlib - matches crc32_calc() in the bootloader)
// ---------------------------------------------------------------------------
const CRC32_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
})();

/** @param {Buffer|Uint8Array} data */
function crc32(data) {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    crc = CRC32_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// ---------------------------------------------------------------------------
// Minimal Modbus TCP client
// ---------------------------------------------------------------------------
class ModbusError extends Error {}

class ModbusTcpClient {
  /** @param {string} ip @param {number} port @param {number} unitId @param {number} timeoutMs */
  constructor(ip, port, unitId, timeoutMs = 5000) {
    this.ip = ip;
    this.port = port;
    this.unitId = unitId;
    this.timeoutMs = timeoutMs;
    /** @type {import('node:net').Socket|null} */
    this.socket = null;
    this.txId = 0;
    this.rxBuf = Buffer.alloc(0);
    /** @type {Array<{resolve:Function,reject:Function,txId:number}>} */
    this.pending = [];
  }

  connect() {
    return new Promise((resolve, reject) => {
      const sock = net.createConnection({ host: this.ip, port: this.port });
      const onError = (err) => reject(err);
      sock.once('error', onError);
      sock.setNoDelay(true);
      sock.once('connect', () => {
        sock.removeListener('error', onError);
        this.socket = sock;
        sock.on('data', (chunk) => this._onData(chunk));
        sock.on('error', (err) => this._failAll(err));
        sock.on('close', () => this._failAll(new ModbusError('connection closed')));
        resolve();
      });
    });
  }

  close() {
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = null;
    }
  }

  /** @param {Error} err */
  _failAll(err) {
    const waiters = this.pending.splice(0);
    for (const w of waiters) w.reject(err);
  }

  /** @param {Buffer} chunk */
  _onData(chunk) {
    this.rxBuf = Buffer.concat([this.rxBuf, chunk]);
    // Parse as many complete MBAP frames as available.
    while (this.rxBuf.length >= 7) {
      const len = this.rxBuf.readUInt16BE(4); // unit id + PDU bytes
      const total = 6 + len;
      if (this.rxBuf.length < total) break;
      const frame = this.rxBuf.subarray(0, total);
      this.rxBuf = this.rxBuf.subarray(total);
      const txId = frame.readUInt16BE(0);
      const pdu = frame.subarray(7); // skip MBAP (7 bytes)
      const waiterIdx = this.pending.findIndex((w) => w.txId === txId);
      if (waiterIdx >= 0) {
        const [waiter] = this.pending.splice(waiterIdx, 1);
        waiter.resolve(pdu);
      }
    }
  }

  /** @param {Buffer} pdu @returns {Promise<Buffer>} */
  _request(pdu) {
    if (!this.socket) return Promise.reject(new ModbusError('not connected'));
    this.txId = (this.txId + 1) & 0xffff;
    const txId = this.txId;
    const mbap = Buffer.alloc(7);
    mbap.writeUInt16BE(txId, 0); // transaction id
    mbap.writeUInt16BE(0, 2); // protocol id
    mbap.writeUInt16BE(pdu.length + 1, 4); // length = unit id + PDU
    mbap.writeUInt8(this.unitId, 6); // unit id
    const frame = Buffer.concat([mbap, pdu]);

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const idx = this.pending.findIndex((w) => w.txId === txId);
        if (idx >= 0) this.pending.splice(idx, 1);
        reject(new ModbusError(`Modbus timeout (txId=${txId})`));
      }, this.timeoutMs);

      this.pending.push({
        txId,
        resolve: (pdu) => {
          clearTimeout(timer);
          resolve(pdu);
        },
        reject: (err) => {
          clearTimeout(timer);
          reject(err);
        },
      });
      this.socket.write(frame);
    });
  }

  /** @param {Buffer} pdu @param {number} expectedFunc */
  _checkException(pdu, expectedFunc) {
    const func = pdu.readUInt8(0);
    if (func === (expectedFunc | 0x80)) {
      const exc = pdu.readUInt8(1);
      throw new ModbusError(`Modbus exception ${exc} for function ${expectedFunc}`);
    }
    if (func !== expectedFunc) {
      throw new ModbusError(`Unexpected function ${func} (expected ${expectedFunc})`);
    }
  }

  /** FC04 read input registers. @param {number} addr @param {number} qty @returns {Promise<number[]>} */
  async readInputRegisters(addr, qty) {
    const pdu = Buffer.alloc(5);
    pdu.writeUInt8(0x04, 0);
    pdu.writeUInt16BE(addr, 1);
    pdu.writeUInt16BE(qty, 3);
    const resp = await this._request(pdu);
    this._checkException(resp, 0x04);
    const byteCount = resp.readUInt8(1);
    const regs = [];
    for (let i = 0; i < byteCount / 2; i++) regs.push(resp.readUInt16BE(2 + i * 2));
    return regs;
  }

  /** FC06 write single register. @param {number} addr @param {number} value */
  async writeSingleRegister(addr, value) {
    const pdu = Buffer.alloc(5);
    pdu.writeUInt8(0x06, 0);
    pdu.writeUInt16BE(addr, 1);
    pdu.writeUInt16BE(value & 0xffff, 3);
    const resp = await this._request(pdu);
    this._checkException(resp, 0x06);
  }

  /** FC16 write multiple registers. @param {number} addr @param {number[]} values */
  async writeMultipleRegisters(addr, values) {
    const qty = values.length;
    const byteCount = qty * 2;
    const pdu = Buffer.alloc(6 + byteCount);
    pdu.writeUInt8(0x10, 0);
    pdu.writeUInt16BE(addr, 1);
    pdu.writeUInt16BE(qty, 3);
    pdu.writeUInt8(byteCount, 5);
    for (let i = 0; i < qty; i++) pdu.writeUInt16BE(values[i] & 0xffff, 6 + i * 2);
    const resp = await this._request(pdu);
    this._checkException(resp, 0x10);
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** @param {number[]} regs @param {number} base */
function u32FromRegs(regs, base) {
  return ((regs[base] << 16) | regs[base + 1]) >>> 0;
}

/**
 * Pack firmware bytes into a Modbus 16-bit register list (big-endian per reg),
 * padding an odd trailing byte with 0xFF - mirrors bytes_to_regs() in Python.
 * @param {Buffer} data @returns {number[]}
 */
function bytesToRegs(data) {
  const regs = [];
  for (let i = 0; i < data.length; i += 2) {
    const hi = data[i];
    const lo = i + 1 < data.length ? data[i + 1] : 0xff;
    regs.push((hi << 8) | lo);
  }
  return regs;
}

/** @param {ModbusTcpClient} c */
async function readInputRegs(c) {
  return c.readInputRegisters(0, IR_COUNT);
}

/**
 * Poll IR_CMD_STATUS until it matches `expected` or ERROR, or timeout.
 * @param {ModbusTcpClient} c @param {number} expected @param {number} timeoutS
 */
async function waitForStatus(c, expected, timeoutS = 30) {
  const deadline = Date.now() + timeoutS * 1000;
  while (Date.now() < deadline) {
    const regs = await readInputRegs(c);
    const st = regs[IR_CMD_STATUS];
    if (st === expected) return true;
    if (st === CMD_STATUS_ERROR) {
      const err = regs[IR_LAST_ERROR];
      console.log(`  Bootloader error: ${ERROR_NAMES[err] ?? err} (${err})`);
      return false;
    }
    await sleep(50);
  }
  console.log(`  Timeout waiting for status ${CMD_STATUS_NAMES[expected] ?? expected}`);
  return false;
}

/**
 * Wait until the bootloader becomes reachable and sits in BOOT_WAIT_COMMAND.
 * @param {{bootIp:string,port:number}} args
 * @param {number} timeoutS
 */
async function waitForBootloaderReady(args, timeoutS = 20) {
  const deadline = Date.now() + timeoutS * 1000;
  let lastNotice = '';

  while (Date.now() < deadline) {
    const c = new ModbusTcpClient(args.bootIp, args.port, UNIT_ID, 1200);
    try {
      await c.connect();
      const regs = await readInputRegs(c);
      const magic = u32FromRegs(regs, IR_MAGIC_HI);
      const state = regs[IR_BOOT_STATE];

      if (magic === BOOTLOADER_MAGIC && state === BOOT_STATE_WAIT_COMMAND) {
        return true;
      }

      lastNotice = `bootloader responded but is not ready yet (magic=0x${magic.toString(16).padStart(8, '0')}, state=${BOOT_STATE_NAMES[state] ?? state})`;
    } catch (e) {
      lastNotice = e?.message ? String(e.message) : String(e);
    } finally {
      c.close();
    }

    await sleep(300);
  }

  if (lastNotice) console.log(`  Last bootloader probe: ${lastNotice}`);
  console.log(`  Timeout waiting for bootloader on ${args.bootIp}:${args.port}`);
  return false;
}

/** @param {{appIp:string,port:number}} args @param {number} registerAddr @param {number} value @param {string} label */
async function sendAppAction(args, registerAddr, value, label) {
  const c = new ModbusTcpClient(args.appIp, args.port, UNIT_ID);
  await c.connect();
  try {
    console.log(`Sending ${label} to application at ${args.appIp}:${args.port}...`);
    try {
      await c.writeSingleRegister(registerAddr, value);
    } catch {
      // Application may reset before replying; that is acceptable here.
    }
  } finally {
    c.close();
  }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
/** @param {{ip:string,port:number}} args */
async function cmdStatus(args) {
  const c = new ModbusTcpClient(args.ip, args.port, UNIT_ID);
  await c.connect();
  try {
    const regs = await readInputRegs(c);
    const magic = u32FromRegs(regs, IR_MAGIC_HI);
    if (magic !== BOOTLOADER_MAGIC) {
      console.log(`WARNING: unexpected magic 0x${magic.toString(16).padStart(8, '0')} (expected 0xB00710AD)`);
    } else {
      console.log(`Magic:          0x${magic.toString(16).toUpperCase().padStart(8, '0')}  OK`);
    }

    const ver = u32FromRegs(regs, IR_VERSION_HI);
    console.log(`BL version:     ${ver >>> 16}.${(ver >>> 8) & 0xff}.${ver & 0xff}`);

    const state = regs[IR_BOOT_STATE];
    console.log(`Boot state:     ${BOOT_STATE_NAMES[state] ?? state} (${state})`);
    console.log(`App valid:      ${regs[IR_APP_VALID]}`);
    const appVer = u32FromRegs(regs, IR_APP_VER_HI);
    console.log(`App version:    ${appVer >>> 16}.${(appVer >>> 8) & 0xff}.${appVer & 0xff}`);
    const prodId = u32FromRegs(regs, IR_PRODUCT_ID_HI);
    console.log(`Product ID:     0x${prodId.toString(16).toUpperCase().padStart(8, '0')}`);
    console.log(`HW revision:    ${regs[IR_HW_REV]}`);
    const err = regs[IR_LAST_ERROR];
    console.log(`Last error:     ${ERROR_NAMES[err] ?? err} (${err})`);
    const blockCount = u32FromRegs(regs, IR_BLOCK_COUNT_HI);
    const recvBlocks = u32FromRegs(regs, IR_RECV_BLOCKS_HI);
    if (blockCount) console.log(`Blocks:         ${recvBlocks}/${blockCount} received`);
    const imageSize = u32FromRegs(regs, IR_IMAGE_SIZE_HI);
    const imageCrc = u32FromRegs(regs, IR_IMAGE_CRC_HI);
    if (imageSize) {
      console.log(`Image size:     ${imageSize} bytes`);
      console.log(`Image CRC32:    0x${imageCrc.toString(16).toUpperCase().padStart(8, '0')}`);
    }
    const st = regs[IR_CMD_STATUS];
    console.log(`Cmd status:     ${CMD_STATUS_NAMES[st] ?? st} (${st})`);
    console.log(`Staging valid:  ${regs[IR_STAGING_VALID]}`);
  } finally {
    c.close();
  }
}

/** @param {{ip:string,port:number}} args */
async function cmdAbort(args) {
  const c = new ModbusTcpClient(args.ip, args.port, UNIT_ID);
  await c.connect();
  try {
    console.log('Sending CMD_ABORT_UPDATE...');
    await c.writeSingleRegister(HR_CMD, CMD_ABORT_UPDATE);
    if (await waitForStatus(c, CMD_STATUS_OK, 5)) console.log('Aborted OK');
  } finally {
    c.close();
  }
}

/** @param {{ip:string,port:number}} args */
async function cmdReboot(args) {
  const c = new ModbusTcpClient(args.ip, args.port, UNIT_ID);
  await c.connect();
  try {
    console.log('Sending CMD_REBOOT...');
    // The device resets immediately and will not send a Modbus response, so a
    // timeout / connection drop here is expected and not an error.
    try {
      await c.writeSingleRegister(HR_CMD, CMD_REBOOT);
    } catch {
      // expected: device rebooted before replying
    }
    console.log('Device is rebooting.');
  } finally {
    c.close();
  }
}

/** @param {{appIp:string,port:number}} args */
async function cmdAppReboot(args) {
  await sendAppAction(args, APP_HR_TRIGGER, APP_CMD_REBOOT, 'APP reboot command');
  console.log('Application reboot command sent.');
}

/** @param {{appIp:string,port:number,bootIp:string}} args */
async function cmdAppBootloader(args) {
  await sendAppAction(args, APP_HR_TRIGGER, APP_CMD_BOOTLOADER, 'APP bootloader command');
  console.log('Application is switching to bootloader mode...');

  if (await waitForBootloaderReady(args, 20)) {
    console.log(`Bootloader is ready at ${args.bootIp}:${args.port}`);
    await cmdStatus({ ip: args.bootIp, port: args.port });
  } else {
    process.exit(1);
  }
}

/** @param {{appIp:string,port:number}} args */
async function cmdAppFactoryReset(args) {
  await sendAppAction(args, APP_HR_FACTORY_RESET, APP_CMD_FACTORY_RESET, 'APP factory reset command');
  console.log('Application factory reset command sent.');
}

/** @param {{ip:string,port:number,firmware:string,version:number}} args */
async function cmdUpdate(args) {
  let fwData;
  try {
    fwData = await readFile(args.firmware);
  } catch (e) {
    console.log(`ERROR: cannot open ${args.firmware}: ${e.message}`);
    process.exit(1);
  }

  const imageSize = fwData.length;
  if (imageSize === 0 || imageSize > STAGING_FLASH_SIZE) {
    console.log(`ERROR: image size ${imageSize} bytes is out of range (1..${STAGING_FLASH_SIZE})`);
    process.exit(1);
  }

  const imageCrc = crc32(fwData);
  const blockSize = FW_MAX_BLOCK_SIZE;
  const blockCount = Math.ceil(imageSize / blockSize);
  const fwVersion = args.version;

  console.log(`Firmware:    ${args.firmware}`);
  console.log(`Size:        ${imageSize} bytes`);
  console.log(`CRC32:       0x${imageCrc.toString(16).toUpperCase().padStart(8, '0')}`);
  console.log(`Block size:  ${blockSize} bytes`);
  console.log(`Blocks:      ${blockCount}`);
  console.log(`FW version:  0x${(fwVersion >>> 0).toString(16).toUpperCase().padStart(8, '0')}`);
  console.log('');

  const c = new ModbusTcpClient(args.ip, args.port, UNIT_ID);
  await c.connect();
  try {
    // ---- Verify bootloader is reachable ----------------------------------
    let regs = await readInputRegs(c);
    const magic = u32FromRegs(regs, IR_MAGIC_HI);
    if (magic !== 0xb00710ad) {
      console.log(`ERROR: unexpected magic 0x${magic.toString(16).padStart(8, '0')} - is bootloader running?`);
      process.exit(1);
    }
    const state = regs[IR_BOOT_STATE];
    if (![2, 4, 5, 9].includes(state)) {
      console.log(`ERROR: unexpected boot state ${BOOT_STATE_NAMES[state] ?? state}`);
      process.exit(1);
    }

    // Use the bootloader's own product_id / hw_revision for the BEGIN_UPDATE
    // pre-check (variant-agnostic — no hard-coded per-variant identity). The
    // image's real identity is still validated against the bootloader from its
    // embedded fw_header at FINALIZE, so a wrong-variant image is still caught.
    const targetProdId = u32FromRegs(regs, IR_PRODUCT_ID_HI);
    const hwRev32 = u32FromRegs(regs, IR_HW_REV);
    const hwRevBegin = ((((hwRev32 >>> 16) & 0xff) << 8) | ((hwRev32 >>> 8) & 0xff)) >>> 0;
    console.log(`Target:      product_id 0x${targetProdId.toString(16).toUpperCase().padStart(8, '0')}, hw ${(hwRev32 >>> 16) & 0xff}.${(hwRev32 >>> 8) & 0xff}`);
    console.log('');

    // ---- BEGIN_UPDATE ----------------------------------------------------
    console.log('Step 1/4  BEGIN_UPDATE (erasing staging...)');
    // Parameter block starts at HR[0x0010]: image_size(2), crc32(2), fw_ver(2),
    // prod_id(2), hw_rev(1), block_size(1), block_count(2).
    const params = [
      (imageSize >>> 16) & 0xffff,
      imageSize & 0xffff,
      (imageCrc >>> 16) & 0xffff,
      imageCrc & 0xffff,
      (fwVersion >>> 16) & 0xffff,
      fwVersion & 0xffff,
      (targetProdId >>> 16) & 0xffff,
      targetProdId & 0xffff,
      hwRevBegin,
      blockSize,
      (blockCount >>> 16) & 0xffff,
      blockCount & 0xffff,
    ];
    await c.writeMultipleRegisters(0x0010, params);
    await c.writeSingleRegister(HR_CMD, CMD_BEGIN_UPDATE);
    // Staging erase takes ~1s per sector x 2 sectors ~ 2-4s.
    if (!(await waitForStatus(c, CMD_STATUS_OK, 20))) {
      console.log('BEGIN_UPDATE failed');
      process.exit(1);
    }
    console.log('  OK');

    // ---- WRITE_BLOCK -----------------------------------------------------
    console.log(`Step 2/4  Sending ${blockCount} blocks...`);
    for (let blkIdx = 0; blkIdx < blockCount; blkIdx++) {
      const offset = blkIdx * blockSize;
      const chunk = fwData.subarray(offset, offset + blockSize);
      const dataLen = chunk.length;

      // [block_idx_hi, block_idx_lo, data_len, data...]
      const payload = [(blkIdx >>> 16) & 0xffff, blkIdx & 0xffff, dataLen, ...bytesToRegs(chunk)];

      await c.writeMultipleRegisters(HR_BLOCK_BASE, payload);
      if (!(await waitForStatus(c, CMD_STATUS_OK, 5))) {
        console.log(`\n  Block ${blkIdx} write failed`);
        process.exit(1);
      }

      const pct = Math.floor(((blkIdx + 1) * 100) / blockCount);
      process.stdout.write(`  ${blkIdx + 1}/${blockCount}  [${String(pct).padStart(3, ' ')}%]\r`);
    }
    console.log(`  ${blockCount}/${blockCount}  [100%]  done          `);

    // ---- FINALIZE (CRC check of staging) ---------------------------------
    console.log('Step 3/4  FINALIZE (CRC32 check of staging area...)');
    await c.writeSingleRegister(HR_CMD, CMD_FINALIZE_UPDATE);
    if (!(await waitForStatus(c, CMD_STATUS_OK, 10))) {
      console.log('FINALIZE failed - CRC mismatch or blocks missing');
      process.exit(1);
    }
    console.log('  Staging CRC OK');

    // ---- INSTALL ---------------------------------------------------------
    console.log('Step 4/4  INSTALL (erasing app, copying, verifying...)');
    await c.writeSingleRegister(HR_CMD, CMD_INSTALL_UPDATE);
    if (!(await waitForStatus(c, CMD_STATUS_OK, 30))) {
      console.log('INSTALL failed');
      process.exit(1);
    }
    console.log('  Install OK');
    console.log('');
    console.log('Firmware update complete. Device will reboot to application.');
  } finally {
    c.close();
  }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
function parseArgs(argv) {
  const args = {
    ip: BOOTLOADER_TARGET_IP,
    bootIp: BOOTLOADER_TARGET_IP,
    appIp: APP_TARGET_IP,
    port: TARGET_PORT,
    version: 0x00010000,
    command: null,
    firmware: null,
  };
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--ip' || a === '--boot-ip') {
      args.ip = argv[++i];
      args.bootIp = args.ip;
    }
    else if (a === '--app-ip') args.appIp = argv[++i];
    else if (a === '--port') args.port = parseInt(argv[++i], 10);
    else if (a === '--version') args.version = parseInt(argv[++i], 0) >>> 0;
    else if (a === '-h' || a === '--help') args.command = 'help';
    else positional.push(a);
  }
  if (!args.command) args.command = positional.shift() ?? null;
  if (args.command === 'update') args.firmware = positional.shift() ?? null;
  return args;
}

function usage() {
  console.log(`PLCJS Bootloader OTA client (Modbus TCP)

Usage:
  node fw_update.mjs status                 Read bootloader status registers
  node fw_update.mjs update <file.bin>      Upload firmware binary
  node fw_update.mjs abort                  Abort current update session
  node fw_update.mjs reboot                 Software reset the device
  node fw_update.mjs app-reboot             Reboot the main application
  node fw_update.mjs app-bootloader         Put the application into bootloader mode
  node fw_update.mjs app-factory-reset      Reset application settings

Options:
  --ip <addr>        Bootloader IP       (alias for --boot-ip)
  --boot-ip <addr>   Bootloader IP       (default ${BOOTLOADER_TARGET_IP})
  --app-ip <addr>    Application IP      (default ${APP_TARGET_IP})
  --port <n>         Modbus TCP port      (default ${TARGET_PORT})
  --version <hex>    FW version 0xMMmmpp  (default 0x00010000)`);
}

async function main() {
  const args = parseArgs(process.argv.slice(2));

  if (args.command === 'help' || !args.command) {
    usage();
    process.exit(args.command ? 0 : 1);
  }

  try {
    switch (args.command) {
      case 'status':
        await cmdStatus(args);
        break;
      case 'abort':
        await cmdAbort(args);
        break;
      case 'reboot':
        await cmdReboot(args);
        break;
      case 'app-reboot':
        await cmdAppReboot(args);
        break;
      case 'app-bootloader':
        await cmdAppBootloader(args);
        break;
      case 'app-factory-reset':
        await cmdAppFactoryReset(args);
        break;
      case 'update':
        if (!args.firmware) {
          console.log('ERROR: `update` requires a firmware .bin path');
          process.exit(1);
        }
        await cmdUpdate(args);
        break;
      default:
        console.log(`ERROR: unknown command '${args.command}'`);
        usage();
        process.exit(1);
    }
  } catch (e) {
    console.log(`ERROR: ${e.message ?? e}`);
    process.exit(1);
  }
}

await main();
