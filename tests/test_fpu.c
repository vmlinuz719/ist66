#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "fpu.h"

/*
 * Helper: create an RDC-700 f36 value from an integer.
 * Uses the same to_float exponent bias as the commented-out test in fpu.c.
 * to_float = 0x4C8000000 encodes exponent field 0x99 (153), which in
 * excess-127 means 2^26, so the integer n is represented as n * 2^26
 * in the 27-bit significand — i.e. the leading bit of the significand
 * is at bit 26 for values < 2^1.
 */
static void make_f36_int(uint64_t n, rdc700_float_t *out) {
    uint64_t to_float = 0x4C8000000;
    uint64_t word = n | to_float;
    set_f36(&word, out);
    rdc700_fnorm(out, out);
}

/* ---- Classification tests ---- */

static void test_is_zero(void) {
    rdc700_float_t z = { .sign_exp = 0, .signif = 0 };
    CU_ASSERT_TRUE(is_zero(&z));
    CU_ASSERT_FALSE(is_nan(&z));
    CU_ASSERT_FALSE(is_inf(&z));
}

static void test_is_nan_true_nan(void) {
    /* True NaN: sign=1, exp=0 */
    rdc700_float_t n = { .sign_exp = 0x8000, .signif = 0 };
    CU_ASSERT_TRUE(is_nan(&n));
    CU_ASSERT_FALSE(is_zero(&n));
    CU_ASSERT_FALSE(is_inf(&n));
}

static void test_is_nan_pseudo_nan(void) {
    /* Pseudo NaN: sign_exp=0 but signif nonzero */
    rdc700_float_t n = { .sign_exp = 0, .signif = 42 };
    CU_ASSERT_TRUE(is_nan(&n));
}

static void test_is_inf(void) {
    /* Positive infinity: exp = 0x7FFF */
    rdc700_float_t pinf = { .sign_exp = 0x7FFF, .signif = 0 };
    CU_ASSERT_TRUE(is_inf(&pinf));
    CU_ASSERT_FALSE(is_nan(&pinf));
    CU_ASSERT_FALSE(is_zero(&pinf));

    /* Negative infinity */
    rdc700_float_t ninf = { .sign_exp = 0xFFFF, .signif = 0 };
    CU_ASSERT_TRUE(is_inf(&ninf));
}

/* ---- Format conversion round-trips ---- */

static void test_f36_round_trip(void) {
    rdc700_float_t f;
    uint64_t word_out;

    make_f36_int(132, &f);

    int rc = get_f36(&f, &word_out);
    CU_ASSERT_EQUAL(rc, 0);

    /* Convert back and verify */
    rdc700_float_t f2;
    set_f36(&word_out, &f2);
    rdc700_fnorm(&f2, &f2);

    CU_ASSERT_EQUAL(f.sign_exp, f2.sign_exp);
    /* Allow small rounding difference in low bits */
    CU_ASSERT_TRUE((f.signif >> 37) == (f2.signif >> 37));
}

static void test_f72_round_trip(void) {
    rdc700_float_t f;
    uint64_t hi, lo;

    make_f36_int(202, &f);

    int rc = get_f72(&f, &hi, &lo);
    CU_ASSERT_EQUAL(rc, 0);

    rdc700_float_t f2;
    set_f72(&hi, &lo, &f2);
    rdc700_fnorm(&f2, &f2);

    CU_ASSERT_EQUAL(f.sign_exp, f2.sign_exp);
    CU_ASSERT_EQUAL(f.signif, f2.signif);
}

static void test_f36_zero_round_trip(void) {
    uint64_t zero = 0;
    rdc700_float_t f;
    set_f36(&zero, &f);
    CU_ASSERT_TRUE(is_zero(&f));

    uint64_t word_out;
    get_f36(&f, &word_out);
    CU_ASSERT_EQUAL(word_out, 0);
}

/* ---- Normalization ---- */

