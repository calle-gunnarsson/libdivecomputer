#ifndef SUUNTO_H
#define SUUNTO_H

#define SUUNTO_SUCCESS         0
#define SUUNTO_ERROR          -1
#define SUUNTO_ERROR_IO       -2
#define SUUNTO_ERROR_MEMORY   -3
#define SUUNTO_ERROR_PROTOCOL -4
#define SUUNTO_ERROR_TIMEOUT  -5

typedef void (*dive_callback_t) (const unsigned char *data, unsigned int size, void *userdata);

#include "suunto_eon.h"
#include "suunto_vyper.h"
#include "suunto_vyper2.h"
#include "suunto_d9.h"

#endif /* SUUNTO_H */