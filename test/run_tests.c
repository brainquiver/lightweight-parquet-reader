/*
 * The test runner. It calls every suite and returns the exit code of the run.
 *
 * The make test target builds and runs this file, so a non zero return fails
 * the build. The runner only calls the suites. The tally is in check.c, and
 * check_report() converts it to the exit code.
 */
#include "check.h"
#include "suites.h"

int main(void)
{
    suite_thrift();
    suite_rle();
    suite_parquet();

    return check_report();
}
