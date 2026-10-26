/* tools/host/test_main.c — native test koşucusu (libc serbest).
 * Kullanım: build/host/test_runner [isim_filtresi]
 * Çıkış kodu: kalan test varsa 1.
 */
#include <stdio.h>
#include <string.h>
#include "../../tests/test.h"

typedef struct { const char* name; const char* file; TestFn fn; } TestEntry;

static TestEntry g_tests[2048];
static int g_count;
static int g_cur_failed;

void test_register(const char* name, const char* file, TestFn fn) {
    if (g_count < (int)(sizeof(g_tests) / sizeof(g_tests[0]))) {
        g_tests[g_count].name = name;
        g_tests[g_count].file = file;
        g_tests[g_count].fn = fn;
        g_count++;
    }
}

void test_fail(const char* file, i32 line, const char* expr) {
    if (g_cur_failed < 10) fprintf(stderr, "    %s:%d: CHECK(%s)\n", file, line, expr);
    g_cur_failed++;
}

void test_fail_near(const char* file, i32 line, const char* expr, f64 a, f64 b, f64 eps) {
    if (g_cur_failed < 10)
        fprintf(stderr, "    %s:%d: CHECK_NEAR(%s): %.9g vs %.9g (eps %.3g)\n",
                file, line, expr, a, b, eps);
    g_cur_failed++;
}

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : NULL;
    int passed = 0, failed = 0;
    for (int i = 0; i < g_count; i++) {
        if (filter && !strstr(g_tests[i].name, filter)) continue;
        g_cur_failed = 0;
        g_tests[i].fn();
        if (g_cur_failed) {
            fprintf(stderr, "KALDI  %s (%s)\n", g_tests[i].name, g_tests[i].file);
            failed++;
        } else {
            passed++;
        }
    }
    printf("%d geçti, %d kaldı\n", passed, failed);
    return failed ? 1 : 0;
}
