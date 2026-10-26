/* tests/test_asset_json.c — JSON ayrıştırıcı testleri: 90+ geçerli/geçersiz vaka, hata konumu,
 * sayı dönüşümünün Python float() ile ULP karşılaştırması (popen), ~1 MB rastgele belge gidiş-dönüşü
 * ve 100.000 rastgele mutasyonla fuzz (host ASan/UBSan açık → çökme/taşma yakalanır).
 */
#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/log.h"
#include "asset/json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static u8 g_mem[MB(48)];

static b32 parse(const char* s, JsonValue* v, JsonError* e, Arena* a) {
    return json_parse(s, (u32)strlen(s), a, v, e);
}

TEST(json_valid_cases) {
    static const char* ok[] = {
        "0", "-0", "1", "-1", "123456789", "1.5", "-1.25e-3", "1E10", "1e+2", "0.0", "1e-400", "1e400",
        "\"\"", "\"abc\"", "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "\"\\u0041\"", "\"\\u00e7\\u011f\"",
        "\"\\ud83d\\ude00\"", "\"ğüşıöç\"", "\"\xF0\x9F\x98\x80\"", "true", "false", "null",
        "[]", "[1]", "[1,2,3]", "[ 1 , 2 ]", "[[]]", "[[[[[]]]]]", "{}", "{\"a\":1}", "{\"a\":{\"b\":[1,{}]}}",
        " \t\r\n 1 \t\r\n", "\xEF\xBB\xBF{}", "[\"a\",true,null,-0.5]", "{\"\":0}", "{\"a\":1,\"a\":2}",
        "[1e1,1E-1,1.0e+1]", "-123.456e-7", "[0,-0,0.0]", "\"/\"", "{\"x\":[],\"y\":{}}",
        "123456789012345678901234567890", "0.1", "9007199254740993", "4.9e-324", "1.7976931348623157e308",
    };
    Arena a;
    for (u32 i = 0; i < ARRAY_LEN(ok); i++) {
        arena_init(&a, g_mem, MB(1));
        JsonValue v; JsonError e;
        b32 r = parse(ok[i], &v, &e, &a);
        if (!r) LOG("gecmesi gereken: %s -> %u:%u %s", ok[i], e.line, e.col, e.msg);
        CHECK(r);
    }
    /* Anlam kontrolleri */
    arena_init(&a, g_mem, MB(1));
    JsonValue v; JsonError e;
    CHECK(parse("{\"a\":1,\"b\":[true,\"x\\u00e7\"],\"a\":7}", &v, &e, &a));
    CHECK(json_num(json_get(&v, "a"), 0) == 7.0);                 /* son kazanır */
    CHECK(json_len(json_get(&v, "b")) == 2);
    CHECK(json_bool(json_at(json_get(&v, "b"), 0), FALSE));
    CHECK(strcmp(json_str(json_at(json_get(&v, "b"), 1), ""), "x\xC3\xA7") == 0);
    CHECK(parse("\"\\ud83d\\ude00\"", &v, &e, &a) && v.count == 4 && (u8)v.as.str[0] == 0xF0);
    CHECK(parse("1e400", &v, &e, &a) && v.as.num > 1e308);
    CHECK(parse("-0", &v, &e, &a) && v.as.num == 0.0 && (1.0 / v.as.num) < 0.0);
}

TEST(json_invalid_cases) {
    static const char* bad[] = {
        "", " ", "{", "}", "[", "]", "[1,]", "[,1]", "{\"a\"}", "{\"a\":}", "{a:1}", "{\"a\":1,}", "{,}",
        "01", "-", "+1", "1.", ".5", "1e", "1e+", "0x10", "NaN", "Infinity", "-Infinity", "1 2",
        "tru", "nul", "falsey", "\"abc", "\"\\x\"", "\"\\u12\"", "\"\\uZZZZ\"", "\"\\ud800\"", "\"\\udc00\"",
        "\"\\ud800\\u0041\"", "\"a\nb\"", "\"\x01\"", "\"\xC0\xAF\"", "\"\xED\xA0\x80\"", "\"\xF8\x88\x80\x80\x80\"",
        "\"\xC3\"", "\"\xE2\x82\"", "\"\xFF\"", "[1 2]", "{\"a\" 1}", "{\"a\":1 \"b\":2}", "'a'", "[\"a\",]",
        "{} {}", "[]]", "\"\\\"", "-01", "1.e3", "--1", "[-]", "{\"a\":[}", "[1,{]",
    };
    Arena a;
    for (u32 i = 0; i < ARRAY_LEN(bad); i++) {
        arena_init(&a, g_mem, MB(1));
        JsonValue v; JsonError e;
        b32 r = parse(bad[i], &v, &e, &a);
        if (r) LOG("reddedilmesi gereken kabul edildi: [%s]", bad[i]);
        CHECK(!r);
        CHECK(e.line >= 1 && e.msg[0] != 0);
    }
    /* Derinlik sınırı */
    static char deep[2 * (JSON_MAX_DEPTH + 10) + 1];
    u32 n = JSON_MAX_DEPTH + 10;
    for (u32 i = 0; i < n; i++) { deep[i] = '['; deep[n + i] = ']'; }
    deep[2 * n] = 0;
    arena_init(&a, g_mem, MB(4));
    JsonValue v; JsonError e;
    CHECK(!parse(deep, &v, &e, &a));
    /* Hata konumu */
    CHECK(!parse("{\n  \"a\": 1,\n  \"b\": tru\n}", &v, &e, &a));
    CHECK(e.line == 3);
    CHECK(e.col == 8);
}

