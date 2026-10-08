# /******************************************************************************\
#
#                   This file is part of the Folding@home Client.
#
#           The fah-client runs Folding@home protein folding simulations.
#                     Copyright (c) 2001-2026, foldingathome.org
#                                All rights reserved.
#
#        This program is free software; you can redistribute it and/or modify
#        it under the terms of the GNU General Public License as published by
#         the Free Software Foundation; either version 3 of the License, or
#                        (at your option) any later version.
#
#          This program is distributed in the hope that it will be useful,
#           but WITHOUT ANY WARRANTY; without even the implied warranty of
#           MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#                    GNU General Public License for more details.
#
#      You should have received a copy of the GNU General Public License along
#      with this program; if not, write to the Free Software Foundation, Inc.,
#            51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
#                   For information regarding this software email:
#                                  Joseph Coffland
#                           joseph@cauldrondevelopment.com
#
# \******************************************************************************/

from pathlib import Path
import os, subprocess, tempfile

root = Path(__file__).resolve().parents[2]

def compile_and_run(harness, sources=(), *, includes=(root/'src',),
                    defines=(), files=None, cxx=None):
    """Compile one isolated scheduling harness and run it, failing immediately."""
    with tempfile.TemporaryDirectory(prefix='fah-scheduling-') as directory:
        build = Path(directory)
        (build/'test.cpp').write_text(harness)
        for name, contents in (files or {}).items():
            path = build/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        paths = ([build] if files else []) + list(includes)
        sources = [str(root/'src/fah/client'/name) for name in sources]
        if os.name == 'nt':
            command = ['cl', '/nologo', '/EHsc', '/std:c++17']
            command += ['/D'+name for name in defines] + ['/I'+str(path) for path in paths]
            command += ['test.cpp', *sources, '/Fetest.exe']
            executable = build/'test.exe'
        else:
            command = [cxx or os.environ.get('CXX', 'c++'), '-std=c++17']
            command += ['-D'+name for name in defines] + ['-I'+str(path) for path in paths]
            command += ['test.cpp', *sources, '-o', 'test']
            executable = build/'test'
        subprocess.run(command, cwd=build, check=True)
        subprocess.run([str(executable)], check=True)
