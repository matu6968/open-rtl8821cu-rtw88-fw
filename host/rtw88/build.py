#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build an isolated rtw88 bundle against exact Ubuntu source and headers.

No module is installed or loaded. Downloading source is opt-in with --fetch.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE = json.loads((HERE / 'source.json').read_text())
KERNEL_SOURCE = Path('linux-source-7.0.0')
RTW88_SOURCE = KERNEL_SOURCE / 'drivers/net/wireless/realtek/rtw88'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def verify_source(source):
    for name, expected in SOURCE['source_files'].items():
        if sha(source / name) != expected:
            raise ValueError('source hash mismatch: ' + name)


def prepare(source, output, fwlog=False):
    verify_source(source)
    work = (HERE / 'build').resolve()
    source, output = source.resolve(), output.resolve()
    if (output == work or not output.is_relative_to(work) or
            source.is_relative_to(output) or output.is_relative_to(source)):
        raise ValueError('output must be an isolated directory under host/build, outside the original source')
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(source, output)
    run(['patch', '--batch', '--fuzz=0', '-p1', '-i', HERE / 'rtw88-unified-wow.patch'], cwd=output)
    run(['patch', '--batch', '--fuzz=0', '-p1', '-i', HERE / 'rtw88-deep-auto.patch'], cwd=output)
    for name in ('source_wow.c', 'source_wow.h', 'source_deep.inc'):
        shutil.copyfile(HERE / name, output / name)
    if fwlog:
        run(['patch', '--batch', '--fuzz=0', '-p1', '-i', HERE / 'rtw88-fwlog.patch'], cwd=output)
        for name in ('source_fwlog.c', 'source_fwlog.h'):
            shutil.copyfile(HERE / name, output / name)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fetch', action='store_true')
    ap.add_argument('--source', type=Path, default=HERE / 'build' / RTW88_SOURCE)
    ap.add_argument('--headers', type=Path, default=Path('/lib/modules') / os.uname().release / 'build')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--prepare-only', action='store_true')
    ap.add_argument('--fwlog', action='store_true', help='include the opt-in C2H text decoder')
    args = ap.parse_args()
    work = HERE / 'build'
    work.mkdir(exist_ok=True)
    if os.uname().release != SOURCE['kernel_release']:
        ap.error('this source/ABI pin requires ' + SOURCE['kernel_release'])
    if args.fetch and not args.source.exists():
        if args.source != work / RTW88_SOURCE:
            ap.error('--fetch extracts into the default source directory')
        package = work / ('linux-source-7.0.0_' + SOURCE['package_version'] + '_all.deb')
        if not package.exists():
            run(['apt-get', 'download', SOURCE['package'] + '=' + SOURCE['package_version']], cwd=work)
        if sha(package) != SOURCE['package_sha256']:
            raise ValueError('source package hash mismatch')
        run(['dpkg-deb', '-x', package, work / 'package'])
        archive = work / 'package/usr/src' / (KERNEL_SOURCE.name + '.tar.bz2')
        run(['tar', '-xf', archive, RTW88_SOURCE], cwd=work)
    args.source = args.source.resolve()
    args.output = (args.output or work / ('fwlog/modules' if args.fwlog else 'modules')).resolve()
    prepare(args.source, args.output, args.fwlog)
    if args.prepare_only:
        return
    config = (args.headers / '.config').read_text()
    required_config = ['CONFIG_PM=y', 'CONFIG_PM_SLEEP=y', 'CONFIG_MODVERSIONS=y']
    if args.fwlog:
        required_config.append('CONFIG_RTW88_DEBUG=y')
    for entry in required_config:
        if entry not in config.splitlines():
            raise ValueError('required kernel configuration missing: ' + entry)
    selected = {'RTW88_CORE', 'RTW88_USB', 'RTW88_8821C', 'RTW88_8821CU'}
    choices = re.findall(r'obj-\$\(CONFIG_(RTW88_\w+)\)', (args.output / 'Makefile').read_text())
    settings = ['CONFIG_' + name + ('=m' if name in selected else '=n') for name in choices]
    # Ubuntu's installed modpost srcversion parser rejects literal backslash
    # continuation records emitted by its own fixdep. This optional metadata
    # switch leaves CONFIG_MODVERSIONS and all symbol CRC checks enabled.
    command = ['make', '-C', args.headers, 'M=' + str(args.output), *settings,
               'CONFIG_MODULE_SRCVERSION_ALL=', '-j' + str(args.jobs), 'modules']
    artifacts = work / 'fwlog' if args.fwlog else work
    artifacts.mkdir(parents=True, exist_ok=True)
    with (artifacts / 'build.log').open('w') as log:
        run(command, stdout=log, stderr=subprocess.STDOUT)
    modules = {}
    for name in SOURCE['modules']:
        path = args.output / (name + '.ko')
        vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', path], text=True).strip()
        if not vermagic.startswith(SOURCE['kernel_release'] + ' ') or 'modversions' not in vermagic:
            raise ValueError('unexpected module vermagic: ' + name)
        modules[name] = {'path': str(path), 'sha256': sha(path), 'vermagic': vermagic}
    report = {'source': SOURCE, 'kernel': os.uname().release, 'headers': str(args.headers.resolve()),
              'patch_sha256': sha(HERE / 'rtw88-unified-wow.patch'),
              'auto_patch_sha256': sha(HERE / 'rtw88-deep-auto.patch'),
              'overlay_sha256': {name: sha(HERE / name) for name in ('source_wow.c', 'source_wow.h', 'source_deep.inc')},
              'module_srcversion_generated': False, 'symbol_crc_checks': True, 'modules': modules}
    if args.fwlog:
        report['fwlog_sha256'] = {name: sha(HERE / name) for name in
                                  ('rtw88-fwlog.patch', 'source_fwlog.c', 'source_fwlog.h')}
    manifest = artifacts / 'bundle.json'
    manifest.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'modules': list(modules), 'manifest': str(manifest)}))


if __name__ == '__main__':
    main()
