/* Exercise the real authenticated helper, gate, installation and restart without
 * starting capture. RFC8032 trust is isolated to this test translation unit. */
int engine_security_test_main(void);
#define main engine_security_test_main
#include "test_desktop_update.c"
#undef main
#include "../src/desktop_update_ui.c"

static CastEditionIdentity test_identity = {
    .edition = "community", .platform = "linux-x86_64", .version = "1.2.3"};
static CastLicenseInfo inspected_license;
static int inspection_result, inspection_calls;
const CastEditionIdentity *cast_edition_identity(void)
{
    return &test_identity;
}
int cast_license_inspect(const char *path, int64_t now, CastLicenseInfo *license, char *error,
                         size_t n)
{
    (void)now;
    ++inspection_calls;
    assert(test_identity.pro);
    assert(!strcmp(path, "test-license-location"));
    if (inspection_result) {
        return problem(error, n, "Invalid test license signature");
    }
    *license = inspected_license;
    return 0;
}
static void pro_eligibility_regressions(void)
{
    Config config = {0};
    strcpy(config.licensing_file, "test-license-location");
    CastDesktopUpdate candidate = {.released_at = 200};
    char error[CAST_ERR];
    inspection_calls = 0;
    assert(!eligible(&config, &candidate, error, sizeof error));
    assert(!inspection_calls);
    test_identity.pro = true;
    strcpy(test_identity.edition, "pro");
    inspected_license = (CastLicenseInfo){.state = CAST_LICENSE_VALID,
                                          .platform_supported = true,
                                          .perpetual = true,
                                          .updates_until = 200,
                                          .expires_at = 100};
    /* Perpetual usage survives expiry; update eligibility includes the cutoff. */
    assert(!eligible(&config, &candidate, error, sizeof error));
    candidate.released_at = 199;
    assert(!eligible(&config, &candidate, error, sizeof error));
    candidate.released_at = 201;
    assert(eligible(&config, &candidate, error, sizeof error) < 0);
    candidate.released_at = 200;
    inspected_license.perpetual = false;
    inspected_license.expires_at = 200;
    assert(!eligible(&config, &candidate, error, sizeof error));
    inspected_license.expires_at = 199;
    assert(eligible(&config, &candidate, error, sizeof error) < 0);
    inspected_license.expires_at = 300;
    const CastLicenseState refused[] = {CAST_LICENSE_MISSING,       CAST_LICENSE_INVALID,
                                        CAST_LICENSE_NOT_YET_VALID, CAST_LICENSE_EXPIRED,
                                        CAST_LICENSE_INELIGIBLE,    CAST_LICENSE_UNAVAILABLE};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
        inspected_license.state = refused[i];
        assert(eligible(&config, &candidate, error, sizeof error) < 0);
    }
    inspected_license.state = CAST_LICENSE_VALID;
    inspected_license.platform_supported = false;
    assert(eligible(&config, &candidate, error, sizeof error) < 0);
    inspected_license.platform_supported = true;
    inspection_result = -1;
    assert(eligible(&config, &candidate, error, sizeof error) < 0);
    assert(strstr(error, "Invalid test license signature"));
    inspection_result = 0;
    assert(!eligible(&config, &candidate, error, sizeof error));
    assert(inspection_calls > 10);
    test_identity.pro = false;
    strcpy(test_identity.edition, "community");
}
int app_runtime_client(const Config *config, int argc, const char *const *argv, char *error,
                       size_t n)
{
    (void)config;
    (void)argc;
    (void)argv;
    return problem(error, n, "No recorder is started by this test");
}
static void lifecycle_archive(const char *archive_path, const char *marker, bool rollback)
{
    char script[CAST_UPDATE_PATH_MAX + 512];
    int size =
        snprintf(script, sizeof script,
                 "#!/bin/sh\nif [ \"$1\" = --version ]; then %s echo cast 1.2.4; exit 0; "
                 "fi\nprintf 'restarted' > '%s'\n",
                 rollback ? "case \\\"$0\\\" in */stage/*) ;; *) exit 7 ;; esac;" : "", marker);
    assert(size > 0 && size < (int)sizeof script);
    struct archive *a = archive_write_new();
    assert(archive_write_set_format_pax_restricted(a) == ARCHIVE_OK);
    assert(archive_write_add_filter_gzip(a) == ARCHIVE_OK);
    assert(archive_write_open_filename(a, archive_path) == ARCHIVE_OK);
    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, "payload/cast");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0755);
    archive_entry_set_size(entry, size);
    assert(archive_write_header(a, entry) == ARCHIVE_OK);
    assert(archive_write_data(a, script, (size_t)size) == size);
    archive_entry_free(entry);
    assert(archive_write_close(a) == ARCHIVE_OK);
    archive_write_free(a);
}
static void pause_test(void)
{
    struct timespec delay = {.tv_nsec = 10000000};
    nanosleep(&delay, NULL);
}
static int lifecycle_case(bool rollback, bool migrate)
{
    char temp[] = "/tmp/cast-update-lifecycle-XXXXXX", root[CAST_UPDATE_PATH_MAX];
    assert(mkdtemp(temp));
    assert(realpath(temp, root));
    assert(!setenv("HOME", root, 1));
    char stage[CAST_UPDATE_PATH_MAX], archive_path[CAST_UPDATE_PATH_MAX];
    char manifest_path[CAST_UPDATE_PATH_MAX], marker[CAST_UPDATE_PATH_MAX];
    char target[CAST_UPDATE_PATH_MAX], binary[CAST_UPDATE_PATH_MAX], socket[CAST_UPDATE_PATH_MAX];
    char lockfile[CAST_UPDATE_PATH_MAX];
    assert(!pathcat(stage, sizeof stage, root, "stage"));
    assert(!mkdir(stage, 0700));
    assert(!pathcat(archive_path, sizeof archive_path, stage, "archive"));
    assert(!pathcat(manifest_path, sizeof manifest_path, stage, "manifest"));
    assert(!pathcat(marker, sizeof marker, root, "restarted"));
    assert(!pathcat(target, sizeof target, root, "runtime"));
    assert(!mkdir(target, 0755));
    assert(
        !pathcat(binary, sizeof binary, migrate ? root : target, migrate ? "loose-cast" : "cast"));
    FILE *file = fopen(binary, "wb");
    assert(file);
    assert(fprintf(file, "#!/bin/sh\nprintf restored > '%s'\n", marker) > 0);
    assert(!fclose(file));
    assert(!chmod(binary, 0755));
    lifecycle_archive(archive_path, marker, rollback);
    char digest[65];
    uint64_t size;
    hash_file(archive_path, digest, &size);
    unsigned char manifest[CAST_UPDATE_MANIFEST_MAX];
    size_t manifest_size;
    sign_manifest(manifest, &manifest_size, "community", "linux-x86_64", "cast", digest, size);
    file = fopen(manifest_path, "wb");
    assert(file);
    assert(fwrite(manifest, 1, manifest_size, file) == manifest_size);
    assert(!fclose(file));
    assert(!pathcat(socket, sizeof socket, root, "daemon.sock"));
    int z = snprintf(lockfile, sizeof lockfile, "%s.lock", socket);
    assert(z > 0 && z < (int)sizeof lockfile);
    int fd = open(lockfile, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    close(fd);
    /* Confirm another producer holding a shared gate excludes the installer. */
    char error[CAST_ERR];
    int shared = safety_lock(false, error, sizeof error);
    assert(shared >= 0);
    assert(safety_lock(true, error, sizeof error) < 0);
    close(shared);
    fflush(NULL);
    pid_t parent = fork();
    assert(parent >= 0);
    if (!parent) {
        int authorization[2];
        assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, authorization));
        pid_t helper = fork();
        assert(helper >= 0);
        if (!helper) {
            close(authorization[1]);
            if (authorization[0] != 3) {
                assert(dup2(authorization[0], 3) == 3);
                close(authorization[0]);
            }
            char pid[32];
            snprintf(pid, sizeof pid, "%ld", (long)getppid());
            char *args[] = {
                "--desktop-update-helper", stage, target, binary, socket, "", pid, "3", NULL};
            _exit(cast_desktop_update_helper(8, args));
        }
        close(authorization[0]);
        char ready;
        assert(recv(authorization[1], &ready, 1, 0) == 1 && ready == 'R');
        assert(send(authorization[1], "Y", 1, 0) == 1);
        close(authorization[1]);
        _exit(0);
    }
    int status;
    assert(waitpid(parent, &status, 0) == parent && WIFEXITED(status) && !WEXITSTATUS(status));
    int restarted = 0;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (!access(marker, F_OK)) {
            restarted = 1;
            break;
        }
        pause_test();
    }
    assert(restarted);
