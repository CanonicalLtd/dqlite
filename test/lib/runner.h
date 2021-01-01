/* Convenience macros to reduce munit boiler plate. */

#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include "munit.h"

/* Top-level suites array declaration.
 *
 * These top-level suites hold all module-level child suites and must be defined
 * and then set as child suites of a root suite created at runtime by the test
 * runner's main(). This can be done using the RUNNER macro. */
extern MunitSuite _mainSuites[];
extern int _mainSuitesN;

/* Maximum number of test cases for each suite */
#define SUITE_CAP 128
#define TEST_CAP SUITE_CAP

/* Define the top-level suites array and the main() function of the test. */
#define RUNNER(NAME)                                                       \
	MunitSuite _mainSuites[SUITE_CAP];                                 \
	int _mainSuitesN = 0;                                              \
                                                                           \
	int main(int argc, char *argv[MUNIT_ARRAY_PARAM(argc + 1)])        \
	{                                                                  \
		MunitSuite suite = {(char *)"", NULL, _mainSuites, 1, 0};  \
		return munit_suite_main(&suite, (void *)NAME, argc, argv); \
	}

/* Declare and register a new test suite #S belonging to the file's test module.
 *
 * A test suite is a pair of static variables:
 *
 * static MunitTest _##S##Suites[SUITE_CAP]
 * static MunitTest _##S##Tests[SUITE_CAP]
 *
 * The tests and suites attributes of the next available MunitSuite slot in the
 * _moduleSuites array will be set to the suite's tests and suites arrays, and
 * the prefix attribute of the slot will be set to /S. */
#define SUITE(S)         \
	SUITE_DECLARE(S) \
	SUITE_ADD_CHILD(main, #S, S)

/* Declare and register a new test. */
#define TEST(S, C, SETUP, TEAR_DOWN, OPTIONS, PARAMS)                    \
	static MunitResult test_##S##_##C(const MunitParameter params[], \
					  void *data);                   \
	TEST_ADD_TO_SUITE(S, C, SETUP, TEAR_DOWN, OPTIONS, PARAMS)       \
	static MunitResult test_##S##_##C(                               \
	    MUNIT_UNUSED const MunitParameter params[],                  \
	    MUNIT_UNUSED void *data)

#define SKIP_IF_NO_FIXTURE         \
	if (f == NULL) {           \
		return MUNIT_SKIP; \
	}

/* Declare the MunitSuite[] and the MunitTest[] arrays that compose the test
 * suite identified by S. */
