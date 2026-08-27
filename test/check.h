/*
 * The assertion harness that every suite in this directory uses.
 *
 * The harness is one C99 file, and it only depends on the standard library.
 * A framework is more than a suite of this size needs, and it adds one more
 * thing to build on a machine that otherwise only builds this library.
 *
 * The harness never calls exit(), because a call to exit() hides the control
 * flow of the program. A harness that exits on the first failure also reports
 * one fault when the run holds five. Every check records its result and the
 * run reaches its end, so one build reports every fault.
 *
 * Only the macros carry the file and the line that locate a failure, so a
 * suite calls the macros and never the functions below them.
 *
 *     CHECK_TRUE(thrift_ok(&reader), "the walk reached the stop byte");
 *     CHECK_EQUAL(count, 3U, "the high nibble is the count");
 *     CHECK_EQUAL_TEXT(p_back, p_original, "the round trip returns the text");
 *
 * A suite takes void and returns void. It opens with check_suite() and then
 * runs its checks. The runner, run_tests.c, calls each suite and returns
 * check_report().
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Sets the suite that owns the checks after it.
 *
 * The name is printed once, and again beside each failure, so a failure
 * identifies its suite without a search through the earlier output.
 */
void check_suite(const char *p_name);

/* Records one condition. CHECK_TRUE fills the last two, so it is preferred. */
void check_that(bool b_ok, const char *p_what, const char *p_file,
                uint32_t line);

/*
 * Records one integer comparison, and prints both values when they differ.
 *
 * The parameters are uint64_t, because every unsigned integer that this
 * repository uses converts to it without loss. One comparator therefore
 * serves size_t and uint32_t alike.
 */
void check_equal_u64(uint64_t got, uint64_t want, const char *p_what,
                     const char *p_file, uint32_t line);

/*
 * Records one byte comparison, and prints the offset of the first difference.
 *
 * The offset is the useful part. When two streams of a million bytes differ,
 * the offset is a position that a reader can seek to. A plain "they differ"
 * does not shorten the search.
 */
void check_equal_bytes(const uint8_t *p_got, size_t got_len,
                       const uint8_t *p_want, size_t want_len,
                       const char *p_what, const char *p_file, uint32_t line);

/* The same comparison over two null terminated strings. */
void check_equal_text(const char *p_got, const char *p_want,
                      const char *p_what, const char *p_file, uint32_t line);

/*
 * Prints the tally and returns the exit code of the run.
 *
 * The code is zero when every check passed and one when any check failed. A
 * shell truncates an exit code to eight bits, so an exit code equal to the
 * failure count would report success after 256 failures.
 */
int check_report(void);

#define CHECK_TRUE(cond, what) \
    check_that((cond), (what), __FILE__, (uint32_t)__LINE__)

#define CHECK_FALSE(cond, what) \
    check_that(!(cond), (what), __FILE__, (uint32_t)__LINE__)

#define CHECK_EQUAL(got, want, what) \
    check_equal_u64((uint64_t)(got), (uint64_t)(want), (what), __FILE__, \
                    (uint32_t)__LINE__)

#define CHECK_EQUAL_BYTES(got, got_len, want, want_len, what) \
    check_equal_bytes((got), (got_len), (want), (want_len), (what), \
                      __FILE__, (uint32_t)__LINE__)

#define CHECK_EQUAL_TEXT(got, want, what) \
    check_equal_text((got), (want), (what), __FILE__, (uint32_t)__LINE__)

#endif /* CHECK_H */