static void test_fnorm_already_normalized(void) {
    rdc700_float_t f = { .sign_exp = 16383 + 5, .signif = 1UL << 63 };
    rdc700_float_t dst;
    rdc700_fnorm(&f, &dst);
    CU_ASSERT_EQUAL(dst.sign_exp, f.sign_exp);
    CU_ASSERT_EQUAL(dst.signif, f.signif);
}

static void test_fnorm_denormalized(void) {
    /* Significand with leading one at bit 61 instead of 63 — needs 2 shifts */
    rdc700_float_t f = { .sign_exp = 16383 + 10, .signif = 1UL << 61 };
    rdc700_float_t dst;
    rdc700_fnorm(&f, &dst);
    CU_ASSERT_EQUAL(dst.signif, 1UL << 63);
    CU_ASSERT_EQUAL(dst.sign_exp & 0x7FFF, 16383 + 10 - 2);
}

static void test_fnorm_zero(void) {
    rdc700_float_t f = { .sign_exp = 16383 + 5, .signif = 0 };
    rdc700_float_t dst;
    rdc700_fnorm(&f, &dst);
    CU_ASSERT_TRUE(is_zero(&dst));
}

static void test_fnorm_nan(void) {
    rdc700_float_t f = { .sign_exp = 0x8000, .signif = 0 };
    rdc700_float_t dst;
    rdc700_fnorm(&f, &dst);
    CU_ASSERT_TRUE(is_nan(&dst));
}

static void test_fnorm_inf(void) {
    rdc700_float_t f = { .sign_exp = 0x7FFF, .signif = 0 };
    rdc700_float_t dst;
    rdc700_fnorm(&f, &dst);
    CU_ASSERT_TRUE(is_inf(&dst));
}

/* ---- Negation ---- */

static void test_fneg_positive(void) {
    rdc700_float_t f;
    make_f36_int(42, &f);
    CU_ASSERT_EQUAL(f.sign_exp & 0x8000, 0); /* positive */

    rdc700_float_t neg;
    rdc700_fneg(&f, &neg);
    CU_ASSERT_NOT_EQUAL(neg.sign_exp & 0x8000, 0); /* now negative */
    CU_ASSERT_EQUAL(neg.signif, f.signif);
}

static void test_fneg_double_negation(void) {
    rdc700_float_t f;
    make_f36_int(42, &f);

    rdc700_float_t neg, pos;
    rdc700_fneg(&f, &neg);
    rdc700_fneg(&neg, &pos);
    CU_ASSERT_EQUAL(pos.sign_exp, f.sign_exp);
    CU_ASSERT_EQUAL(pos.signif, f.signif);
}

static void test_fneg_zero(void) {
    rdc700_float_t z = { .sign_exp = 0, .signif = 0 };
    rdc700_float_t dst;
    rdc700_fneg(&z, &dst);
    CU_ASSERT_TRUE(is_zero(&dst));
}

static void test_fneg_nan(void) {
    rdc700_float_t n = { .sign_exp = 0x8000, .signif = 0 };
    rdc700_float_t dst;
    rdc700_fneg(&n, &dst);
    CU_ASSERT_TRUE(is_nan(&dst));
}

/* ---- Rounding ---- */

static void test_round_to_f36_overflow(void) {
    /* Exponent too large for f36 range */
    rdc700_float_t src = { .sign_exp = 16383 + 200, .signif = 1UL << 63 };
    rdc700_float_t dst;
    int rc = f80_round_to_f36(&src, &dst);
    CU_ASSERT_EQUAL(rc, F_OVRF);
}

static void test_round_to_f36_underflow(void) {
    /* Exponent too small for f36 range */
    rdc700_float_t src = { .sign_exp = 1, .signif = 1UL << 63 };
    rdc700_float_t dst;
    int rc = f80_round_to_f36(&src, &dst);
    CU_ASSERT_EQUAL(rc, F_UNDF);
}

