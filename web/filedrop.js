// web/filedrop.js — Sürükle-bırak dosya köprüsü: bırakılan dosyaların baytlarını wasm belleğine kopyalar
// ve engine_file_begin/alloc/add/end export'larını çağırır. Ayrıştırma tamamen C'de (src/asset/).

export function installFileDrop(exports, memory) {
  if (typeof exports.engine_file_begin !== "function") return;
  const enc = new TextEncoder();
  window.addEventListener("dragover", (e) => e.preventDefault());
  window.addEventListener("drop", async (e) => {
    e.preventDefault();
    const files = Array.from(e.dataTransfer?.files || []);
    if (files.length === 0) return;
    const blobs = await Promise.all(files.map((f) => f.arrayBuffer()));
    exports.engine_file_begin();
    files.forEach((f, i) => {
      const name = enc.encode(f.name);
      const data = new Uint8Array(blobs[i]);
      const np = exports.engine_file_alloc(name.length);
      const dp = exports.engine_file_alloc(data.length);
      new Uint8Array(memory.buffer, np, name.length).set(name);
      new Uint8Array(memory.buffer, dp, data.length).set(data);
      exports.engine_file_add(np, name.length, dp, data.length);
    });
    const ok = exports.engine_file_end();
    console.log(`[dosya] ${files.map((f) => f.name).join(", ")} → ${ok ? "yüklendi" : "hata"}`);
  });
}