static f64 ulp_dist(f64 a, f64 b) {
    i64 x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    if (x < 0) x = (i64)0x8000000000000000ull - x;
    if (y < 0) y = (i64)0x8000000000000000ull - y;
    f64 d = (f64)(x - y);
    return d < 0 ? -d : d;
}

/* 2000 rastgele sayı metni → bizim çözümümüz vs Python float() (doğru yuvarlanmış referans) */
TEST(json_number_accuracy_vs_python) {
    static char nums[2000][40];
    Rng rng;
    rng_seed(&rng, 2024);
    if (system("mkdir -p build/test_out") != 0) { CHECK(FALSE); return; }
    FILE* f = fopen("build/test_out/json_nums.txt", "w");
    if (!f) { CHECK(f != NULL); return; }
    for (u32 i = 0; i < 2000; i++) {
        u32 nd = 1u + rng_u32(&rng) % 20u;
        u32 k = 0;
        if (rng_u32(&rng) & 1u) nums[i][k++] = '-';
        nums[i][k++] = (char)('1' + rng_u32(&rng) % 9u);
        if (nd > 1) {
            nums[i][k++] = '.';
            for (u32 d = 1; d < nd; d++) nums[i][k++] = (char)('0' + rng_u32(&rng) % 10u);
        }
        i32 ex = (i32)(rng_u32(&rng) % 640u) - 320;
        if (i < 1000) ex = (i32)(rng_u32(&rng) % 60u) - 30;       /* yarısı tipik aralık */
        k += (u32)snprintf(nums[i] + k, sizeof(nums[i]) - k, "e%d", ex);
        nums[i][k] = 0;
        fprintf(f, "%s\n", nums[i]);
    }
    fclose(f);
    FILE* py = popen("python3 -c \"import sys,struct\nfor l in open('build/test_out/json_nums.txt'):\n"
                     "  print(struct.unpack('<Q', struct.pack('<d', float(l)))[0])\"", "r");
    if (!py) { CHECK(py != NULL); return; }
    Arena a;
    f64 max_ulp_typ = 0.0, max_ulp_all = 0.0;
    u32 exact = 0;
    for (u32 i = 0; i < 2000; i++) {
        unsigned long long bits = 0;
        if (fscanf(py, "%llu", &bits) != 1) { CHECK(FALSE); break; }
        f64 ref;
        u64 b64 = (u64)bits;
        memcpy(&ref, &b64, 8);
        arena_init(&a, g_mem, MB(1));
        JsonValue v; JsonError e;
        CHECK(parse(nums[i], &v, &e, &a));
        f64 d = ulp_dist(v.as.num, ref);
        if (d == 0.0) exact++;
        if (i < 1000 && d > max_ulp_typ) max_ulp_typ = d;
        if (d > max_ulp_all) max_ulp_all = d;
    }
    pclose(py);
    LOG("json sayi: %u/2000 tam, tipik aralik (1e+-30) max %.0f ULP, tum aralik (1e+-320) max %.0f ULP",
        exact, max_ulp_typ, max_ulp_all);
    CHECK(max_ulp_typ <= 2.0);
    CHECK(max_ulp_all <= 8.0);
}

