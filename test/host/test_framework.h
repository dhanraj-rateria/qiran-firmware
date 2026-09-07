#ifndef QIRAN_TEST_FRAMEWORK_H
#define QIRAN_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdint.h>

static int g_checks;
static int g_failures;
static const char *g_case;

#define TEST_CASE(name)                                                       \
    do {                                                                      \
        g_case = (name);                                                      \
        printf("-- %s\n", g_case);                                            \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                        \
    do {                                                                      \
        unsigned long long a_ = (unsigned long long)(actual);                 \
        unsigned long long e_ = (unsigned long long)(expected);               \
        g_checks++;                                                           \
        if (a_ != e_) {                                                       \
            g_failures++;                                                     \
            printf("   FAIL %s:%d [%s] %s = %llu, expected %llu\n",           \
                   __FILE__, __LINE__, g_case, #actual, a_, e_);              \
        }                                                                     \
    } while (0)

#define CHECK_TRUE(cond)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            printf("   FAIL %s:%d [%s] %s\n",                                 \
                   __FILE__, __LINE__, g_case, #cond);                        \
        }                                                                     \
    } while (0)

#define TEST_REPORT()                                                         \
    (printf("\n%d checks, %d failures\n", g_checks, g_failures),              \
     (g_failures == 0) ? 0 : 1)

#endif
