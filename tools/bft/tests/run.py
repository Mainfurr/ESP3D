#!/usr/bin/env python3
"""Run the production BFT client against sample_code's uncompressed Marlin receiver.
Only platform headers / physical UART, clock and SD are replaced. No pip packages.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
tests = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='esp3d-bft-tests-') as directory:
    work = Path(directory)
    files = {
        root / 'esp3d/src/modules/bft/binary_file_transfer_service.cpp': 'esp3d/src/modules/bft/binary_file_transfer_service.cpp',
        root / 'esp3d/src/modules/bft/binary_file_transfer_service.h': 'esp3d/src/modules/bft/binary_file_transfer_service.h',
        tests / 'esp3d_config.h': 'esp3d/src/include/esp3d_config.h',
        tests / 'serial_service.h': 'esp3d/src/modules/serial/serial_service.h',
        root / 'sample_code/binary_stream.h': 'Marlin/feature/binary_stream.h',
        tests / 'MarlinConfig.h': 'Marlin/inc/MarlinConfig.h',
        tests / 'test_service.cpp': 'test_service.cpp',
    }
    for source, target in files.items():
        dest = work / target
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
    compiler = os.environ.get('CXX', 'g++')
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-implicit-fallthrough', '-O0', '-g', '-I', str(work),
                    str(work / 'test_service.cpp'),
                    str(work / 'esp3d/src/modules/bft/binary_file_transfer_service.cpp'),
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
