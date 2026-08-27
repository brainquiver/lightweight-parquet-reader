/*
 * The assertion harness. The header, check.h, gives its purpose and the reason
 * for its small size.
 */
#include "check.h"

#include <stdio.h>
#include <string.h>

/* The tally of the whole run. One process runs one set of suites, so the
 * tally is a global, and the checks do not carry a handle to it. */
typedef struct
{
    const char *p_suite;
    uint32_t    suites;
    uint32_t    checks;
    uint32_t    failures;
} tally_t;

static tally_t g_tally = { "none", 0U, 0U, 0U };

/* The head of a failure line, so every report opens the same way. */
static void report_head(const char *p_what, const char *p_file, uint32_t line)
{
    (void)printf("  FAIL  %s:%u  [%s]\n        %s\n", p_file,
                 (unsigned)line, g_tally.p_suite, p_what);

    return;
}

void check_suite(const char *p_name)
{
    g_tally.p_suite = p_name;
    g_tally.suites += 1U;
    (void)printf("%s\n", p_name);

    return;
}

void check_that(bool b_ok, const char *p_what, const char *p_file,
                uint32_t line)
{
    g_tally.checks += 1U;

    if (!b_ok)
    {
        g_tally.failures += 1U;
        report_head(p_what, p_file, line);
    }

    return;
}

void check_equal_u64(uint64_t got, uint64_t want, const char *p_what,
                     const char *p_file, uint32_t line)
{
    g_tally.checks += 1U;

    if (want != got)
    {
        g_tally.failures += 1U;
        report_head(p_what, p_file, line);
        (void)printf("        want %llu, got %llu\n",
                     (unsigned long long)want, (unsigned long long)got);
    }

    return;
}

/*
 * The offset of the first byte that differs, or the shorter length when one
 * is a prefix of the other. The caller already knows that they differ.
 */
static size_t first_difference(const uint8_t *p_got, size_t got_len,
                               const uint8_t *p_want, size_t want_len)
{
    size_t shortest = (got_len < want_len) ? got_len : want_len;
    size_t at = shortest;
    size_t i = 0U;

    for (i = 0U; (i < shortest) && (at == shortest); i++)
    {
        if (p_got[i] != p_want[i])
        {
            at = i;
        }
    }

    return at;
}

/* The largest number of context bytes printed at a difference. */
#define WINDOW_BYTES 24U

/* One side of a difference, in a readable form. A byte outside printable
 * ASCII becomes a dot, so a terminal never receives a control byte. */
static void print_window(const char *p_label, const uint8_t *p_bytes,
                         size_t len, size_t at)
{
    size_t i = 0U;
    size_t end = ((at + WINDOW_BYTES) < len) ? (at + WINDOW_BYTES) : len;

    (void)printf("        %s ", p_label);

    for (i = at; i < end; i++)
    {
        /* 0x20 to 0x7E is printable ASCII. Every other byte prints as a
         * dot, and this includes each UTF-8 continuation byte. */
        (void)printf("%c", ((0x20U <= p_bytes[i]) && (0x7EU >= p_bytes[i]))
                           ? (char)p_bytes[i] : '.');
    }

    (void)printf("\n");

    return;
}

void check_equal_bytes(const uint8_t *p_got, size_t got_len,
                       const uint8_t *p_want, size_t want_len,
                       const char *p_what, const char *p_file, uint32_t line)
{
    bool b_same = (want_len == got_len);

    g_tally.checks += 1U;

    if (b_same)
    {
        b_same = (0 == memcmp(p_got, p_want, got_len));
    }

    if (!b_same)
    {
        size_t at = first_difference(p_got, got_len, p_want, want_len);

        g_tally.failures += 1U;
        report_head(p_what, p_file, line);
        (void)printf("        %zu bytes wanted, %zu given, first difference "
                     "at %zu\n", want_len, got_len, at);
        print_window("want", p_want, want_len, at);
        print_window("got ", p_got, got_len, at);
    }

    return;
}

void check_equal_text(const char *p_got, const char *p_want,
                      const char *p_what, const char *p_file, uint32_t line)
{
    check_equal_bytes((const uint8_t *)p_got, strlen(p_got),
                      (const uint8_t *)p_want, strlen(p_want), p_what,
                      p_file, line);

    return;
}

int check_report(void)
{
    (void)printf("\n%u suites, %u checks, %u failures\n", g_tally.suites,
                 g_tally.checks, g_tally.failures);

    return (0U == g_tally.failures) ? 0 : 1;
}
