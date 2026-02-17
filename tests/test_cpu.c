#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

static void test_placeholder(void) {
    CU_ASSERT_TRUE(1);
}

int main(void) {
    if (CU_initialize_registry() != CUE_SUCCESS)
        return CU_get_error();

    CU_pSuite suite = CU_add_suite("CPU (placeholder)", NULL, NULL);
    CU_add_test(suite, "placeholder", test_placeholder);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    unsigned int failures = CU_get_number_of_failures();
    CU_cleanup_registry();

    return failures ? 1 : 0;
}
