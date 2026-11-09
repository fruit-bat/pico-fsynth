#pragma once
//
// test.h - a minimal unit test harness for the host test suite.
//
// Each test program defines test functions and runs them from main():
//
//   TEST(crc_known_value) { CHECK_EQ(fs_crc32(0, "123456789", 9), 0xcbf43926u); }
//   int main(void) { RUN(crc_known_value); return test_summary(); }
//
// A failed CHECK prints the file, line and expression (with the values for
// CHECK_EQ / CHECK_NEAR) and the test carries on, so one run reports every
// failure. test_summary() prints a count and returns the exit status CTest
// expects: 0 when everything passed.
//
#include <stdio.h>
#include <stdint.h>
#include <math.h>

static int test_checks;
static int test_failures;
static int test_failed_tests;
static int test_run_count;

#define TEST(name) static void name(void)

// Run one test function and print ok / FAIL with its name
#define RUN(name) do {                                                      \
        const int before_ = test_failures;                                  \
        ++test_run_count;                                                   \
        name();                                                             \
        if (test_failures != before_) ++test_failed_tests;                  \
        printf("%s %s\n", test_failures == before_ ? "ok  " : "FAIL", #name); \
    } while (0)

#define CHECK(cond) do {                                                    \
        ++test_checks;                                                      \
        if (!(cond)) {                                                      \
            ++test_failures;                                                \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

// Integer equality; both sides are printed on failure
#define CHECK_EQ(a, b) do {                                                 \
        ++test_checks;                                                      \
        const long long a_ = (long long)(a), b_ = (long long)(b);           \
        if (a_ != b_) {                                                     \
            ++test_failures;                                                \
            fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: %lld != %lld\n", \
                    __FILE__, __LINE__, #a, #b, a_, b_);                    \
        }                                                                   \
    } while (0)

// |a - b| <= tol
#define CHECK_NEAR(a, b, tol) do {                                          \
        ++test_checks;                                                      \
        const double a_ = (double)(a), b_ = (double)(b), t_ = (double)(tol); \
        if (!(fabs(a_ - b_) <= t_)) {                                       \
            ++test_failures;                                                \
            fprintf(stderr, "%s:%d: CHECK_NEAR(%s, %s, %s) failed: %g vs %g\n", \
                    __FILE__, __LINE__, #a, #b, #tol, a_, b_);              \
        }                                                                   \
    } while (0)

// Print the totals; returns the process exit status (0 = all passed)
static inline int test_summary(void) {
    printf("%d tests, %d checks, %d failed checks in %d tests\n",
           test_run_count, test_checks, test_failures, test_failed_tests);
    return test_failures ? 1 : 0;
}
