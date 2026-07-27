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
void test_suite_spatial(void);
void test_suite_camera(void);
void test_suite_tree_profile(void);
void test_suite_tree_graph(void);
void test_suite_tree_growth(void);
void test_suite_tree_mechanics(void);
void test_suite_tree_foliage(void);
void test_suite_tree_bark(void);

#endif /* TG_TEST_SUITES_H */
