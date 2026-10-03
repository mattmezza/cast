#ifndef COMPOSITION_ASSETS_H
#define COMPOSITION_ASSETS_H
#include "cast.h"
/* Decode one bounded local regular image. Protocols and shell commands are never used. */
int composition_logo_load(const char *, Frame *, char *, size_t);
int composition_image_scale(const Frame *, Frame *, int, int, char *, size_t);
#endif
