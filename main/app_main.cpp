/**
 * @file app_main.cpp
 * @brief Entry point. The application itself is App — see main/app.hpp.
 */

#include "app.hpp"

extern "C" void app_main(void)
{
    /* The App lives in run_app(), so it is one firmware-lifetime object owned
     * nowhere else and reachable by nothing else — no file-scope pointers, no
     * implicit ownership (issue #59). */
    run_app();
}
