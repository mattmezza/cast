#include "config_migrate.h"
#include "cast.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIGRATION_LIMIT (128 * 1024)
static int write_new(const char *path, const char *data, size_t length, char *error, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        snprintf(error, size, "cannot create migration output %s: %s", path, strerror(errno));
        return 5;
    }
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, data + offset, length - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            close(fd);
            unlink(path);
            snprintf(error, size, "cannot write migration output");
            return 5;
        }
        offset += (size_t)written;
    }
    if (fsync(fd)) {
        close(fd);
        unlink(path);
        snprintf(error, size, "cannot sync migration output");
        return 5;
    }
    close(fd);
    return 0;
}
int cast_config_migrate(int argc, char **argv, const char *default_path, char *error, size_t size)
{
    const char *path = default_path, *edition = "pro", *output = NULL, *backup = NULL;
    bool selected_path = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--edition") || !strcmp(argv[i], "--write") ||
            !strcmp(argv[i], "--backup")) {
            const char *option = argv[i++];
            if (i == argc) {
                snprintf(error, size, "%s requires a value", option);
                return 2;
            }
            if (!strcmp(option, "--edition")) {
                edition = argv[i];
            } else if (!strcmp(option, "--write")) {
                output = argv[i];
            } else {
                backup = argv[i];
            }
        } else if (argv[i][0] != '-' && !selected_path) {
            path = argv[i];
            selected_path = true;
        } else {
            snprintf(
                error, size,
                "config migrate [PATH] [--edition community|pro] [--write OUTPUT] [--backup FILE]");
            return 2;
        }
    }
    if (strcmp(edition, "community") && strcmp(edition, "pro")) {
        snprintf(error, size, "migration edition must be community or pro");
        return 2;
    }
    Config config;
    if (config_load(&config, path, true, error, size)) {
        return 2;
    }
    FILE *file = fopen(path, "r");
    char *original = calloc(1, MIGRATION_LIMIT + 1);
    char *result = calloc(1, MIGRATION_LIMIT + 4096);
    if (!file || !original || !result) {
        if (file) {
            fclose(file);
        }
        free(original);
        free(result);
        snprintf(error, size, "cannot read migration source");
        return 5;
    }
    size_t length = fread(original, 1, MIGRATION_LIMIT + 1, file);
    bool failed = ferror(file) || length > MIGRATION_LIMIT;
    fclose(file);
    if (failed) {
        free(original);
        free(result);
        snprintf(error, size, "migration source exceeds 128 KiB or is unreadable");
        return 2;
    }
    original[length] = 0;
    const char *sections[] = {"record", "record", "stream"};
    const char *keys[] = {"video_codec", "rate_control", "video_encoder"};
    const char *values[] = {"auto", !strcmp(edition, "pro") ? "bitrate" : "auto", "auto"};
    bool seen[3] = {false, false, false};
    char section[64] = "";
    size_t used = 0;
    for (const char *start = original; *start;) {
        const char *end = strchr(start, '\n');
        size_t count = end ? (size_t)(end - start) + 1 : strlen(start);
        const char *trim = start;
        while (trim < start + count && isspace((unsigned char)*trim)) {
            trim++;
        }
        if (*trim == '[') {
            const char *close = memchr(trim, ']', (size_t)(start + count - trim));
            if (close && close - trim - 1 < (int)sizeof section) {
                memcpy(section, trim + 1, (size_t)(close - trim - 1));
                section[close - trim - 1] = 0;
                char *name = section;
                while (isspace((unsigned char)*name)) {
                    name++;
                }
                memmove(section, name, strlen(name) + 1);
                size_t section_length = strlen(section);
                while (section_length && isspace((unsigned char)section[section_length - 1])) {
                    section[--section_length] = 0;
                }
            }
        }
        int replacement = -1;
        for (int i = 0; i < 3; ++i) {
            size_t key_length = strlen(keys[i]);
            if (!strcmp(section, sections[i]) && !strncmp(trim, keys[i], key_length)) {
                const char *after = trim + key_length;
                while (after < start + count && isspace((unsigned char)*after)) {
                    after++;
                }
                if (*after == '=' || *after == ':') {
                    replacement = i;
                }
            }
        }
        if (replacement >= 0) {
            const char *comment = NULL;
            const char *line_end = start + count;
            while (line_end > start && (line_end[-1] == '\n' || line_end[-1] == '\r')) {
                line_end--;
            }
            for (const char *cursor = trim + strlen(keys[replacement]); cursor < line_end;
                 cursor++) {
                if ((*cursor == '#' || *cursor == ';') && cursor > start &&
                    isspace((unsigned char)cursor[-1])) {
                    comment = cursor;
                    break;
                }
            }
            /* Inline comments are disabled in Cast's INI parser. Retain an
             * encoder suffix separately, outside the replacement name. */
            used += (size_t)snprintf(result + used, MIGRATION_LIMIT + 4096 - used,
                                     "%s = %s\n%.*s%s", keys[replacement], values[replacement],
                                     comment ? (int)(line_end - comment) : 0,
                                     comment ? comment : "", comment ? "\n" : "");
            seen[replacement] = true;
        } else {
            memcpy(result + used, start, count);
            used += count;
            result[used] = 0;
        }
        start += count;
    }
    if (used && result[used - 1] != '\n') {
        result[used++] = '\n';
    }
    for (int i = 0; i < 3; ++i) {
        if (!seen[i]) {
            /* INI permits repeated section headers. Only append a missing key
             * after examining the whole input, so a later section cannot turn
             * an earlier insertion into a duplicate. */
            if (strcmp(section, sections[i])) {
                used += (size_t)snprintf(result + used, MIGRATION_LIMIT + 4096 - used, "\n[%s]\n",
                                         sections[i]);
                snprintf(section, sizeof section, "%s", sections[i]);
            }
            used += (size_t)snprintf(result + used, MIGRATION_LIMIT + 4096 - used, "%s = %s\n",
                                     keys[i], values[i]);
        }
    }
    int rc = 0;
    if (!output) {
        printf("Dry-run migration for %s: record.video_codec=auto, record.rate_control=%s, "
               "stream.video_encoder=auto.\n",
               edition, values[1]);
        puts("Legacy CRF/preset values are preserved; inactive on OpenH264. Encoder readiness is "
             "checked separately.");
        fwrite(result, 1, used, stdout);
    } else {
        if (backup) {
            rc = write_new(backup, original, length, error, size);
        }
        if (!rc && !strcmp(output, path)) {
            if (!backup) {
                snprintf(error, size, "overwriting the input requires --backup FILE");
                rc = 2;
            } else {
                char temporary[PATH_MAX];
                if (snprintf(temporary, sizeof temporary, "%s.migration.%ld", output,
                             (long)getpid()) >= (int)sizeof temporary) {
                    snprintf(error, size, "migration output path is too long");
                    rc = 2;
                } else if (!(rc = write_new(temporary, result, used, error, size))) {
                    struct stat before;
                    if (lstat(output, &before) || !S_ISREG(before.st_mode) ||
                        before.st_uid != getuid() || rename(temporary, output)) {
                        unlink(temporary);
                        snprintf(error, size, "cannot atomically replace migration source");
                        rc = 5;
                    }
                }
            }
        } else if (!rc) {
            rc = write_new(output, result, used, error, size);
        }
        if (!rc) {
            printf("Migration written: %s; original preserved%s\n", output,
                   backup ? " in backup" : "");
        }
    }
    free(original);
    free(result);
    return rc;
}
