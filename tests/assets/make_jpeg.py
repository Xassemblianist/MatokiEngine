#!/usr/bin/env python3
# tests/assets/make_jpeg.py — JPEG test varlıklarını ve ffmpeg referans PPM çıktılarını üretir.
# Yalnızca Python standart kütüphanesi ve sistemdeki `ffmpeg` aracını kullanır.
# Kullanım: python3 tests/assets/make_jpeg.py [--bench /tmp/dizin]

import math
import os
import struct
import subprocess
import sys
import tempfile

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# Standart JPEG (ITU-T T.81 Ek K) Luma/Chroma DC ve AC Huffman tabloları
STD_DC_LUMA_BITS = bytes([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0])
STD_DC_LUMA_VALS = bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11])

STD_DC_CHROMA_BITS = bytes([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0])
STD_DC_CHROMA_VALS = bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11])

STD_AC_LUMA_BITS = bytes([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125])
STD_AC_LUMA_VALS = bytes([
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16,
    0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4,
    0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
    0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
])

STD_AC_CHROMA_BITS = bytes([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119])
STD_AC_CHROMA_VALS = bytes([
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34,
    0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
    0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2,
    0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
])


def gen_ppm(w, h, seed=0):
    buf = bytearray(w * h * 3)
    inv_w = 1.0 / max(1, w - 1)
    inv_h = 1.0 / max(1, h - 1)
    sin_x = [48.0 * math.sin(x * 0.27 + seed) for x in range(w)]
    cos_y = [math.cos(y * 0.23 - seed) for y in range(h)]
    cb_x  = [14.0 * math.sin((x * inv_w) * math.pi * 1.5 + seed) for x in range(w)]
    cb_y  = [math.cos((y * inv_h) * math.pi * 1.2) for y in range(h)]
    cr_x  = [14.0 * math.cos((x * inv_w) * math.pi * 1.2 - seed) for x in range(w)]
    cr_y  = [math.sin((y * inv_h) * math.pi * 1.5) for y in range(h)]
    idx = 0
    for y in range(h):
        cy = cos_y[y]
        cby = cb_y[y]
        cry = cr_y[y]
        yb4 = (y // 4) & 1
        for x in range(w):
            yv = 128.0 + sin_x[x] * cy + (16.0 if (((x // 4) & 1) == yb4) else 0.0)
            cb = cb_x[x] * cby
            cr = cr_x[x] * cry
            r = int(round(yv + 1.402 * cr))
            g = int(round(yv - 0.344136 * cb - 0.714136 * cr))
            b = int(round(yv + 1.772 * cb))
            buf[idx]     = 0 if r < 0 else (255 if r > 255 else r)
            buf[idx + 1] = 0 if g < 0 else (255 if g > 255 else g)
            buf[idx + 2] = 0 if b < 0 else (255 if b > 255 else b)
            idx += 3
    return f"P6\n{w} {h}\n255\n".encode() + bytes(buf)


def build_huff_decode_table(bits, vals):
    fast = [None] * 256
    slow = {}
    code = 0
    k = 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            sym = vals[k]
            if length <= 8:
                prefix = code << (8 - length)
                for f in range(1 << (8 - length)):
                    fast[prefix + f] = (sym, length)
            else:
                slow[(length, code)] = sym
            code += 1
            k += 1
        code <<= 1
    return (fast, slow)


def build_huff_encode_table(bits, vals):
    enc = {}
    code = 0
    k = 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            enc[vals[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return enc


class JpegBitReader:
    def __init__(self, data, pos):
        self.data = data
        self.pos = pos
        self.bitbuf = 0
        self.bitcnt = 0

    def _fill(self):
        while self.bitcnt < 24 and self.pos < len(self.data):
            b = self.data[self.pos]
            if b == 0xFF:
                if self.pos + 1 >= len(self.data):
                    break
                nxt = self.data[self.pos + 1]
                if nxt == 0x00:
                    self.pos += 2
                else:
                    break
            else:
                self.pos += 1
            self.bitbuf = (self.bitbuf << 8) | b
            self.bitcnt += 8

    def get_bits(self, n):
        if n == 0:
            return 0
        self._fill()
        self.bitcnt -= n
        return (self.bitbuf >> self.bitcnt) & ((1 << n) - 1)

    def decode(self, htab):
        if self.bitcnt < 16:
            self._fill()
        fast, slow = htab
        if self.bitcnt >= 8:
            peek = (self.bitbuf >> (self.bitcnt - 8)) & 0xFF
            ent = fast[peek]
            if ent is not None:
                self.bitcnt -= ent[1]
                return ent[0]
        code = 0
        for length in range(1, 17):
            code = (code << 1) | self.get_bits(1)
            if length <= 8:
                ent = fast[code << (8 - length)]
                if ent is not None and ent[1] == length:
                    return ent[0]
            elif (length, code) in slow:
                return slow[(length, code)]
        raise ValueError("Invalid Huffman code")


class JpegBitWriter:
    def __init__(self):
        self.out = bytearray()
        self.bitbuf = 0
        self.bitcnt = 0

    def put_bits(self, val, n):
        if n == 0:
            return
        self.bitbuf = (self.bitbuf << n) | (val & ((1 << n) - 1))
        self.bitcnt += n
        while self.bitcnt >= 8:
            self.bitcnt -= 8
            b = (self.bitbuf >> self.bitcnt) & 0xFF
            self.out.append(b)
            if b == 0xFF:
                self.out.append(0x00)

    def flush(self):
        if self.bitcnt > 0:
            pad = 8 - self.bitcnt
            self.put_bits((1 << pad) - 1, pad)
        return bytes(self.out)


def extend_val(v, sz):
    if sz == 0:
        return 0
    if v < (1 << (sz - 1)):
        return v - ((1 << sz) - 1)
    return v


def mag_cat(v):
    av = abs(v)
    sz = 0
    while av > 0:
        sz += 1
        av >>= 1
    if v >= 0:
        bits = v
    else:
        bits = v + ((1 << sz) - 1)
    return sz, bits


def parse_baseline_jpeg(jpg_bytes):
    assert jpg_bytes[:2] == b"\xFF\xD8"
    pos = 2
    quant = {}
    dc_huff = {}
    ac_huff = {}
    w = h = 0
    comps = []
    while pos < len(jpg_bytes):
        while jpg_bytes[pos] == 0xFF:
            pos += 1
        marker = jpg_bytes[pos]
        pos += 1
        if marker == 0xD9:
            break
        seg_len = struct.unpack(">H", jpg_bytes[pos:pos + 2])[0]
        seg = jpg_bytes[pos + 2:pos + seg_len]
        pos += seg_len
        if marker == 0xDB:
            qpos = 0
            while qpos < len(seg):
                tq = seg[qpos] & 0x0F
                quant[tq] = seg[qpos + 1:qpos + 65]
                qpos += 65
        elif marker == 0xC4:
            hpos = 0
            while hpos < len(seg):
                tc = seg[hpos] >> 4
                th = seg[hpos] & 0x0F
                bits = seg[hpos + 1:hpos + 17]
                total = sum(bits)
                vals = seg[hpos + 17:hpos + 17 + total]
                hpos += 17 + total
                if tc == 0:
                    dc_huff[th] = build_huff_decode_table(bits, vals)
                else:
                    ac_huff[th] = build_huff_decode_table(bits, vals)
        elif marker == 0xC0:
            h, w, nf = struct.unpack(">BHHB", seg[:6])[1:]
            for i in range(nf):
                cid, hv, tq = struct.unpack(">BBB", seg[6 + 3 * i:9 + 3 * i])
                comps.append({"id": cid, "h": hv >> 4, "v": hv & 0x0F, "tq": tq})
        elif marker == 0xDA:
            ns = seg[0]
            for i in range(ns):
                cid = seg[1 + 2 * i]
                td_ta = seg[2 + 2 * i]
                for c in comps:
                    if c["id"] == cid:
                        c["td"] = td_ta >> 4
                        c["ta"] = td_ta & 0x0F
            h_max = max(c["h"] for c in comps)
            v_max = max(c["v"] for c in comps)
            mcu_cols = (w + 8 * h_max - 1) // (8 * h_max)
            mcu_rows = (h + 8 * v_max - 1) // (8 * v_max)
            for c in comps:
                c["bw"] = mcu_cols * c["h"]
                c["bh"] = mcu_rows * c["v"]
                c["comp_w"] = (w * c["h"] + h_max - 1) // h_max
                c["comp_h"] = (h * c["v"] + v_max - 1) // v_max
                c["sbw"] = (c["comp_w"] + 7) // 8
                c["sbh"] = (c["comp_h"] + 7) // 8
                c["blocks"] = [[None] * c["bw"] for _ in range(c["bh"])]
                c["dc_pred"] = 0
            br = JpegBitReader(jpg_bytes, pos)
            for my in range(mcu_rows):
                for mx in range(mcu_cols):
                    for c in comps:
                        for vy in range(c["v"]):
                            for hx in range(c["h"]):
                                blk = [0] * 64
                                s = br.decode(dc_huff[c["td"]])
                                diff = extend_val(br.get_bits(s), s) if s > 0 else 0
                                c["dc_pred"] += diff
                                blk[0] = c["dc_pred"]
                                k = 1
                                while k < 64:
                                    rs = br.decode(ac_huff[c["ta"]])
                                    r = rs >> 4
                                    sz = rs & 0x0F
                                    if sz == 0:
                                        if r == 15:
                                            k += 16
                                        else:
                                            break
                                    else:
                                        k += r
                                        blk[k] = extend_val(br.get_bits(sz), sz)
                                        k += 1
                                c["blocks"][my * c["v"] + vy][mx * c["h"] + hx] = blk
            break
    return w, h, quant, comps


def make_seg(marker, payload):
    return bytes([0xFF, marker]) + struct.pack(">H", len(payload) + 2) + payload


def build_grayscale_jpeg(jpg_bytes):
    w, h, quant, comps = parse_baseline_jpeg(jpg_bytes)
    cy = comps[0]
    dc_enc = build_huff_encode_table(STD_DC_LUMA_BITS, STD_DC_LUMA_VALS)
    ac_enc = build_huff_encode_table(STD_AC_LUMA_BITS, STD_AC_LUMA_VALS)

    out = bytearray(b"\xFF\xD8")
    out += make_seg(0xDB, bytes([0x00]) + quant[cy["tq"]])
    out += make_seg(0xC0, struct.pack(">BHHBBBB", 8, h, w, 1, 1, 0x11, 0))
    out += make_seg(0xC4, bytes([0x00]) + STD_DC_LUMA_BITS + STD_DC_LUMA_VALS)
    out += make_seg(0xC4, bytes([0x10]) + STD_AC_LUMA_BITS + STD_AC_LUMA_VALS)
    out += make_seg(0xDA, struct.pack(">BBBBBB", 1, 1, 0x00, 0, 63, 0))

    bw = JpegBitWriter()
    dc_pred = 0
    for by in range(cy["sbh"]):
        for bx in range(cy["sbw"]):
            blk = cy["blocks"][by][bx]
            diff = blk[0] - dc_pred
            dc_pred = blk[0]
            sz, bits = mag_cat(diff)
            code, clen = dc_enc[sz]
            bw.put_bits(code, clen)
            bw.put_bits(bits, sz)

            zero_run = 0
            for k in range(1, 64):
                v = blk[k]
                if v == 0:
                    zero_run += 1
                else:
                    while zero_run >= 16:
                        c, cl = ac_enc[0xF0]
                        bw.put_bits(c, cl)
                        zero_run -= 16
                    sz, bits = mag_cat(v)
                    c, cl = ac_enc[(zero_run << 4) | sz]
                    bw.put_bits(c, cl)
                    bw.put_bits(bits, sz)
                    zero_run = 0
            if zero_run > 0:
                c, cl = ac_enc[0x00]
                bw.put_bits(c, cl)

    out += bw.flush()
    out += b"\xFF\xD9"
    return bytes(out)


def flush_eobrun(bw, eobrun, ac_enc, ref_bits=None):
    if eobrun == 0:
        return
    r = eobrun.bit_length() - 1
    c, cl = ac_enc[r << 4]
    bw.put_bits(c, cl)
    if r > 0:
        bw.put_bits(eobrun - (1 << r), r)
    if ref_bits:
        for rb in ref_bits:
            bw.put_bits(rb, 1)
        ref_bits.clear()


def encode_prog_ac_first(blocks, ss, se, al, ac_enc):
    bw = JpegBitWriter()
    eobrun = 0
    for blk in blocks:
        last_nz = se
        while last_nz >= ss and ((blk[last_nz] >> al) if blk[last_nz] >= 0 else -((-blk[last_nz]) >> al)) == 0:
            last_nz -= 1
        if last_nz < ss:
            eobrun += 1
            if eobrun == 32767:
                flush_eobrun(bw, eobrun, ac_enc)
                eobrun = 0
            continue
        if eobrun > 0:
            flush_eobrun(bw, eobrun, ac_enc)
            eobrun = 0
        zero_run = 0
        for k in range(ss, last_nz + 1):
            v = blk[k]
            v_shifted = (v >> al) if v >= 0 else -((-v) >> al)
            if v_shifted == 0:
                zero_run += 1
            else:
                while zero_run >= 16:
                    c, cl = ac_enc[0xF0]
                    bw.put_bits(c, cl)
                    zero_run -= 16
                sz, bits = mag_cat(v_shifted)
                c, cl = ac_enc[(zero_run << 4) | sz]
                bw.put_bits(c, cl)
                bw.put_bits(bits, sz)
                zero_run = 0
        if last_nz < se:
            eobrun = 1
    flush_eobrun(bw, eobrun, ac_enc)
    return bw.flush()


def encode_prog_ac_refine(blocks, ss, se, ah, al, ac_enc):
    bw = JpegBitWriter()
    eobrun = 0
    eob_ref_bits = []
    for blk in blocks:
        last_act = se
        while last_act >= ss and (abs(blk[last_act]) >> al) == 0:
            last_act -= 1
        if last_act < ss:
            eobrun += 1
            if eobrun == 32767:
                flush_eobrun(bw, eobrun, ac_enc, eob_ref_bits)
                eobrun = 0
            continue
        zero_run = 0
        ref_bits = []
        pending_zrls = []
        for k in range(ss, last_act + 1):
            v = blk[k]
            av = v if v >= 0 else -v
            prev = av >> ah
            cur = av >> al
            if prev != 0:
                ref_bits.append(cur & 1)
            else:
                if cur == 0:
                    zero_run += 1
                    if zero_run == 16:
                        pending_zrls.append(ref_bits)
                        ref_bits = []
                        zero_run = 0
                else:
                    if eobrun > 0:
                        flush_eobrun(bw, eobrun, ac_enc, eob_ref_bits)
                        eobrun = 0
                    for zrl_bits in pending_zrls:
                        c, cl = ac_enc[0xF0]
                        bw.put_bits(c, cl)
                        for rb in zrl_bits:
                            bw.put_bits(rb, 1)
                    pending_zrls = []
                    c, cl = ac_enc[(zero_run << 4) | 1]
                    bw.put_bits(c, cl)
                    bw.put_bits(1 if v > 0 else 0, 1)
                    for rb in ref_bits:
                        bw.put_bits(rb, 1)
                    ref_bits = []
                    zero_run = 0
        for zrl_bits in pending_zrls:
            eob_ref_bits.extend(zrl_bits)
        eob_ref_bits.extend(ref_bits)
        if last_act < se or zero_run > 0 or len(pending_zrls) > 0 or len(ref_bits) > 0:
            eobrun += 1
            if eobrun == 32767:
                flush_eobrun(bw, eobrun, ac_enc, eob_ref_bits)
                eobrun = 0
    flush_eobrun(bw, eobrun, ac_enc, eob_ref_bits)
    return bw.flush()


def make_prog_ac_table(std_bits, std_vals):
    # 1 adet 12-bit kodu 16-bit'e indirip 14 adet EOB1..EOB14 (0x10..0xE0) sembolünü ekler (Kraft korunur)
    bits = bytearray(std_bits)
    bits[11] -= 1
    bits[15] += 1 + 14
    vals = bytes(std_vals) + bytes([r << 4 for r in range(1, 15)])
    return bytes(bits), vals


def build_progressive_jpeg(jpg_bytes):
    w, h, quant, comps = parse_baseline_jpeg(jpg_bytes)
    prog_ac_y_bits, prog_ac_y_vals = make_prog_ac_table(STD_AC_LUMA_BITS, STD_AC_LUMA_VALS)
    prog_ac_c_bits, prog_ac_c_vals = make_prog_ac_table(STD_AC_CHROMA_BITS, STD_AC_CHROMA_VALS)
    dc_enc_y = build_huff_encode_table(STD_DC_LUMA_BITS, STD_DC_LUMA_VALS)
    dc_enc_c = build_huff_encode_table(STD_DC_CHROMA_BITS, STD_DC_CHROMA_VALS)
    ac_enc_y = build_huff_encode_table(prog_ac_y_bits, prog_ac_y_vals)
    ac_enc_c = build_huff_encode_table(prog_ac_c_bits, prog_ac_c_vals)

    out = bytearray(b"\xFF\xD8")
    for tq, qbytes in sorted(quant.items()):
        out += make_seg(0xDB, bytes([tq]) + qbytes)

    sof_payload = bytearray(struct.pack(">BHHB", 8, h, w, len(comps)))
    for c in comps:
        sof_payload += struct.pack(">BBB", c["id"], (c["h"] << 4) | c["v"], c["tq"])
    out += make_seg(0xC2, bytes(sof_payload))  # SOF2 (Progressive)

    out += make_seg(0xC4, bytes([0x00]) + STD_DC_LUMA_BITS + STD_DC_LUMA_VALS)
    out += make_seg(0xC4, bytes([0x01]) + STD_DC_CHROMA_BITS + STD_DC_CHROMA_VALS)
    out += make_seg(0xC4, bytes([0x10]) + prog_ac_y_bits + prog_ac_y_vals)
    out += make_seg(0xC4, bytes([0x11]) + prog_ac_c_bits + prog_ac_c_vals)

    h_max = max(c["h"] for c in comps)
    v_max = max(c["v"] for c in comps)
    mcu_cols = (w + 8 * h_max - 1) // (8 * h_max)
    mcu_rows = (h + 8 * v_max - 1) // (8 * v_max)

    # 1) Etkileşimli DC ilk tarama (Ss=0, Se=0, Ah=0, Al=1)
    sos_dc1 = bytearray([len(comps)])
    for i, c in enumerate(comps):
        sos_dc1 += bytes([c["id"], 0x00 if i == 0 else 0x11])
    sos_dc1 += bytes([0, 0, 0x01])
    out += make_seg(0xDA, bytes(sos_dc1))

    bw = JpegBitWriter()
    preds = [0] * len(comps)
    for my in range(mcu_rows):
        for mx in range(mcu_cols):
            for ci, c in enumerate(comps):
                enc = dc_enc_y if ci == 0 else dc_enc_c
                for vy in range(c["v"]):
                    for hx in range(c["h"]):
                        blk = c["blocks"][my * c["v"] + vy][mx * c["h"] + hx]
                        dc_al = blk[0] >> 1
                        diff = dc_al - preds[ci]
                        preds[ci] = dc_al
                        sz, bits = mag_cat(diff)
                        code, clen = enc[sz]
                        bw.put_bits(code, clen)
                        bw.put_bits(bits, sz)
    out += bw.flush()

    # 2) Etkileşimli DC iyileştirme taraması (Ss=0, Se=0, Ah=1, Al=0)
    sos_dc2 = bytearray([len(comps)])
    for i, c in enumerate(comps):
        sos_dc2 += bytes([c["id"], 0x00 if i == 0 else 0x11])
    sos_dc2 += bytes([0, 0, 0x10])
    out += make_seg(0xDA, bytes(sos_dc2))
    bw = JpegBitWriter()
    for my in range(mcu_rows):
        for mx in range(mcu_cols):
            for ci, c in enumerate(comps):
                for vy in range(c["v"]):
                    for hx in range(c["h"]):
                        blk = c["blocks"][my * c["v"] + vy][mx * c["h"] + hx]
                        bw.put_bits(blk[0] & 1, 1)
    out += bw.flush()

    # 3) Her bileşen için AC ilk (Al=1) ve AC iyileştirme (Ah=1, Al=0) taramaları
    for ci, c in enumerate(comps):
        ac_enc = ac_enc_y if ci == 0 else ac_enc_c
        ta = 0x00 if ci == 0 else 0x01
        scan_blks = [c["blocks"][by][bx] for by in range(c["sbh"]) for bx in range(c["sbw"])]

        # Düşük frekans AC ilk (Ss=1..5, Al=1)
        out += make_seg(0xDA, bytes([1, c["id"], ta, 1, 5, 0x01]))
        out += encode_prog_ac_first(scan_blks, 1, 5, 1, ac_enc)

        # Yüksek frekans AC ilk (Ss=6..63, Al=1)
        out += make_seg(0xDA, bytes([1, c["id"], ta, 6, 63, 0x01]))
        out += encode_prog_ac_first(scan_blks, 6, 63, 1, ac_enc)

        # Tam bant AC iyileştirme (Ss=1..63, Ah=1, Al=0)
        out += make_seg(0xDA, bytes([1, c["id"], ta, 1, 63, 0x10]))
        out += encode_prog_ac_refine(scan_blks, 1, 63, 1, 0, ac_enc)

    out += b"\xFF\xD9"
    return bytes(out)


def ffmpeg_encode(ppm_bytes, jpg_path, pix_fmt="yuvj420p", extra_args=None):
    cmd = ["ffmpeg", "-y", "-f", "image2pipe", "-vcodec", "ppm", "-i", "-", "-pix_fmt", pix_fmt, "-q:v", "2"]
    if extra_args:
        cmd.extend(extra_args)
    cmd.append(jpg_path)
    subprocess.run(cmd, input=ppm_bytes, capture_output=True, check=True)


def ffmpeg_decode_ppm(jpg_path, ppm_path):
    subprocess.run(["ffmpeg", "-y", "-i", jpg_path, "-pix_fmt", "rgb24", ppm_path],
                   capture_output=True, check=True)


def generate_all(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    ppm_48 = gen_ppm(48, 48, seed=1)

    # 1) 4:4:4, 4:2:2, 4:2:0
    ffmpeg_encode(ppm_48, os.path.join(out_dir, "color_444.jpg"), "yuvj444p")
    ffmpeg_encode(ppm_48, os.path.join(out_dir, "color_422.jpg"), "yuvj422p")
    ffmpeg_encode(ppm_48, os.path.join(out_dir, "color_420.jpg"), "yuvj420p")

    # 2) Restart aralıklı (DRI + RSTn)
    ffmpeg_encode(ppm_48, os.path.join(out_dir, "restart_420.jpg"), "yuvj420p", ["-slices", "4"])

    # 3) Gri (1 bileşenli SOF0) ve Progressive (SOF2: DC/AC ilk + iyileştirme)
    base_420 = open(os.path.join(out_dir, "color_420.jpg"), "rb").read()
    with open(os.path.join(out_dir, "gray.jpg"), "wb") as f:
        f.write(build_grayscale_jpeg(base_420))
    with open(os.path.join(out_dir, "prog_420.jpg"), "wb") as f:
        f.write(build_progressive_jpeg(base_420))

    # 4) Tek blok olmayan (odd) boyutlar: 33x17, 1x1, 257x129
    ffmpeg_encode(gen_ppm(33, 17, seed=2), os.path.join(out_dir, "odd_33x17.jpg"), "yuvj420p")
    ffmpeg_encode(gen_ppm(1, 1, seed=3), os.path.join(out_dir, "odd_1x1.jpg"), "yuvj444p")
    ffmpeg_encode(gen_ppm(257, 129, seed=4), os.path.join(out_dir, "odd_257x129.jpg"), "yuvj420p")

    # Tüm JPEG'ler için ffmpeg ile çözülmüş referans .ppm çıktılarını üret
    names = [
        "gray", "color_444", "color_422", "color_420",
        "restart_420", "prog_420",
        "odd_33x17", "odd_1x1", "odd_257x129"
    ]
    total_bytes = 0
    for name in names:
        jpg_p = os.path.join(out_dir, f"{name}.jpg")
        ppm_p = os.path.join(out_dir, f"{name}_ref.ppm")
        ffmpeg_decode_ppm(jpg_p, ppm_p)
        total_bytes += os.path.getsize(jpg_p) + os.path.getsize(ppm_p)

    print(f"uretildi: {len(names)} jpg + ref ppm, toplam {total_bytes / 1024:.1f} KB")


def generate_bench(bench_dir):
    os.makedirs(bench_dir, exist_ok=True)
    ppm_1024 = gen_ppm(1024, 1024, seed=7)
    base_path = os.path.join(bench_dir, "bench_1024_base.jpg")
    prog_path = os.path.join(bench_dir, "bench_1024_prog.jpg")
    ffmpeg_encode(ppm_1024, base_path, "yuvj420p")
    with open(prog_path, "wb") as f:
        f.write(build_progressive_jpeg(open(base_path, "rb").read()))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--bench":
        generate_bench(sys.argv[2])
    else:
        generate_all(OUT_DIR)
