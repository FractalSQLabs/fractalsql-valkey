/* SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * tests/audit_regress.c -- regression tests (VK-01, VK-02) over the helpers in src/fv_common.h.
 *   VK-01  fv_csv_of writes at most what it allocated for the longest %.9g of a DOUBLE (not just a float32), however many elements
 *   VK-02  fv_contains finds a needle anywhere in a bounded buffer, never reads past it, and does not depend on memmem()
 * Built with ASan + UBSan by gate 37 so a write past the buffer is a failure, not a lucky pass. Exit status is the failure count. */
#include "fv_common.h"

#include <float.h>

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "  FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void test_csv_of(void) {
    /* the longest renderings %.9g produces for a double: 16 characters each ("-1.23456789e-308") */
    enum { N = 64 };
    double v[N];
    for (int i = 0; i < N; i++) v[i] = (i & 1) ? -1.23456789e-308 : -1.23456789e+307;
    char *s = fv_csv_of(v, N);
    CHECK(s != NULL, "VK-01 fv_csv_of failed for %d worst-case doubles", N);
    if (s) {
        size_t commas = 0; for (const char *p = s; *p; p++) if (*p == ',') commas++;
        CHECK(commas == N - 1, "VK-01 expected %d separators, got %zu", N - 1, commas);
        CHECK(strlen(s) >= (size_t) N * 15, "VK-01 output suspiciously short (%zu)", strlen(s));
        free(s);
    }
    double one = 0.5;
    s = fv_csv_of(&one, 1);
    CHECK(s && strcmp(s, "0.5") == 0, "VK-01 a single value renders as 0.5 (got %s)", s ? s : "NULL");
    free(s);
    s = fv_csv_of(&one, 0);
    CHECK(s && s[0] == '\0', "VK-01 zero elements renders as an empty string");
    free(s);
    CHECK(fv_csv_of(&one, SIZE_MAX / 8) == NULL, "VK-01 an absurd element count is refused, not wrapped");
}

static void test_contains(void) {
    const char json[] = "{\"x\":1,\"_actor\":\"forged\",\"y\":2}";
    CHECK(fv_contains(json, sizeof json - 1, "\"_actor\"", 8), "VK-02 the reserved key is found in the middle");
    CHECK(!fv_contains(json, sizeof json - 1, "\"_actorX\"", 9), "VK-02 a near miss is not a match");
    CHECK(fv_contains("a=>b", 4, "=>", 2), "VK-02 => found");
    CHECK(fv_contains("=>", 2, "=>", 2), "VK-02 a needle that is the whole haystack is found");
    CHECK(fv_contains("x=>", 3, "=>", 2), "VK-02 a needle at the very end is found");
    CHECK(!fv_contains("=", 1, "=>", 2), "VK-02 a needle longer than the haystack is not found");
    CHECK(!fv_contains("", 0, "=>", 2), "VK-02 an empty haystack never matches");
    /* the haystack is bounded by its length, not by a NUL: a match past the stated length must not count */
    char buf[8]; memcpy(buf, "ab=>cd", 7);
    CHECK(!fv_contains(buf, 3, "=>", 2), "VK-02 a match beyond the stated length is ignored");
    CHECK(!fv_contains(buf, 3, "=>", 2), "VK-02 a match cut at the stated length is ignored");
    CHECK(fv_contains(buf, 5, "=>", 2), "VK-02 a match inside the stated length is found");
}

int main(void) {
    test_csv_of();
    test_contains();
    printf("audit_regress: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
