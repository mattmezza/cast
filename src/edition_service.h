#ifndef CAST_EDITION_SERVICE_H
#define CAST_EDITION_SERVICE_H
#include "cast.h"
#include "edition.h"

typedef struct EditionService EditionService;
typedef struct {
    int fd, result;
    char output[CAST_IPC_MAX - 16];
} EditionReply;
/* A bounded worker owns license I/O/verification. Submit transfers the peer fd
 * only on success; the daemon thread sends completed replies. */
EditionService *edition_service_open(const char *, char *, size_t);
int edition_service_submit(EditionService *, int, int, char **, const Config *, char *, size_t);
bool edition_service_busy(EditionService *);
bool edition_service_reply(EditionService *, EditionReply *);
void edition_service_snapshot(EditionService *, CastEditionSnapshot *);
uint64_t edition_service_checked_at(EditionService *);
void edition_service_path(EditionService *, const char *);
void edition_service_close(EditionService *);
#endif
