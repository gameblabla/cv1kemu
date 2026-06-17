#!/usr/bin/env node
/*
 * Compare CV1K browser/WASM core PCM generation against a native/headless WAV.
 * Usage:
 *   make -f Makefile.wasm
 *   ./cv1k_sandbox --model d --romset mmpork.zip --run-frames 4000 --dump-audio native.wav --ir-jit
 *   node tools/wasm_audio_compare.js web/cv1k_wasm_core.wasm mmpork.zip 4000 native.wav
 */
const fs = require('fs');
const crypto = require('crypto');

const [,, wasmPath, romPath, framesArg, nativeWavPath] = process.argv;
if (!wasmPath || !romPath || !framesArg) {
  console.error('usage: node tools/wasm_audio_compare.js <cv1k_wasm_core.wasm> <rom.zip> <frames> [native.wav]');
  process.exit(2);
}
const frameCount = Math.max(1, Number(framesArg) | 0);

function cstr(mem, ptr) {
  const u8 = new Uint8Array(mem.buffer);
  let out = '';
  for (let p = ptr >>> 0; p < u8.length && u8[p] !== 0; p++) out += String.fromCharCode(u8[p]);
  return out;
}
function sha256(buf) { return crypto.createHash('sha256').update(buf).digest('hex'); }
function pcmStats(buf) {
  let firstNonZero = -1, min = 0, max = 0, sumSq = 0;
  const samples = buf.length >>> 1;
  for (let i = 0; i < samples; i++) {
    const v = buf.readInt16LE(i << 1);
    if (v && firstNonZero < 0) firstNonZero = i;
    if (v < min) min = v;
    if (v > max) max = v;
    sumSq += v * v;
  }
  return { samples, frames: samples >>> 1, firstNonZeroSample: firstNonZero, firstNonZeroFrame: firstNonZero < 0 ? -1 : (firstNonZero >>> 1), min, max, rms: Math.sqrt(sumSq / Math.max(1, samples)) };
}
function wavPayload(buf) {
  if (buf.length >= 44 && buf.subarray(0,4).toString('ascii') === 'RIFF' && buf.subarray(8,12).toString('ascii') === 'WAVE') {
    for (let p = 12; p + 8 <= buf.length;) {
      const id = buf.subarray(p, p + 4).toString('ascii');
      const n = buf.readUInt32LE(p + 4);
      if (id === 'data') return buf.subarray(p + 8, p + 8 + n);
      p += 8 + ((n + 1) & ~1);
    }
  }
  return buf;
}
(async function main() {
  const wasmBytes = fs.readFileSync(wasmPath);
  const romBytes = fs.readFileSync(romPath);
  const { instance } = await WebAssembly.instantiate(wasmBytes, {});
  const e = instance.exports;
  const mem = e.memory;
  if (!e.cv1k_wasm_init(16000, 1)) throw new Error('cv1k_wasm_init failed: ' + cstr(mem, e.cv1k_wasm_get_error_ptr()));
  if (e.cv1k_wasm_set_jit) e.cv1k_wasm_set_jit(1);
  const ptr = e.cv1k_wasm_malloc(romBytes.length);
  new Uint8Array(mem.buffer, ptr, romBytes.length).set(romBytes);
  if (!e.cv1k_wasm_load_romset(ptr, romBytes.length)) throw new Error('ROM load failed: ' + cstr(mem, e.cv1k_wasm_get_error_ptr()));
  if (!e.cv1k_wasm_start()) throw new Error('start failed: ' + cstr(mem, e.cv1k_wasm_get_error_ptr()));

  const chunks = [];
  let audioFrames = 0;
  for (let i = 0; i < frameCount; i++) {
    if (!e.cv1k_wasm_frame(0)) throw new Error(`frame ${i} failed: ` + cstr(mem, e.cv1k_wasm_get_error_ptr()));
    const n = e.cv1k_wasm_get_audio_frames() >>> 0;
    const ap = e.cv1k_wasm_get_audio_ptr() >>> 0;
    chunks.push(Buffer.from(new Uint8Array(mem.buffer, ap, n * 4)));
    audioFrames += n;
    if (e.cv1k_wasm_audio_consume) e.cv1k_wasm_audio_consume();
  }
  const wasmPcm = Buffer.concat(chunks);
  const report = {
    wasm: {
      emuFrames: frameCount,
      audioRate: e.cv1k_wasm_get_audio_rate ? e.cv1k_wasm_get_audio_rate() : 0,
      audioFrames,
      pc: e.cv1k_wasm_get_pc ? '0x' + (e.cv1k_wasm_get_pc() >>> 0).toString(16) : null,
      sha256: sha256(wasmPcm),
      stats: pcmStats(wasmPcm)
    }
  };
  if (nativeWavPath) {
    const nativePcm = wavPayload(fs.readFileSync(nativeWavPath));
    report.native = { sha256: sha256(nativePcm), stats: pcmStats(nativePcm), byteLength: nativePcm.length };
    report.sameLength = nativePcm.length === wasmPcm.length;
    report.firstDifferentByte = -1;
    const n = Math.min(nativePcm.length, wasmPcm.length);
    for (let i = 0; i < n; i++) {
      if (nativePcm[i] !== wasmPcm[i]) { report.firstDifferentByte = i; break; }
    }
  }
  console.log(JSON.stringify(report, null, 2));
})().catch(err => { console.error(err && err.stack || String(err)); process.exit(1); });
