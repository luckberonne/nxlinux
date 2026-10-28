#include "../src/host/sha256.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
static void check(const char *msg, size_t n, int repeat, const char *want) {
    sha256_ctx c; uint8_t d[32]; char hex[65];
    sha256_init(&c);
    for (int i = 0; i < repeat; i++) sha256_update(&c, msg, n);
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    if (strcmp(hex, want)) { printf("FAIL %s...: got %s\n", n ? msg : "(empty)", hex); g_fail = 1; }
}
int main(void) {
    check("", 0, 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", 3, 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, 1, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");   // two blocks
    check("a", 1, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");                                                                 // FIPS 180-4 long message
    check("0123456789012345678901234567890123456789012345678901234567890123", 64, 1, "9674d9e078535b7cec43284387a6ee39956188e735a85452b0050b55341cda56");   // exactly one block (checked against sha256sum)
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
