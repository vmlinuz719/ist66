#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define BCD_MAX_DIGITS 36

typedef struct {
    uint8_t d[BCD_MAX_DIGITS];  // digits, most significant first
    int len;                     // number of significant digits (1..36)
} bcd_t;

// Strip leading zeros, keeping at least one digit
static void bcd_strip(bcd_t *a)
{
    int shift = 0;
    while (shift < a->len - 1 && a->d[shift] == 0)
        shift++;
    if (shift > 0) {
        a->len -= shift;
        memmove(a->d, a->d + shift, a->len);
    }
}

// Compare: returns -1, 0, or 1
static int bcd_compare(const bcd_t *a, const bcd_t *b)
{
    if (a->len != b->len)
        return a->len > b->len ? 1 : -1;
    for (int i = 0; i < a->len; i++) {
        if (a->d[i] != b->d[i])
            return a->d[i] > b->d[i] ? 1 : -1;
    }
    return 0;
}

// Subtract: result = a - b, assuming a >= b
static void bcd_subtract(uint8_t *result, const uint8_t *a, const uint8_t *b, int len)
{
    int borrow = 0;
    for (int i = len - 1; i >= 0; i--) {
        int diff = a[i] - b[i] - borrow;
        if (diff < 0) {
            diff += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        result[i] = (uint8_t)diff;
    }
}

// Multiply BCD number by single digit (0-9), store in result
static void bcd_mul1(uint8_t *result, const uint8_t *a, int len, uint8_t digit)
{
    int carry = 0;
    for (int i = len - 1; i >= 0; i--) {
        int prod = a[i] * digit + carry;
        result[i + 1] = (uint8_t)(prod % 10);
        carry = prod / 10;
    }
    result[0] = (uint8_t)carry;
}

// Addition: result = a + b
// Returns 0 on success, -1 if result exceeds BCD_MAX_DIGITS
int bcd_add(const bcd_t *a, const bcd_t *b, bcd_t *result)
{
    // Work right-to-left with carry, padding shorter operand with zeros
    int carry = 0;
    int rlen = (a->len > b->len ? a->len : b->len) + 1;  // +1 for possible carry out
    if (rlen > BCD_MAX_DIGITS + 1)
        return -1;

    uint8_t tmp[BCD_MAX_DIGITS + 1];
    memset(tmp, 0, sizeof(tmp));

    for (int i = 0; i < rlen - 1; i++) {
        int ai = a->len - 1 - i;
        int bi = b->len - 1 - i;
        int sum = carry;
        if (ai >= 0) sum += a->d[ai];
        if (bi >= 0) sum += b->d[bi];
        tmp[rlen - 1 - i] = (uint8_t)(sum % 10);
        carry = sum / 10;
    }
    tmp[0] = (uint8_t)carry;

    if (rlen > BCD_MAX_DIGITS) {
        // Overflow: result too large
        return -1;
    }

    memcpy(result->d, tmp, rlen);
    result->len = rlen;
    bcd_strip(result);
    return 0;
}

// Multiplication: result = a * b
// Returns 0 on success, -1 if result exceeds BCD_MAX_DIGITS
int bcd_multiply(const bcd_t *a, const bcd_t *b, bcd_t *result)
{
    int rlen = a->len + b->len;
    if (rlen > BCD_MAX_DIGITS)
        return -1;

    uint8_t tmp[BCD_MAX_DIGITS * 2];
    memset(tmp, 0, sizeof(tmp));

    // Standard long multiplication: for each digit of b (right to left),
    // multiply all of a and accumulate with offset
    for (int i = b->len - 1; i >= 0; i--) {
        int carry = 0;
        int offset = b->len - 1 - i;
        for (int j = a->len - 1; j >= 0; j--) {
            int pos = rlen - 1 - offset - (a->len - 1 - j);
            int prod = tmp[pos] + a->d[j] * b->d[i] + carry;
            tmp[pos] = (uint8_t)(prod % 10);
            carry = prod / 10;
        }
        // Propagate remaining carry
        int pos = rlen - 1 - offset - a->len;
        while (carry && pos >= 0) {
            int sum = tmp[pos] + carry;
            tmp[pos] = (uint8_t)(sum % 10);
            carry = sum / 10;
            pos--;
        }
    }

    memcpy(result->d, tmp, rlen);
    result->len = rlen;
    bcd_strip(result);
    return 0;
}

// Division: quotient = dividend / divisor, remainder = dividend % divisor
// Returns 0 on success, -1 on division by zero
int bcd_divide(const bcd_t *dividend, const bcd_t *divisor,
               bcd_t *quotient, bcd_t *remainder)
{
    int all_zero = 1;
    for (int i = 0; i < divisor->len; i++) {
        if (divisor->d[i] != 0) { all_zero = 0; break; }
    }
    if (all_zero)
        return -1;

    if (bcd_compare(dividend, divisor) < 0) {
        memset(quotient->d, 0, BCD_MAX_DIGITS);
        quotient->len = 1;
        memcpy(remainder->d, dividend->d, dividend->len);
        remainder->len = dividend->len;
        return 0;
    }

    int rlen = divisor->len + 1;
    uint8_t rem[BCD_MAX_DIGITS + 1];
    memset(rem, 0, sizeof(rem));

    uint8_t prod[BCD_MAX_DIGITS + 1];

    memset(quotient->d, 0, BCD_MAX_DIGITS);
    quotient->len = dividend->len;

    for (int i = 0; i < dividend->len; i++) {
        memmove(rem, rem + 1, rlen - 1);
        rem[rlen - 1] = dividend->d[i];

        uint8_t q = 0;
        for (int trial = 9; trial >= 1; trial--) {
            bcd_mul1(prod, divisor->d, divisor->len, (uint8_t)trial);
            int cmp = 0;
            for (int j = 0; j < rlen; j++) {
                if (rem[j] != prod[j]) {
                    cmp = rem[j] > prod[j] ? 1 : -1;
                    break;
                }
            }
            if (cmp >= 0) {
                q = (uint8_t)trial;
                break;
            }
        }

        if (q > 0) {
            bcd_mul1(prod, divisor->d, divisor->len, q);
            bcd_subtract(rem, rem, prod, rlen);
        }

        quotient->d[i] = q;
    }

    if (rlen <= BCD_MAX_DIGITS) {
        memcpy(remainder->d, rem, rlen);
        remainder->len = rlen;
    } else {
        memcpy(remainder->d, rem + (rlen - BCD_MAX_DIGITS), BCD_MAX_DIGITS);
        remainder->len = BCD_MAX_DIGITS;
    }

    bcd_strip(quotient);
    bcd_strip(remainder);
    return 0;
}

void bcd_print(const bcd_t *n)
{
    for (int i = 0; i < n->len; i++)
        putchar('0' + n->d[i]);
}

void bcd_from_str(bcd_t *n, const char *s)
{
    int slen = strlen(s);
    if (slen > BCD_MAX_DIGITS) slen = BCD_MAX_DIGITS;
    n->len = slen;
    for (int i = 0; i < slen; i++)
        n->d[i] = s[i] - '0';
}

void test_div(const char *a, const char *b)
{
    bcd_t dividend, divisor, quotient, remainder;
    bcd_from_str(&dividend, a);
    bcd_from_str(&divisor, b);

    printf("DIV: ");
    if (bcd_divide(&dividend, &divisor, &quotient, &remainder) == 0) {
        bcd_print(&dividend); printf(" / "); bcd_print(&divisor);
        printf(" = "); bcd_print(&quotient);
        printf(" R "); bcd_print(&remainder); printf("\n");
    } else {
        bcd_print(&dividend); printf(" / "); bcd_print(&divisor);
        printf(" = DIVISION BY ZERO\n");
    }
}

void test_add(const char *a, const char *b)
{
    bcd_t ba, bb, result;
    bcd_from_str(&ba, a);
    bcd_from_str(&bb, b);

    printf("ADD: ");
    if (bcd_add(&ba, &bb, &result) == 0) {
        bcd_print(&ba); printf(" + "); bcd_print(&bb);
        printf(" = "); bcd_print(&result); printf("\n");
    } else {
        printf("OVERFLOW\n");
    }
}

void test_mul(const char *a, const char *b)
{
    bcd_t ba, bb, result;
    bcd_from_str(&ba, a);
    bcd_from_str(&bb, b);

    printf("MUL: ");
    if (bcd_multiply(&ba, &bb, &result) == 0) {
        bcd_print(&ba); printf(" * "); bcd_print(&bb);
        printf(" = "); bcd_print(&result); printf("\n");
    } else {
        printf("OVERFLOW\n");
    }
}

int main(void)
{
    printf("--- Division ---\n");
    test_div("123456789012345678901234567890", "9876543210");
    test_div("1000000", "125");
    test_div("42", "999");
    test_div("999999999", "7");
    test_div("12345", "12345");
    test_div("9876543210", "1");
    test_div("999999999999999999999999999999999999", "3");
    test_div("999999999999999999999999999999999999", "999999999999999999");
    test_div("12345", "0");
    test_div("0", "12345");

    printf("\n--- Addition ---\n");
    test_add("123456789", "987654321");           // sum with carry across all digits
    test_add("999999999999999999", "1");           // carry propagation
    test_add("0", "0");
    test_add("123456789012345678", "987654321098765432"); // 18+18 -> 18 digits
    test_add("999999999999999999999999999999999999",      // max + 1 = overflow
             "1");

    printf("\n--- Multiplication ---\n");
    test_mul("12345", "67890");
    test_mul("999999999", "999999999");
    test_mul("123456789", "1");
    test_mul("0", "9999999");
    test_mul("123456789012345678", "2");
    test_mul("999999999999999999", "999999999999999999"); // 18*18 = 36 digits

    return 0;
}
