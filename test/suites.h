/*
 * Every suite in this directory, declared once so that run_tests.c does not
 * need an extern declaration of its own.
 *
 * A suite takes void and returns void. It opens with check_suite() and then
 * runs its checks. A new suite needs only a file, a line here and a line in
 * run_tests.c.
 */
#ifndef SUITES_H
#define SUITES_H

void suite_thrift(void);
void suite_rle(void);
void suite_parquet(void);

#endif /* SUITES_H */
