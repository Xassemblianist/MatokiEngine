/* tests/test.h — mini test çatısı (sadece native host derlemesi).
 * Kullanım:  TEST(isim) { CHECK(x == 1); CHECK_NEAR(a, b, 1e-5); }
 * Testler constructor ile otomatik kaydolur; koşucu: tools/host/test_main.c
 */
#ifndef TESTS_TEST_H
#define TESTS_TEST_H

#include "core/types.h"

typedef void (*TestFn)(void);

void test_register(const char* name, const char* file, TestFn fn);
void test_fail(const char* file, i32 line, const char* expr);
void test_fail_near(const char* file, i32 line, const char* expr, f64 a, f64 b, f64 eps);

#define TEST(name)                                                        \
    static void test_fn_##name(void);                                     \
    __attribute__((constructor)) static void test_reg_##name(void) {      \
        test_register(#name, __FILE__, test_fn_##name);                   \
    }                                                                     \
    static void test_fn_##name(void)

#define CHECK(c) \
    do { if (!(c)) test_fail(__FILE__, __LINE__, #c); } while (0)

#define CHECK_NEAR(a, b, eps)                                             \
    do {                                                                  \
        f64 t_a_ = (f64)(a), t_b_ = (f64)(b), t_e_ = (f64)(eps);          \
        f64 t_d_ = t_a_ - t_b_; if (t_d_ < 0) t_d_ = -t_d_;               \
        if (!(t_d_ <= t_e_))                                              \
            test_fail_near(__FILE__, __LINE__, #a " ~ " #b, t_a_, t_b_, t_e_); \
    } while (0)

#endif
