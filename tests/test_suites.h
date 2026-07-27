/* test_suites.h -- registry of portable test suites.
 *
 * Suites are listed explicitly rather than auto-discovered so that the run
 * order is fixed and a test run is itself deterministic.
 */
#ifndef TG_TEST_SUITES_H
#define TG_TEST_SUITES_H

void test_suite_mem(void);
void test_suite_math(void);
void test_suite_rng(void);
void test_suite_hash(void);
void test_suite_mesh(void);

#endif /* TG_TEST_SUITES_H */