static void test_round_to_f72_overflow(void) {
    rdc700_float_t src = { .sign_exp = 16383 + 200, .signif = 1UL << 63 };
    rdc700_float_t dst;
    int rc = f80_round_to_f72(&src, &dst);
    CU_ASSERT_EQUAL(rc, F_OVRF);
}

static void test_round_to_f72_underflow(void) {
    rdc700_float_t src = { .sign_exp = 1, .signif = 1UL << 63 };
    rdc700_float_t dst;
    int rc = f80_round_to_f72(&src, &dst);
    CU_ASSERT_EQUAL(rc, F_UNDF);
}

/* ---- Co-normalization ---- */

static void test_fconorm_equal_exponents(void) {
    rdc700_float_t a = { .sign_exp = 16383 + 5, .signif = 1UL << 63 };
    rdc700_float_t b = { .sign_exp = 16383 + 5, .signif = 1UL << 62 };
    rdc700_float_t g, l;
    int rc = rdc700_fconorm(&a, &b, &g, &l);
    CU_ASSERT_EQUAL(rc, 0);
    /* When exponents are equal, values pass through unchanged */
    CU_ASSERT_EQUAL(g.sign_exp, a.sign_exp);
    CU_ASSERT_EQUAL(g.signif, a.signif);
    CU_ASSERT_EQUAL(l.sign_exp, b.sign_exp);
    CU_ASSERT_EQUAL(l.signif, b.signif);
}

static void test_fconorm_different_exponents(void) {
    rdc700_float_t a = { .sign_exp = 16383 + 10, .signif = 1UL << 63 };
    rdc700_float_t b = { .sign_exp = 16383 + 8, .signif = 1UL << 63 };
    rdc700_float_t g, l;
    int rc = rdc700_fconorm(&a, &b, &g, &l);
    CU_ASSERT_EQUAL(rc, 0);
    /* Greater should have the larger exponent */
    CU_ASSERT_EQUAL(g.sign_exp & 0x7FFF, 16383 + 10);
    /* Lesser should be shifted to match */
    CU_ASSERT_EQUAL(l.sign_exp & 0x7FFF, 16383 + 10);
    CU_ASSERT_TRUE(l.signif < g.signif);
}

static void test_fconorm_insignificant(void) {
    rdc700_float_t a = { .sign_exp = 16383 + 100, .signif = 1UL << 63 };
    rdc700_float_t b = { .sign_exp = 16383 + 1, .signif = 1UL << 63 };
    rdc700_float_t g, l;
    int rc = rdc700_fconorm(&a, &b, &g, &l);
    CU_ASSERT_EQUAL(rc, F_INSG);
}

/* ---- Arithmetic (based on commented-out test in fpu.c) ---- */

/*
 * Test: 132/100 + 7/10 should equal 202/100
 * This replicates the commented-out main() in fpu.c.
 */
static void test_div_and_add(void) {
    rdc700_float_t src, tgt, result_a, result_b, result_c;

    /* 132 / 100 */
    make_f36_int(132, &src);
    make_f36_int(100, &tgt);
    rdc700_fdiv(&src, &tgt, &result_a);
    rdc700_fnorm(&result_a, &result_a);

    /* 7 / 10 */
    make_f36_int(7, &src);
    make_f36_int(10, &tgt);
    rdc700_fdiv(&src, &tgt, &result_b);
    rdc700_fnorm(&result_b, &result_b);

    /* 132/100 + 7/10 */
    rdc700_fadd(&result_a, &result_b, &result_c);

    /* Expected: 202/100 */
    make_f36_int(202, &src);
    make_f36_int(100, &tgt);
    rdc700_float_t expected;
    rdc700_fdiv(&src, &tgt, &expected);

    /* Compare exponent */
    int exp_got = ((int)(result_c.sign_exp & 0x7FFF)) - 16383;
    int exp_exp = ((int)(expected.sign_exp & 0x7FFF)) - 16383;
    CU_ASSERT_EQUAL(exp_got, exp_exp);

    /* Compare significands — allow tiny rounding difference */
    int64_t diff = (int64_t)(result_c.signif - expected.signif);
    if (diff < 0) diff = -diff;
    CU_ASSERT_TRUE(diff < 4);
}

