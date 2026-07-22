#include "platform/rnet_platform.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect_true(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void)
{
    char host[32];
    rnet_u16 port = 0;

    expect_true(rnet_os_parse_hostport("127.0.0.1:0", host, sizeof(host), &port) == 0,
                "ephemeral port parses");
    expect_true(strcmp(host, "127.0.0.1") == 0 && port == 0, "ephemeral endpoint values");

    expect_true(rnet_os_parse_hostport(":7777", host, sizeof(host), &port) == 0,
                "wildcard host parses");
    expect_true(strcmp(host, "0.0.0.0") == 0 && port == 7777, "wildcard endpoint values");

    expect_true(rnet_os_parse_hostport("127.0.0.1:", host, sizeof(host), &port) != 0,
                "empty port rejected");
    expect_true(rnet_os_parse_hostport("127.0.0.1:not-a-port", host, sizeof(host), &port) != 0,
                "nonnumeric port rejected");
    expect_true(rnet_os_parse_hostport("127.0.0.1:7777x", host, sizeof(host), &port) != 0,
                "trailing port text rejected");
    expect_true(rnet_os_parse_hostport("127.0.0.1:65536", host, sizeof(host), &port) != 0,
                "out-of-range port rejected");

    if (failures == 0)
    {
        printf("hostport_test: ok\n");
        return 0;
    }
    fprintf(stderr, "hostport_test: %d failure(s)\n", failures);
    return 1;
}
