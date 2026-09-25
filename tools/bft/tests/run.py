#!/usr/bin/env python3
"""Run the production BFT client against Marlin's receiver, including compression.
Only platform headers / physical UART, clock and SD are replaced. No pip packages.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import argparse

root = Path(__file__).resolve().parents[3]
tests = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--marlin-src', type=Path,
                    help='Marlin/src directory containing feature/ and libs/')
args = parser.parse_args()
receiver = (args.marlin_src / 'feature/binary_stream.h' if args.marlin_src
            else root / 'sample_code/binary_stream.h')
decoder = (args.marlin_src / 'libs/heatshrink' if args.marlin_src
           else root / 'sample_code/heatshrink')
if not receiver.is_file() or not (decoder / 'heatshrink_decoder.cpp').is_file():
    parser.error('Receiver sources missing; pass --marlin-src /path/to/Marlin/src')
with tempfile.TemporaryDirectory(prefix='esp3d-bft-tests-') as directory:
    work = Path(directory)
    files = {
        root / 'esp3d/src/modules/bft/binary_file_transfer_service.cpp': 'esp3d/src/modules/bft/binary_file_transfer_service.cpp',
        root / 'esp3d/src/modules/bft/binary_file_transfer_service.h': 'esp3d/src/modules/bft/binary_file_transfer_service.h',
        tests / 'esp3d_config.h': 'esp3d/src/include/esp3d_config.h',
        tests / 'serial_service.h': 'esp3d/src/modules/serial/serial_service.h',
        receiver: 'Marlin/feature/binary_stream.h',
        tests / 'MarlinConfig.h': 'Marlin/inc/MarlinConfig.h',
        tests / 'test_service.cpp': 'test_service.cpp',
    }
    for source, target in files.items():
        dest = work / target
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
    shutil.copyfile(tests / 'MarlinConfig.h', work / 'Marlin/inc/MarlinConfigPre.h')
    shutil.copytree(decoder, work / 'Marlin/libs/heatshrink')
    encoder = root / 'esp3d/src/modules/bft/heatshrink'
    shutil.copytree(encoder, work / 'esp3d/src/modules/bft/heatshrink')
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-Wall', '-Wextra',
                    '-Werror', '-Wno-implicit-fallthrough', '-c',
                    str(encoder / 'heatshrink_encoder.c'),
                    '-o', str(work / 'encoder.o')], check=True)
    symbols = subprocess.check_output(['nm', '-u', str(work / 'encoder.o')], text=True)
    assert not any(name in symbols.split() for name in ('malloc', 'calloc', 'realloc', 'free'))
    compiler = os.environ.get('CXX', 'g++')
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-implicit-fallthrough', '-Wno-empty-body',
                    '-O0', '-g', '-I', str(work),
                    str(work / 'test_service.cpp'),
                    str(work / 'esp3d/src/modules/bft/binary_file_transfer_service.cpp'),
                    str(work / 'Marlin/libs/heatshrink/heatshrink_decoder.cpp'),
                    str(work / 'encoder.o'),
                    '-o', str(work / 'test_bft')], check=True)
    subprocess.run([str(work / 'test_bft')], check=True)

    serial = (root / 'esp3d/src/modules/serial/serial_service.cpp').read_text()
    methods = serial[serial.index('size_t ESP3DSerialService::writeBytes('):
                     serial.index('size_t ESP3DSerialService::readBytes(')]
    (work / 'serial_write_methods.inc').write_text(methods)
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(work), str(tests / 'test_serial.cpp'),
                    '-o', str(work / 'test_serial')], check=True)
    subprocess.run([str(work / 'test_serial')], check=True)