static void test_fadd_zero_identity(void) {
    rdc700_float_t a;
    make_f36_int(42, &a);
    rdc700_float_t z = { .sign_exp = 0, .signif = 0 };
    rdc700_float_t dst;

    rdc700_fadd(&a, &z, &dst);
    CU_ASSERT_EQUAL(dst.sign_exp, a.sign_exp);
    CU_ASSERT_EQUAL(dst.signif, a.signif);

    rdc700_fadd(&z, &a, &dst);
    CU_ASSERT_EQUAL(dst.sign_exp, a.sign_exp);
    CU_ASSERT_EQUAL(dst.signif, a.signif);
}

static void test_fadd_nan_propagates(void) {
    rdc700_float_t n = { .sign_exp = 0x8000, .signif = 0 };
    rdc700_float_t a;
    make_f36_int(5, &a);
    rdc700_float_t dst;

    int rc = rdc700_fadd(&n, &a, &dst);
    CU_ASSERT_EQUAL(rc, F_ILGL);
    CU_ASSERT_TRUE(is_nan(&dst));
}

static void test_fmul_basic(void) {
    rdc700_float_t a, b, dst;
    make_f36_int(6, &a);
    make_f36_int(7, &b);
    rdc700_fmul(&a, &b, &dst);

    /* Expected: 42 */
    rdc700_float_t expected;
    make_f36_int(42, &expected);

    int exp_got = ((int)(dst.sign_exp & 0x7FFF)) - 16383;
    int exp_exp = ((int)(expected.sign_exp & 0x7FFF)) - 16383;
    CU_ASSERT_EQUAL(exp_got, exp_exp);

    int64_t diff = (int64_t)(dst.signif - expected.signif);
    if (diff < 0) diff = -diff;
    CU_ASSERT_TRUE(diff < 4);
}

static void test_fmul_by_zero(void) {
    rdc700_float_t a;
    make_f36_int(42, &a);
    rdc700_float_t z = { .sign_exp = 0, .signif = 0 };
    rdc700_float_t dst;

    int rc = rdc700_fmul(&a, &z, &dst);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_TRUE(is_zero(&dst));
}

static void test_fmul_zero_times_inf(void) {
    rdc700_float_t z = { .sign_exp = 0, .signif = 0 };
    rdc700_float_t inf = { .sign_exp = 0x7FFF, .signif = 0 };
    rdc700_float_t dst;

    int rc = rdc700_fmul(&z, &inf, &dst);
    CU_ASSERT_EQUAL(rc, F_ILGL);
    CU_ASSERT_TRUE(is_nan(&dst));
}

static void test_fdiv_basic(void) {
    rdc700_float_t a, b, dst;
    make_f36_int(84, &a);
    make_f36_int(2, &b);
    rdc700_fdiv(&a, &b, &dst);

    rdc700_float_t expected;
    make_f36_int(42, &expected);

    int exp_got = ((int)(dst.sign_exp & 0x7FFF)) - 16383;
    int exp_exp = ((int)(expected.sign_exp & 0x7FFF)) - 16383;
    CU_ASSERT_EQUAL(exp_got, exp_exp);

    int64_t diff = (int64_t)(dst.signif - expected.signif);
    if (diff < 0) diff = -diff;
    CU_ASSERT_TRUE(diff < 4);
}

static void test_fdiv_by_zero(void) {
    rdc700_float_t a;
    make_f36_int(42, &a);
    rdc700_float_t z = { .sign_exp = 0, .signif = 0 };
    rdc700_float_t dst;

    int rc = rdc700_fdiv(&a, &z, &dst);
    CU_ASSERT_EQUAL(rc, F_ILGL);
    CU_ASSERT_TRUE(is_inf(&dst));
}

