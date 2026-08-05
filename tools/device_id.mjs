#!/usr/bin/env node
/**
 * device_id.mjs — compute a module's network identity from its STM32 96-bit
 * unique device ID (UID), for producing device labels BEFORE flashing.
 *
 * Mirrors Application/net_id/net_id.c exactly:
 *   MAC        = 02:00 : <CRC32(UID[0..11]) as 4 big-endian bytes>
 *   link-local = 169.254.<mac[4]>.<mac[5]>  (each host octet clamped 1..254)
 *   NetBIOS    = BL-<variant>-<mac[3..5] hex>   (bootloader name)
 * CRC32 is IEEE 802.3 (init 0xFFFFFFFF, poly 0xEDB88320 reflected, final XOR).
 *
 * Usage:
 *   node device_id.mjs <uid-hex>              # 24 hex chars / 3 words, any spacing
 *   node device_id.mjs --stlink [--cli PATH]  # read UID via STM32CubeProgrammer
 *   node device_id.mjs --stlink --variant 12do
 *   node device_id.mjs <uid-hex> --csv        # MAC,link-local[,name] one line
 *
 * The UID lives at 0x1FFF7A10 on the STM32F407 (12 bytes, little-endian words).
 */
import { execFileSync } from 'node:child_process';
import process from 'node:process';

const UID_ADDR = 0x1fff7a10;

const VARIANTS = {
  '12di': 0x504c1201, '12do': 0x504c1202, '4rtd': 0x504c0403,
  '4aic': 0x504c0404, '4aiv': 0x504c0405, '4ao': 0x504c0406,
};
function variantToken(pid) {
  for (const [k, v] of Object.entries(VARIANTS)) if (v === pid) return k.toUpperCase();
  return 'MOD';
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const b of bytes) {
    crc ^= b;
    for (let k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (~crc) >>> 0;
}

/** @param {number[]} uid 12 bytes, memory order */
function identity(uid, pid) {
  if (uid.length !== 12) throw new Error(`UID must be 12 bytes, got ${uid.length}`);
  const h = crc32(uid);
  const mac = [0x02, 0x00, (h >>> 24) & 0xff, (h >>> 16) & 0xff, (h >>> 8) & 0xff, h & 0xff];
  const clamp = (v) => (v === 0 ? 1 : v === 255 ? 254 : v);
  const ip = [169, 254, clamp(mac[4]), clamp(mac[5])];
  const hx = (v) => v.toString(16).padStart(2, '0').toUpperCase();
  const macStr = mac.map(hx).join(':');
  const netbios = `BL-${variantToken(pid)}-${hx(mac[3])}${hx(mac[4])}${hx(mac[5])}`;
  return { macStr, ip: ip.join('.'), netbios };
}

/** Parse a hex blob into 12 UID bytes (memory order). Accepts 24 hex chars, or
 *  three 32-bit little-endian words (as printed by ST tools). */
function parseUidHex(str) {
  const clean = str.replace(/0x/gi, '').replace(/[^0-9a-fA-F]/g, '');
  if (clean.length !== 24) throw new Error(`expected 24 hex chars (12 bytes), got ${clean.length}`);
  // If given as three big-endian-looking words we still treat the byte stream
  // as memory order; the caller must pass memory-order bytes. ST -r32 output is
  // handled by readUidStlink() which converts words to little-endian bytes.
  const out = [];
  for (let i = 0; i < 24; i += 2) out.push(parseInt(clean.slice(i, i + 2), 16));
  return out;
}

function readUidStlink(cliPath) {
  const cli = cliPath || 'STM32_Programmer_CLI';
  const addr = '0x' + UID_ADDR.toString(16).toUpperCase();
  const out = execFileSync(cli, ['-c', 'port=SWD', 'mode=hotplug', '-r32', addr, '12'],
    { encoding: 'utf8' });
  // Find the data line: "0x1FFF7A10 : AABBCCDD EEFF0011 22334455"
  const m = out.match(new RegExp(addr.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '\\s*:\\s*([0-9A-Fa-f ]+)'));
  if (!m) throw new Error('could not parse UID from ST-Link output:\n' + out);
  const words = m[1].trim().split(/\s+/).slice(0, 3).map((w) => parseInt(w, 16) >>> 0);
  if (words.length !== 3) throw new Error('expected 3 UID words');
  const bytes = [];
  for (const w of words) bytes.push(w & 0xff, (w >>> 8) & 0xff, (w >>> 16) & 0xff, (w >>> 24) & 0xff);
  return bytes;
}

function main() {
  const argv = process.argv.slice(2);
  let uidHex = null, useStlink = false, cli = null, csv = false, variant = null;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--stlink') useStlink = true;
    else if (a === '--cli') cli = argv[++i];
    else if (a === '--csv') csv = true;
    else if (a === '--variant') variant = (argv[++i] || '').toLowerCase();
    else if (a === '-h' || a === '--help') { printUsage(); return; }
    else uidHex = (uidHex ? uidHex + ' ' : '') + a;
  }

  const pid = variant && VARIANTS[variant] ? VARIANTS[variant] : 0;
  let uid;
  try {
    uid = useStlink ? readUidStlink(cli) : uidHex ? parseUidHex(uidHex) : null;
  } catch (e) {
    console.error('ERROR: ' + e.message);
    process.exit(1);
  }
  if (!uid) { printUsage(); process.exit(1); }

  const id = identity(uid, pid);
  if (csv) {
    console.log(variant ? `${id.macStr},${id.ip},${id.netbios}` : `${id.macStr},${id.ip}`);
  } else {
    console.log(`UID:         ${uid.map((b) => b.toString(16).padStart(2, '0')).join('')}`);
    console.log(`MAC:         ${id.macStr}`);
    console.log(`Link-local:  ${id.ip}  (mask 255.255.0.0)`);
    if (variant) console.log(`NetBIOS:     ${id.netbios}`);
  }
}

function printUsage() {
  console.log('Usage:');
  console.log('  node device_id.mjs <uid-hex>                 compute from 12 UID bytes (24 hex)');
  console.log('  node device_id.mjs --stlink [--cli PATH]     read UID via STM32CubeProgrammer');
  console.log('  node device_id.mjs --stlink --variant 12do   also print the bootloader NetBIOS name');
  console.log('  node device_id.mjs <uid-hex> --csv           MAC,link-local[,name]');
}

main();