static void gen_json(Rng* r, char* buf, u32* n, u32 cap, u32 depth) {
    u32 kind = (depth > 6) ? (rng_u32(r) % 4u) : (rng_u32(r) % 6u);
    if (*n + 64 >= cap) kind = 0;
    switch (kind) {
    case 0: *n += (u32)snprintf(buf + *n, cap - *n, "%d", (int)(rng_u32(r) % 2000000u) - 1000000); break;
    case 1: *n += (u32)snprintf(buf + *n, cap - *n, "%.6e", (double)(rng_f32(r) * 1e6f - 5e5f)); break;
    case 2: {
        buf[(*n)++] = '"';
        u32 l = rng_u32(r) % 12u;
        for (u32 i = 0; i < l; i++) {
            u32 c = rng_u32(r) % 40u;
            if (c == 0) { memcpy(buf + *n, "\\n", 2); *n += 2; }
            else if (c == 1) { memcpy(buf + *n, "\\u00e7", 6); *n += 6; }
            else if (c == 2) { memcpy(buf + *n, "\xC4\x9F", 2); *n += 2; }
            else buf[(*n)++] = (char)('a' + c % 26u);
        }
        buf[(*n)++] = '"';
    } break;
    case 3: *n += (u32)snprintf(buf + *n, cap - *n, "%s", (rng_u32(r) & 1u) ? "true" : "null"); break;
    case 4: {
        buf[(*n)++] = '[';
        u32 l = rng_u32(r) % 8u;
        for (u32 i = 0; i < l; i++) { if (i) buf[(*n)++] = ','; gen_json(r, buf, n, cap, depth + 1); }
        buf[(*n)++] = ']';
    } break;
    default: {
        buf[(*n)++] = '{';
        u32 l = rng_u32(r) % 8u;
        for (u32 i = 0; i < l; i++) {
            if (i) buf[(*n)++] = ',';
            *n += (u32)snprintf(buf + *n, cap - *n, "\"k%u\":", i);
            gen_json(r, buf, n, cap, depth + 1);
        }
        buf[(*n)++] = '}';
    } break;
    }
}

static b32 json_eq(const JsonValue* a, const JsonValue* b) {
    if (a->type != b->type || a->count != b->count) return FALSE;
    switch (a->type) {
    case JSON_NULL: return TRUE;
    case JSON_BOOL: return a->as.b == b->as.b;
    case JSON_NUMBER: {
        f64 d = a->as.num - b->as.num, m = a->as.num < 0 ? -a->as.num : a->as.num;
        if (d < 0) d = -d;
        return d <= m * 1e-14 + 1e-300;
    }
    case JSON_STRING: return memcmp(a->as.str, b->as.str, a->count) == 0;
    case JSON_ARRAY:
        for (u32 i = 0; i < a->count; i++) if (!json_eq(&a->as.items[i], &b->as.items[i])) return FALSE;
        return TRUE;
    case JSON_OBJECT:
        for (u32 i = 0; i < a->count; i++) {
            if (a->as.members[i].key_len != b->as.members[i].key_len) return FALSE;
            if (memcmp(a->as.members[i].key, b->as.members[i].key, a->as.members[i].key_len)) return FALSE;
            if (!json_eq(&a->as.members[i].value, &b->as.members[i].value)) return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

TEST(json_roundtrip_1mb) {
    static char src[MB(1) + 4096], dst[MB(2)];
    Rng r;
    rng_seed(&r, 77);
    u32 n = 0;
    src[n++] = '[';
    u32 items = 0;
    while (n < MB(1)) {
        if (items++) src[n++] = ',';
        gen_json(&r, src, &n, sizeof(src) - 8, 0);
    }
    src[n++] = ']';
    src[n] = 0;
    Arena a;
    arena_init(&a, g_mem, MB(48));
    JsonValue v1, v2; JsonError e;
    CHECK(json_parse(src, n, &a, &v1, &e));
    u32 wn = json_write(&v1, dst, sizeof(dst));
    CHECK(wn < sizeof(dst));
    CHECK(json_parse(dst, wn, &a, &v2, &e));
    CHECK(json_eq(&v1, &v2));
}

TEST(json_fuzz_mutations) {
    static const char* seed =
        "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"mesh\":0,\"translation\":[1.5,-2,3e-1]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"s\":\"\\u00e7\\ud83d\\ude00 x\",\"b\":[true,false,null]}";
    static char buf[512];
    u32 len = (u32)strlen(seed);
    Rng r;
    rng_seed(&r, 31337);
    Arena a;
    u32 accepted = 0;
    for (u32 it = 0; it < 100000; it++) {
        memcpy(buf, seed, len);
        u32 n = len;
        u32 muts = 1u + rng_u32(&r) % 4u;
        for (u32 k = 0; k < muts; k++) {
            u32 pos = rng_u32(&r) % n;
            u32 op = rng_u32(&r) % 3u;
            if (op == 0) buf[pos] = (char)(rng_u32(&r) & 0xFFu);
            else if (op == 1 && n > 1) { memmove(buf + pos, buf + pos + 1, n - pos - 1); n--; }
            else if (n + 1 < sizeof(buf)) { memmove(buf + pos + 1, buf + pos, n - pos); buf[pos] = "{}[]\",:\\0e-"[rng_u32(&r) % 11u]; n++; }
        }
        arena_init(&a, g_mem, MB(1));
        JsonValue v; JsonError e;
        accepted += json_parse(buf, n, &a, &v, &e) ? 1u : 0u;
    }
    LOG("json fuzz: 100000 mutasyon, %u gecerli kabul edildi, cokme yok", accepted);
    CHECK(accepted < 100000);
}
