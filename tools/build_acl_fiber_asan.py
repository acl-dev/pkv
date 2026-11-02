#!/usr/bin/env python3
"""Prepare a private Darwin arm64 ACL fcontext archive for ASan tests."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('Unsupported ACL source anchor: ' + repr(old))
    return source.replace(old, new, 1)


def patched(source):
    edits = [
        ('#ifdef SYS_UNIX', '#ifdef SYS_UNIX\n'
         'extern void __sanitizer_start_switch_fiber(void**, '
         'const void*, size_t);\n'
         'extern void __sanitizer_finish_switch_fiber(void*, '
         'const void**, size_t*);'),
        ('\tsize_t dlen;', '\tsize_t dlen;\n\tvoid *asan_fake;\n'
         '\tconst void *asan_bottom;\n\tsize_t asan_size;'),
        ('\ttrans      = jump_fcontext(to->fcontext, &jump);',
         '\t__sanitizer_start_switch_fiber(\n'
         '\t\tfrom->fiber.status == FIBER_STATUS_EXITING ? NULL : '
         '&from->asan_fake,\n'
         '\t\tto->asan_bottom, to->asan_size);\n'
         '\ttrans      = jump_fcontext(to->fcontext, &jump);'),
        ('\tjmp        = (JUMP_CTX*) trans.data;',
         '\tjmp        = (JUMP_CTX*) trans.data;\n'
         '\t__sanitizer_finish_switch_fiber(from->asan_fake,\n'
         '\t\t&jmp->from->asan_bottom, &jmp->from->asan_size);'),
        ('\tJUMP_CTX *jmp = (JUMP_CTX*) arg.data;',
         '\tJUMP_CTX *jmp = (JUMP_CTX*) arg.data;\n'
         '\t__sanitizer_finish_switch_fiber(jmp->to->asan_fake,\n'
         '\t\t&jmp->from->asan_bottom, &jmp->from->asan_size);'),
        ('\tfb->fn  = fn;', '\tfb->asan_fake = NULL;\n'
         '\tfb->asan_bottom = fb->buff;\n\tfb->asan_size = fb->size;\n'
         '\tfb->fn  = fn;'),
    ]
    for old, new in edits:
        source = replace_once(source, old, new)
    return source


def fingerprint(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--acl-source', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='/usr/bin/cc')
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('Only validated Darwin arm64 fcontext is supported')
    root = args.acl_source.resolve() / 'lib_fiber/c'
    original = root / 'src/fiber/fiber_unix.c'
    library = args.library.resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error('Output must be a new directory; inputs are preserved')
    source = patched(original.read_text())
    members = subprocess.check_output(['ar', '-t', str(library)], text=True)
    if members.splitlines().count('fiber_unix.o') != 1:
        parser.error('Expected one fiber_unix.o in the matching archive')
    output.mkdir(parents=True)
    generated = output / 'fiber_unix.c'
    generated.write_text(source)
    obj = output / 'fiber_unix.o'
    args_cc = [args.cc, '-c', '-O3', '-g', '-std=gnu99', '-DMACOSX',
        '-DUSE_BOOST_JMP', '-DUSE_FAST_RING', '-DUSE_CLOCK_GETTIME',
        '-D_REENTRANT', '-D_POSIX_PTHREAD_SEMANTICS', '-D_USE_FAST_MACRO']
    args_cc += ['-I' + str(root / path) for path in
        ['include', 'src', 'src/common', 'src/fiber']]
    args_cc += [str(generated), '-o', str(obj)]
    subprocess.run(args_cc, check=True)
    archive = output / 'libfiber.a'
    shutil.copy2(library, archive)
    subprocess.run(['ar', '-r', str(archive), str(obj)], check=True)
    subprocess.run(['ranlib', str(archive)], check=True)
    report = {'platform': platform.platform(), 'compiler_command': args_cc,
        'scope': 'ASan stack registration for private fcontext stacks only; '
            'requires the source tree matching the input ACL archive',
        'fingerprints': {str(path): fingerprint(path) for path in
            [original, library, generated, obj, archive, Path(__file__)]}}
    (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    print('ASan-only FIBER_LIBRARY=' + str(archive))


if __name__ == '__main__':
    main()
