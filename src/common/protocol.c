#include "protocol.h"

#include <string.h>

int proto_tokenize(char *line, char *fields[], int max) {
    int nf = 0;
    char *tok = strtok(line, "|");
    while (tok && nf < max) {
        fields[nf++] = tok;
        tok = strtok(NULL, "|");
    }
    return nf;
}

void proto_sanitize(const char *in, char *out, size_t out_sz) {
    if (out_sz == 0) return;
    size_t o = 0;
    /* Cada '|' crece a 2 bytes; reservar 1 para '\0'. */
    for (size_t i = 0; in[i] != '\0'; i++) {
        if (in[i] == '|') {
            if (o + 2 >= out_sz) break;
            out[o++] = ':';
            out[o++] = ':';
        } else {
            if (o + 1 >= out_sz) break;
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}
