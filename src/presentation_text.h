#ifndef PRESENTATION_TEXT_H
#define PRESENTATION_TEXT_H
#include "cast.h"
#include <time.h>

typedef struct PresentationText PresentationText;
int presentation_template_validate(const char *, char *, size_t);
int presentation_template_expand(const char *, const struct tm *, char *, size_t, char *, size_t);
PresentationText *presentation_text_create(void);
void presentation_text_destroy(PresentationText *);
int presentation_text_prepare(PresentationText *, const Config *, char *, size_t);
int presentation_text_draw(PresentationText *, const Config *, bool blur, Frame *, char *, size_t);
#endif
