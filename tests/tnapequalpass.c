#include <np.h>

#define DEFACED	((void *)0xbdefaced)
static void test_assert_ptr_equal_pass(void)
{
    void *x = DEFACED;
    NP_ASSERT_PTR_EQUAL(x, DEFACED);
}
