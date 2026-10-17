#!/usr/bin/env python3
"""Link PipeWire client modules and SPA factories into one allocator domain."""
from pathlib import Path
import shutil
import subprocess

MODULES = ('protocol-native', 'client-node', 'client-device', 'adapter', 'metadata', 'session-manager')
SPAS = ('support', 'audioconvert', 'audiomixer')


def prepare(source):
    path = source / 'src/pipewire/impl-module.c'
    text = path.read_text()
    needle = 'static char *find_module(const char *path, const char *name, int level)\n{'
    guard = '\n\tif (' + ' || '.join('spa_streq(name, "libpipewire-module-' + name + '")' for name in MODULES) + ')\n\t\treturn strdup(name);\n'
    if needle not in text:
        raise ValueError('PipeWire module loader changed')
    path.write_text(text.replace(needle, needle + guard))


def build(pwbuild, prefix, environment):
    output = pwbuild / 'static-client'
    if output.exists():
        shutil.rmtree(output)
    output.mkdir()
    groups = [('client', sorted(pwbuild.glob('src/pipewire/libpipewire-0.3.so*.p/*.o')))]
    for module in MODULES:
        groups.append(('libpipewire-module-' + module,
                       sorted(pwbuild.glob('src/modules/libpipewire-module-' + module + '.so.p/**/*.o'))))
    for spa in SPAS:
        groups.append(('libspa-' + spa, sorted((pwbuild / 'spa/plugins' / spa).rglob('*.o'))))
    objects, entries, declarations = [], [], []
    spa_node_added = False
    for index, (name, files) in enumerate(groups):
        if not files:
            raise ValueError('Missing static PipeWire group: ' + name)
        exports = {}
        for original in files:
            if 'spa-node.c.o' in original.name:
                if spa_node_added:
                    continue
                spa_node_added = True
            if 'dumpcoeff' in str(original) or '/test' in str(original) or '/benchmark' in str(original):
                continue
            target = output / (str(len(objects)) + '.o')
            shutil.copy2(original, target)
            symbols = subprocess.check_output(['nm', '-g', '--defined-only', target], text=True)
            replacements = []
            for symbol in ('pipewire__module_init', 'spa_handle_factory_enum', 'spa_log_topic_enum'):
                if any(line.split()[-1:] == [symbol] for line in symbols.splitlines()):
                    replacement = 'cast_pw_builtin_' + str(index) + '_' + symbol
                    exports[symbol] = replacement
                    declarations.append('extern void ' + replacement + '(void);')
                    replacements += ['--redefine-sym', symbol + '=' + replacement]
            for symbol, replacement in [('dlopen', 'cast_pw_static_open'), ('dlsym', 'cast_pw_static_symbol'),
                                        ('dlclose', 'cast_pw_static_close'), ('dlerror', 'cast_pw_static_error')]:
                replacements += ['--redefine-sym', symbol + '=' + replacement]
            subprocess.run(['objcopy', *replacements, target], check=True)
            objects.append(target)
        if name != 'client':
            pointers = [exports.get(symbol) for symbol in ('pipewire__module_init', 'spa_handle_factory_enum', 'spa_log_topic_enum')]
            entries.append('{"' + name + '", ' + ', '.join('(void *)' + value if value else 'NULL' for value in pointers) + '}')
    source = output / 'loader.c'
    source.write_text('''#include <stddef.h>
#include <string.h>
''' + '\n'.join(declarations) + '''
struct builtin { const char *name; void *module, *factory, *topics; };
static struct builtin builtins[] = {
''' + ',\n'.join(entries) + '''
};
void *cast_pw_static_open(const char *path, int flags) {
    (void)flags;
    if (!path) return NULL;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t length = strlen(name);
    if (length > 3 && !strcmp(name + length - 3, ".so")) length -= 3;
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (strlen(builtins[i].name) == length && !strncmp(name, builtins[i].name, length)) return &builtins[i];
    return NULL;
}
void *cast_pw_static_symbol(void *handle, const char *name) {
    struct builtin *entry = handle;
    if (!entry) return NULL;
    if (!strcmp(name, "pipewire__module_init")) return entry->module;
    if (!strcmp(name, "spa_handle_factory_enum")) return entry->factory;
    if (!strcmp(name, "spa_log_topic_enum")) return entry->topics;
    return NULL;
}
int cast_pw_static_close(void *handle) { (void)handle; return 0; }
char *cast_pw_static_error(void) { return "module is not built into the static PipeWire client"; }
''')
    loader = output / 'loader.o'
    subprocess.run(['cc', '-O2', '-c', source, '-o', loader], env=environment, check=True)
    archive = prefix / 'lib/libpipewire-0.3.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['ar', 'rcs', archive, *objects, loader], check=True)
