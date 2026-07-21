/* MD5 known-answer test (RFC 1321 vectors). Guards the md5_hex() used for SIP
 * Digest auth against a silent regression — e.g. a mis-threaded state variable
 * in md5_compress. Run before and after any change to that code. */
#include "sip_util.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const struct { const char *in, *want; } v[] = {
        { "",    "d41d8cd98f00b204e9800998ecf8427e" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        char out[33];
        md5_hex(v[i].in, out);
        int ok = strcmp(out, v[i].want) == 0;
        printf("  md5(\"%s\") = %s  %s\n", v[i].in, out, ok ? "OK" : "FAIL");
        if (!ok) { fails++; printf("    expected %s\n", v[i].want); }
    }
    printf(fails ? "MD5 KAT FAILED\n" : "MD5 KAT PASS\n");
    return fails ? 1 : 0;
}