#ifndef __APPLE__
    if (migrate) {
        struct stat launcher;
        assert(!lstat(binary, &launcher) && S_ISLNK(launcher.st_mode));
        char previous_binary[CAST_UPDATE_PATH_MAX];
        int length =
            snprintf(previous_binary, sizeof previous_binary, "%s.before-desktop-update", binary);
        assert(length > 0 && length < (int)sizeof previous_binary);
        assert(!lstat(previous_binary, &launcher) && S_ISREG(launcher.st_mode));
    }
#endif
    file = fopen(marker, "rb");
    assert(file);
    char content[32] = {0};
    assert(fread(content, 1, sizeof content - 1, file) > 0);
    fclose(file);
    assert(!strcmp(content, rollback ? "restored" : "restarted"));
    file = fopen(binary, "rb");
    assert(file);
    char first[16] = {0};
    assert(fread(first, 1, 9, file) == 9);
    fclose(file);
    assert(!strncmp(first, "#!/bin/sh", 9));
    DIR *d = opendir(root);
    assert(d);
    struct dirent *entry;
    int previous = 0;
    while ((entry = readdir(d))) {
        if (!strncmp(entry->d_name, ".cast-update-previous-", 22)) {
            previous = 1;
        }
    }
    closedir(d);
    assert(rollback || previous);
    remove_tree(root);
    return 0;
}
int main(void)
{
    assert(engine_security_test_main() == 0);
    pro_eligibility_regressions();
    assert(!lifecycle_case(false, false));
    assert(!lifecycle_case(true, false));
#ifndef __APPLE__
    assert(!lifecycle_case(false, true));
#endif
    puts("desktop update authenticated install/restart/rollback lifecycle passed");
    return 0;
}