static void test_fdiv_nan_propagates(void) {
    rdc700_float_t n = { .sign_exp = 0x8000, .signif = 0 };
    rdc700_float_t a;
    make_f36_int(5, &a);
    rdc700_float_t dst;

    int rc = rdc700_fdiv(&n, &a, &dst);
    CU_ASSERT_EQUAL(rc, F_ILGL);
    CU_ASSERT_TRUE(is_nan(&dst));
}

/* ---- Test runner ---- */

int main(void) {
    if (CU_initialize_registry() != CUE_SUCCESS)
        return CU_get_error();

    CU_pSuite suite;

    /* Classification */
    suite = CU_add_suite("Classification", NULL, NULL);
    CU_add_test(suite, "is_zero", test_is_zero);
    CU_add_test(suite, "is_nan (true NaN)", test_is_nan_true_nan);
    CU_add_test(suite, "is_nan (pseudo NaN)", test_is_nan_pseudo_nan);
    CU_add_test(suite, "is_inf", test_is_inf);

    /* Format conversion */
    suite = CU_add_suite("Format Conversion", NULL, NULL);
    CU_add_test(suite, "f36 round-trip", test_f36_round_trip);
    CU_add_test(suite, "f72 round-trip", test_f72_round_trip);
    CU_add_test(suite, "f36 zero round-trip", test_f36_zero_round_trip);

    /* Normalization */
    suite = CU_add_suite("Normalization", NULL, NULL);
    CU_add_test(suite, "already normalized", test_fnorm_already_normalized);
    CU_add_test(suite, "denormalized", test_fnorm_denormalized);
    CU_add_test(suite, "zero", test_fnorm_zero);
    CU_add_test(suite, "NaN", test_fnorm_nan);
    CU_add_test(suite, "Inf", test_fnorm_inf);

    /* Negation */
    suite = CU_add_suite("Negation", NULL, NULL);
    CU_add_test(suite, "positive to negative", test_fneg_positive);
    CU_add_test(suite, "double negation", test_fneg_double_negation);
    CU_add_test(suite, "negate zero", test_fneg_zero);
    CU_add_test(suite, "negate NaN", test_fneg_nan);

    /* Rounding */
    suite = CU_add_suite("Rounding", NULL, NULL);
    CU_add_test(suite, "f36 overflow", test_round_to_f36_overflow);
    CU_add_test(suite, "f36 underflow", test_round_to_f36_underflow);
    CU_add_test(suite, "f72 overflow", test_round_to_f72_overflow);
    CU_add_test(suite, "f72 underflow", test_round_to_f72_underflow);

    /* Co-normalization */
    suite = CU_add_suite("Co-normalization", NULL, NULL);
    CU_add_test(suite, "equal exponents", test_fconorm_equal_exponents);
    CU_add_test(suite, "different exponents", test_fconorm_different_exponents);
    CU_add_test(suite, "insignificant", test_fconorm_insignificant);

    /* Arithmetic */
    suite = CU_add_suite("Arithmetic", NULL, NULL);
    CU_add_test(suite, "132/100 + 7/10 = 202/100", test_div_and_add);
    CU_add_test(suite, "add zero identity", test_fadd_zero_identity);
    CU_add_test(suite, "add NaN propagates", test_fadd_nan_propagates);
    CU_add_test(suite, "6 * 7 = 42", test_fmul_basic);
    CU_add_test(suite, "multiply by zero", test_fmul_by_zero);
    CU_add_test(suite, "0 * inf = NaN", test_fmul_zero_times_inf);
    CU_add_test(suite, "84 / 2 = 42", test_fdiv_basic);
    CU_add_test(suite, "divide by zero", test_fdiv_by_zero);
    CU_add_test(suite, "divide NaN propagates", test_fdiv_nan_propagates);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    unsigned int failures = CU_get_number_of_failures();
    CU_cleanup_registry();

    return failures ? 1 : 0;
}
