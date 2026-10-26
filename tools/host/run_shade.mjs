/* tools/host/run_shade.mjs — Matoki JIT wasm modülü test koşucusu (host).
 * Kullanım:
 *   1) Tekli mod:  node tools/host/run_shade.mjs modül.wasm girdi.bin çıktı.bin
 *   2) Yığın mod:  node tools/host/run_shade.mjs --batch liste.txt
 *      (liste.txt satır formatı: <modül.wasm> <girdi.bin> <çıktı.bin>)
 *
 * Girdi ikili formatı (little-endian):
 *   0..3   : u32 count (quad sayısı)
 *   4..7   : u32 varying_count
 *   8..11  : u32 uniform_bytes
 *   12..15 : u32 tex_count
 *   Ardından sırasıyla:
 *   - quads    : count * 8 byte (QuadHeader: u32 mask, u16 x, u16 y)
 *   - varyings : varying_count * count * 16 byte (f32x4 SoA)
 *   - uniforms : uniform_bytes byte
 *   - her doku (0..tex_count-1) için:
 *       u32 w, u32 h, u32 flags, u32 levels, ardından mip zincirinin tamamı
 *       (Σ_{k<levels} (w>>k)*(h>>k)) * 4 byte RGBA8 piksel verisi (shade_abi.h yerleşimi).
 *
 * Çıktı ikili formatı (little-endian):
 *   - out_rgba : 4 * count * 16 byte (f32x4 SoA, 4 kanal)
 *   - masks    : count * 4 byte (her quad'ın güncellenmiş u32 mask değeri)
 */
import fs from 'node:fs';

function alignUp(x, a) {
    return (x + (a - 1)) & ~(a - 1);
}

