#ifndef CAST_LICENSE_STORE_H
#define CAST_LICENSE_STORE_H
#include "edition.h"
#include <stddef.h>
int cast_license_store_path(const char *, char *, size_t, char *, size_t);
int cast_license_store_read(const char *, unsigned char *, size_t, size_t *, char *, size_t);
/* Lock is exclusive and covers verify/read/write; fd released with close(). */
int cast_license_store_lock(const char *, char *, size_t);
int cast_license_store_write(const char *, const unsigned char *, size_t, char *, size_t);
int cast_license_store_remove(const char *, char *, size_t);
#endif
