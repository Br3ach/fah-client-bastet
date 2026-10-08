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
here = Path(__file__).resolve().parent
root = here.parents[1]
with tempfile.TemporaryDirectory(prefix='fah-general-equivalence-') as directory:
    build = Path(directory)
    sources = [here/'compare.cpp', here/'ReferenceExecutionPlan.cpp', here/'ReferenceWUPlanner.cpp', here/'ReferenceRGPlanner.cpp']
    sources += [root/'src/fah/client'/name for name in
        ['CPUExecutionPlan.cpp', 'CPUWholeCorePacking.cpp', 'WUCPUAllocationPlanner.cpp', 'CPUAllocationPlanner.cpp']]
    includes = [root/'src', root/'src/fah/client', here]
    if os.name == 'nt':
        exe = build/'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/std:c++17', '/O2']
        command += ['/I'+str(path) for path in includes]
        command += [str(path) for path in sources]+['/Fe'+str(exe)]
    else:
        exe = build/'test'
        command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-Wall', '-Wextra']
        command += ['-I'+str(path) for path in includes]
        command += [str(path) for path in sources]+['-o', str(exe)]
    subprocess.run(command, cwd=build, check=True)
    subprocess.run([str(exe)], cwd=build, check=True)