function runOne(wasmPath, inPath, outPath) {
    const wasmBytes = fs.readFileSync(wasmPath);
    const inBuf = fs.readFileSync(inPath);
    const inView = new DataView(inBuf.buffer, inBuf.byteOffset, inBuf.byteLength);

    let pos = 0;
    const count = inView.getUint32(pos, true); pos += 4;
    const varyingCount = inView.getUint32(pos, true); pos += 4;
    const uniformBytes = inView.getUint32(pos, true); pos += 4;
    const texCount = inView.getUint32(pos, true); pos += 4;

    const mod = new WebAssembly.Module(wasmBytes);
    const inst = new WebAssembly.Instance(mod, {});
    const mem = inst.exports.memory;
    const shade = inst.exports.shade;

    const memU8 = new Uint8Array(mem.buffer);
    const memView = new DataView(mem.buffer);

    /* Bellek yerleşimi (64 sayfa = 4 MB own_memory içinde):
     * 0x0100 : ShadeBatch (28 byte)
     * 0x0200 : Texture[8] dizisi (8 * 32 = 256 byte)
     * 0x0400+: dinamik olarak hizalanmış quads, varyings, uniforms, out_rgba, tex_px...
     */
    const batchPtr = 0x0100;
    const texArrPtr = 0x0200;
    let heapPtr = 0x0400;

    /* Quads */
    const quadsBytes = count * 8;
    const quadsPtr = alignUp(heapPtr, 16);
    heapPtr = quadsPtr + quadsBytes;
    memU8.set(new Uint8Array(inBuf.buffer, inBuf.byteOffset + pos, quadsBytes), quadsPtr);
    pos += quadsBytes;

    /* Varyings */
    const varyingsBytes = varyingCount * count * 16;
    const varyingsPtr = alignUp(heapPtr, 16);
    heapPtr = varyingsPtr + varyingsBytes;
    if (varyingsBytes > 0) {
        memU8.set(new Uint8Array(inBuf.buffer, inBuf.byteOffset + pos, varyingsBytes), varyingsPtr);
        pos += varyingsBytes;
    }

    /* Uniforms */
    const uniformsPtr = alignUp(heapPtr, 16);
    heapPtr = uniformsPtr + uniformBytes;
    if (uniformBytes > 0) {
        memU8.set(new Uint8Array(inBuf.buffer, inBuf.byteOffset + pos, uniformBytes), uniformsPtr);
        pos += uniformBytes;
    }

    /* Out RGBA */
    const outRgbaBytes = 4 * count * 16;
    const outRgbaPtr = alignUp(heapPtr, 16);
    heapPtr = outRgbaPtr + outRgbaBytes;
    memU8.fill(0, outRgbaPtr, outRgbaPtr + outRgbaBytes);

    /* Textures */
    for (let t = 0; t < texCount; t++) {
        const w = inView.getUint32(pos, true); pos += 4;
        const h = inView.getUint32(pos, true); pos += 4;
        const flags = inView.getUint32(pos, true); pos += 4;
        const levels = inView.getUint32(pos, true); pos += 4;
        let texels = 0;
        for (let k = 0; k < Math.max(levels, 1); k++) texels += (w >>> k) * (h >>> k);
        const pxBytes = texels * 4;
        const pxPtr = alignUp(heapPtr, 16);
        heapPtr = pxPtr + pxBytes;
        memU8.set(new Uint8Array(inBuf.buffer, inBuf.byteOffset + pos, pxBytes), pxPtr);
        pos += pxBytes;

        const tOff = texArrPtr + t * 32;
        memView.setUint32(tOff + 0, pxPtr, true);       /* px */
        memView.setUint32(tOff + 4, w, true);           /* w */
        memView.setUint32(tOff + 8, h, true);           /* h */
        memView.setUint32(tOff + 12, w - 1, true);      /* w_mask */
        memView.setUint32(tOff + 16, h - 1, true);      /* h_mask */
        memView.setUint32(tOff + 20, Math.clz32(1) - Math.clz32(w), true); /* log2_w */
        memView.setUint32(tOff + 24, flags, true);      /* flags */
        memView.setUint32(tOff + 28, levels, true);     /* levels */
    }

    /* İsteğe bağlı framebuffer (Blend Fusion testi için): u32 fb_w, u32 fb_h, ardından fb_w * fb_h * 4 byte */
    let fbW = 0;
    let fbH = 0;
    let fbBytes = 0;
    let fbPtr = 0;
    if (pos + 8 <= inBuf.byteLength) {
        fbW = inView.getUint32(pos, true); pos += 4;
        fbH = inView.getUint32(pos, true); pos += 4;
        fbBytes = fbW * fbH * 4;
        if (fbBytes > 0) {
            fbPtr = alignUp(heapPtr, 16);
            heapPtr = fbPtr + fbBytes;
            memU8.set(new Uint8Array(inBuf.buffer, inBuf.byteOffset + pos, fbBytes), fbPtr);
            pos += fbBytes;
        }
    }

    /* ShadeBatch başlığını doldur (shade_abi.h ofsetleri) */
    memView.setUint32(batchPtr + 0, count, true);
    memView.setUint32(batchPtr + 4, varyingCount, true);
    memView.setUint32(batchPtr + 8, quadsPtr, true);
    memView.setUint32(batchPtr + 12, varyingsPtr, true);
    memView.setUint32(batchPtr + 16, uniformsPtr, true);
    memView.setUint32(batchPtr + 20, outRgbaPtr, true);
    memView.setUint32(batchPtr + 24, texArrPtr, true);
    memView.setUint32(batchPtr + 28, fbPtr, true);
    memView.setUint32(batchPtr + 32, fbW, true);

    /* JIT derlenmiş shade fonksiyonunu çalıştır */
    shade(batchPtr);

    /* Çıktıyı (out_rgba + güncellenmiş quad maskeleri + varsa fb_px) yaz */
    const outTotal = outRgbaBytes + count * 4 + fbBytes;
    const outBuf = new Uint8Array(outTotal);
    const outView = new DataView(outBuf.buffer);

    outBuf.set(memU8.subarray(outRgbaPtr, outRgbaPtr + outRgbaBytes), 0);
    for (let q = 0; q < count; q++) {
        const m = memView.getUint32(quadsPtr + q * 8, true);
        outView.setUint32(outRgbaBytes + q * 4, m, true);
    }
    if (fbBytes > 0) {
        outBuf.set(memU8.subarray(fbPtr, fbPtr + fbBytes), outRgbaBytes + count * 4);
    }

    fs.writeFileSync(outPath, outBuf);
}

const args = process.argv.slice(2);
if (args[0] === '--batch') {
    const listText = fs.readFileSync(args[1], 'utf8');
    const lines = listText.split('\n');
    for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed) continue;
        const parts = trimmed.split(/\s+/);
        runOne(parts[0], parts[1], parts[2]);
    }
} else if (args.length >= 3) {
    runOne(args[0], args[1], args[2]);
} else {
    console.error('Usage: node run_shade.mjs <mod.wasm> <in.bin> <out.bin> | --batch <list.txt>');
    process.exit(1);
}
