#include <stdio.h>

#include "unity.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();

    /* For interactive menus use unity_run_menu(); here we exit after one CI-style pass. */
    printf("Tests finished.\n");
}