#define SUITE_DECLARE(S)                                           \
	static MunitSuite _##S##Suites[SUITE_CAP];                 \
	static MunitTest _##S##Tests[SUITE_CAP];                   \
	static MunitTestSetup _##S##Setup = NULL;                  \
	static MunitTestTearDown _##S##TearDown = NULL;            \
	static int _##S##SuitesN = 0;                              \
	static int _##S##TestsN = 0;                               \
	__attribute__((constructor)) static void _##S##_init(void) \
	{                                                          \
		memset(_##S##Suites, 0, sizeof(_##S##Suites));     \
		memset(_##S##Tests, 0, sizeof(_##S##Tests));       \
		(void)_##S##SuitesN;                               \
		(void)_##S##TestsN;                                \
		(void)_##S##Setup;                                 \
		(void)_##S##TearDown;                              \
	}

/* Set the tests and suites attributes of the next available slot of the
 * MunitSuite[] array of S1 to the MunitTest[] and MunitSuite[] arrays of S2,
 * using the given PREXIX. */
#define SUITE_ADD_CHILD(S1, PREFIX, S2)                                    \
	__attribute__((constructor)) static void _##S1##_##S2##_init(void) \
	{                                                                  \
		int n = _##S1##SuitesN;                                    \
		_##S1##Suites[n].prefix = PREFIX;                          \
		_##S1##Suites[n].tests = _##S2##Tests;                     \
		_##S1##Suites[n].suites = _##S2##Suites;                   \
		_##S1##Suites[n].iterations = 0;                           \
		_##S1##Suites[n].options = 0;                              \
		_##S1##SuitesN = n + 1;                                    \
	}

/* Add a test case to the MunitTest[] array of suite S. */
#define TEST_ADD_TO_SUITE(S, C, SETUP, TEAR_DOWN, OPTIONS, PARAMS)            \
	__attribute__((constructor)) static void _##S##Tests_##C##_init(void) \
	{                                                                     \
		MunitTest *tests = _##S##Tests;                               \
		int n = _##S##TestsN;                                         \
		TEST_SET_IN_ARRAY(tests, n, "/" #C, test_##S##_##C, SETUP,    \
				  TEAR_DOWN, OPTIONS, PARAMS);                \
		_##S##TestsN = n + 1;                                         \
	}

/* Set the values of the I'th test case slot in the given test array */
#define TEST_SET_IN_ARRAY(TESTS, I, NAME, FUNC, SETUP, TEAR_DOWN, OPTIONS, \
			  PARAMS)                                          \
	TESTS[I].name = NAME;                                              \
	TESTS[I].test = FUNC;                                              \
	TESTS[I].setup = SETUP;                                            \
	TESTS[I].tear_down = TEAR_DOWN;                                    \
	TESTS[I].options = OPTIONS;                                        \
	TESTS[I].parameters = PARAMS

/**
 * Declare and register a new test module #M.
 *
 * A test module is a test suite (i.e. a pair of MunitTest[] and MunitSuite[]
 * arrays), directly or indirectly containing all test cases in a test file.
 *
 * This macro uses hard-coded names to declare the module's tests and suites
 * arrays static, so they can be easly referenced in other static declarations
 * generated by the macros below:
 *
 * static MunitTest _moduleTests[TEST_CAP];
 * static MunitSuite _moduleSuites[TEST_CAP];
 *
 * The tests and suites attributes of the next available MunitSuite slot in the
 * top-level suites array will be set to the module's tests and suites arrays,
 * and the prefix attribute of the slot will be set to #M.
 *
 * Each test file should declare one and only one test module.
 */
#define TEST_MODULE(M)              \
	TEST_SUITE_DECLARE(module); \
	TEST_SUITE_ADD_CHILD(main, #M, module);

/**
 * Declare and register a new test suite #S belonging to the file's test module.
 *
 * A test suite is a pair of static variables:
 *
 * static MunitTest _##S##Suites[TEST_CAP]
 * static MunitTest _##S##Tests[TEST_CAP]
 *
 * The tests and suites attributes of the next available MunitSuite slot in the
 * #ModuleSuites array will be set to the suite's tests and suites arrays, and
 * the prefix attribute of the slot will be set to /S.
 *
 * All tests in the suite will use the same setup and tear down functions.
 */
#define TEST_SUITE(S)          \
	TEST_SUITE_DECLARE(S); \
	TEST_SUITE_ADD_CHILD(module, "/" #S, S);

/**
 * Declare a setup function.
 *
 * Possible signatures are:
 *
 * - TEST_SETUP(S): Declare the setup function for suite S inline.
 * - TEST_SETUP(S, F): Set the setup function for suite S to F.
 */
#define TEST_SETUP(...) TEST_SETUP_MACRO_CHOOSER(__VA_ARGS__)(__VA_ARGS__)
#define TEST_SETUP_(S)                                         \
	static void *S##Setup(const MunitParameter[], void *); \
	_##S##Setup = S##Setup;                                \
	static void *S##Setup(const MunitParameter params[], void *userData)

/**
 * Declare a tear down function.
 *
 * Possible signatures are:
 *
 * - TEST_SETUP(S): Declare the tear down function for suite S inline.
 * - TEST_SETUP(S, F): Set the tear down function for suite S to F.
 */
#define TEST_TEAR_DOWN(...) \
	TEST_TEAR_DOWN_MACRO_CHOOSER(__VA_ARGS__)(__VA_ARGS__)

/**
 * Declare and register a new group of tests #G, belonging to suite #S in the
 * file's test module.
 */
#define TEST_GROUP(C, T)                              \
	static MunitTest _##C##_##T##Tests[TEST_CAP]; \
	static int _##C##_##T##TestsN = 0;            \
	TEST_SUITE_ADD_GROUP(C, T);

/**
 * Declare and register a new test case.
 *
 * Possible signatures are:
 *
 * - TEST_CASE(C): C gets added to the tests array of the file module.
 * - TEST_CASE(S, C): C gets added to the tests array of suite S.
 * - TEST_CASE(S, G, C): C gets added to the tests array of group G in suite S.
 *
 * The test body declaration must follow the macro.
 */
#define TEST_CASE(...) TEST_CASE_MACRO_CHOOSER(__VA_ARGS__)(__VA_ARGS__)

/* Declare the MunitSuite[] and the MunitTest[] arrays that compose the test
 * suite identified by S. */
#define TEST_SUITE_DECLARE(S)                                      \
	static MunitSuite _##S##Suites[TEST_CAP];                  \
	static MunitTest _##S##Tests[TEST_CAP];                    \
	static MunitTestSetup _##S##Setup = NULL;                  \
	static MunitTestTearDown _##S##TearDown = NULL;            \
	static int _##S##SuitesN = 0;                              \
	static int _##S##TestsN = 0;                               \
	__attribute__((constructor)) static void _##S##_init(void) \
	{                                                          \
		memset(_##S##Suites, 0, sizeof(_##S##Suites));     \
		memset(_##S##Tests, 0, sizeof(_##S##Tests));       \
		(void)_##S##SuitesN;                               \
		(void)_##S##TestsN;                                \
		(void)_##S##Setup;                                 \
		(void)_##S##TearDown;                              \
	}

/* Set the tests and suites attributes of the next available slot of the
 * MunitSuite[] array of S1 to the MunitTest[] and MunitSuite[] arrays of S2,
 * using the given PREXIX. */
#define TEST_SUITE_ADD_CHILD(S1, PREFIX, S2)                               \
	__attribute__((constructor)) static void _##S1##_##S2##_init(void) \
	{                                                                  \
		int n = _##S1##SuitesN;                                    \
		_##S1##Suites[n].prefix = PREFIX;                          \
		_##S1##Suites[n].tests = _##S2##Tests;                     \
		_##S1##Suites[n].suites = _##S2##Suites;                   \
		_##S1##Suites[n].iterations = 0;                           \
		_##S1##Suites[n].options = 0;                              \
		_##S1##SuitesN = n + 1;                                    \
	}

/* Set the tests attribute of the next available slot of the MunitSuite[] array
 * of S to the MunitTest[] array of G, using /G as prefix. */
#define TEST_SUITE_ADD_GROUP(S, G)                                       \
	__attribute__((constructor)) static void _##S##_##G##_init(void) \
	{                                                                \
		int n = _##S##SuitesN;                                   \
		_##S##Suites[n].prefix = "/" #G;                         \
		_##S##Suites[n].tests = _##S##_##G##Tests;               \
		_##S##Suites[n].suites = NULL;                           \
		_##S##Suites[n].iterations = 0;                          \
		_##S##Suites[n].options = 0;                             \
		_##S##SuitesN = n + 1;                                   \
	}

/* Choose the appropriate TEST_SETUP_N_ARGS() macro depending on the number of
 * arguments passed to TEST_SETUP(). */
#define TEST_SETUP_MACRO_CHOOSER(...) \
	TEST_GET_3RD_ARG(__VA_ARGS__, TEST_SETUP_2_ARGS, TEST_SETUP_1_ARGS)

#define TEST_SETUP_1_ARGS(S)                                           \
	static void *S##Setup(const MunitParameter[], void *);         \
	__attribute__((constructor)) static void _##S##SetupInit(void) \
	{                                                              \
		_##S##Setup = S##Setup;                                \
	}                                                              \
	static void *S##Setup(const MunitParameter params[], void *userData)

#define TEST_SETUP_2_ARGS(S, F)                                        \
	__attribute__((constructor)) static void _##S##SetupInit(void) \
	{                                                              \
		_##S##Setup = F;                                       \
	}

/* Choose the appropriate TEST_TEAR_DOWN_N_ARGS() macro depending on the number
 * of arguments passed to TEST_TEAR_DOWN(). */
#define TEST_TEAR_DOWN_MACRO_CHOOSER(...)                    \
	TEST_GET_3RD_ARG(__VA_ARGS__, TEST_TEAR_DOWN_2_ARGS, \
			 TEST_TEAR_DOWN_1_ARGS)

#define TEST_TEAR_DOWN_1_ARGS(S)                                          \
	static void S##TearDown(void *data);                              \
	__attribute__((constructor)) static void _##S##TearDownInit(void) \
	{                                                                 \
		_##S##TearDown = S##TearDown;                             \
	}                                                                 \
	static void S##TearDown(void *data)

#define TEST_TEAR_DOWN_2_ARGS(S, F)                                       \
	__attribute__((constructor)) static void _##S##TearDownInit(void) \
	{                                                                 \
		_##S##TearDown = F;                                       \
	}

/* Choose the appropriate TEST_CASE_N_ARGS() macro depending on the number of
 * arguments passed to TEST_CASE(). */
#define TEST_CASE_MACRO_CHOOSER(...)                                      \
	TEST_GET_5TH_ARG(__VA_ARGS__, TEST_CASE_4_ARGS, TEST_CASE_3_ARGS, \
			 TEST_CASE_2_ARGS)

/* Add the test case to the module's MunitTest[] array. */
#define TEST_CASE_2_ARGS(C, PARAMS)                                  \
	static MunitResult test_##C(const MunitParameter[], void *); \
	TEST_CASE_ADD_TO_MODULE(C, PARAMS);                          \
	static MunitResult test_##C(const MunitParameter params[], void *data)

/* Add test case C to the MunitTest[] array of suite S. */
#define TEST_CASE_3_ARGS(S, C, PARAMS)                                     \
	static MunitResult test_##S##_##C(const MunitParameter[], void *); \
	TEST_CASE_ADD_TO_SUITE(S, C, PARAMS);                              \
	static MunitResult test_##S##_##C(const MunitParameter params[],   \
					  void *data)

/* Add test case C to the MunitTest[] array of group G of suite S. */
#define TEST_CASE_4_ARGS(S, G, C, PARAMS)                                      \
	static MunitResult test_##S##_##G##_##C(const MunitParameter[],        \
						void *);                       \
	TEST_CASE_ADD_TO_GROUP(S, G, C, PARAMS);                               \
	static MunitResult test_##S##_##G##_##C(const MunitParameter params[], \
						void *data)

/* Add a test case to the MunitTest[] array of the file module. */
#define TEST_CASE_ADD_TO_MODULE(C, PARAMS)                                     \
	__attribute__((constructor)) static void _moduleTests_##C##_init(void) \
	{                                                                      \
		MunitTest *tests = _moduleTests;                               \
		int n = _moduleTestsN;                                         \
		TEST_CASE_SET_IN_ARRAY(tests, n, "/" #C, test_##C, NULL, NULL, \
				       PARAMS);                                \
		_moduleTestsN = n + 1;                                         \
	}

/* Add a test case to the MunitTest[] array of suite S. */
#define TEST_CASE_ADD_TO_SUITE(S, C, PARAMS)                                  \
	__attribute__((constructor)) static void _##S##Tests_##C##_init(void) \
	{                                                                     \
		MunitTest *tests = _##S##Tests;                               \
		int n = _##S##TestsN;                                         \
		TEST_CASE_SET_IN_ARRAY(tests, n, "/" #C, test_##S##_##C,      \
				       _##S##Setup, _##S##TearDown, PARAMS);  \
		_##S##TestsN = n + 1;                                         \
	}

/* Add a test case to MunitTest[] array of group G in suite S. */
#define TEST_CASE_ADD_TO_GROUP(S, G, C, PARAMS)                                \
	__attribute__((constructor)) static void _##S##_##G##Tests_##C##_init( \
	    void)                                                              \
	{                                                                      \
		MunitTest *tests = _##S##_##G##Tests;                          \
		int n = _##S##_##G##TestsN;                                    \
		TEST_CASE_SET_IN_ARRAY(tests, n, "/" #C, test_##S##_##G##_##C, \
				       _##S##Setup, _##S##TearDown, PARAMS);   \
		_##S##_##G##TestsN = n + 1;                                    \
	}

/* Set the values of the I'th test case slot in the given test array */
#define TEST_CASE_SET_IN_ARRAY(TESTS, I, NAME, FUNC, SETUP, TEAR_DOWN, PARAMS) \
	TESTS[I].name = NAME;                                                  \
	TESTS[I].test = FUNC;                                                  \
	TESTS[I].setup = SETUP;                                                \
	TESTS[I].tear_down = TEAR_DOWN;                                        \
	TESTS[I].options = 0;                                                  \
	TESTS[I].parameters = PARAMS

#define TEST_GET_3RD_ARG(arg1, arg2, arg3, ...) arg3
#define TEST_GET_5TH_ARG(arg1, arg2, arg3, arg4, arg5, ...) arg5

#endif /* TEST_RUNNER_H */
