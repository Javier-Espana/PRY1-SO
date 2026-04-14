#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#include "chat_protocol.h"

/*
 * Formato del protocolo:
 *   TIPO|CAMPO1|CAMPO2|...|CAMPON\n
 * Campos separados por '|', mensaje termina en '\n'. Max BUF_SIZE bytes.
 */

/* Parte una linea por '|' in-place. Retorna # de campos. */
int proto_tokenize(char *line, char *fields[], int max);

/* Copia `in` a `out` reemplazando cada '|' por '::' (el separador no debe
 * aparecer en texto de usuario). Trunca si no cabe. */
void proto_sanitize(const char *in, char *out, size_t out_sz);

#endif
