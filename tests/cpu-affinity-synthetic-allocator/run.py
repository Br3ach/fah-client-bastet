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
import os,shlex,shutil,subprocess,tempfile

here=Path(__file__).resolve().parent
root=here.parents[1]
files=['CPUResources.cpp','CPUResources.h','StrictAffinitySupport.h','CPUSetUtils.h',
       'CPUExecutionPlan.cpp','CPUWholeCorePacking.cpp','CPUWholeCorePacking.h','CPUExecutionPlan.h','CPUAllocationPlanner.cpp','CPUAllocationPlanner.h','WUCPUAllocationPlanner.cpp','WUCPUAllocationPlanner.h',
       'CPUAllocationSlice.h','CPUTypes.h']
planning=['CPUAllocationPlanner.cpp','CPUExecutionPlan.cpp','CPUWholeCorePacking.cpp']
allocation=['CPUResources.cpp']+planning
cases=[('test.cpp',allocation,True,False),
       ('gpu-reservation.cpp',allocation,True,False),
       ('process-policy.cpp',allocation,True,False),
       ('planner.cpp',planning,False,True),
       ('publication.cpp',allocation,True,True),
       ('classification.cpp',allocation,True,True),
       ('wu-planner.cpp',planning+['WUCPUAllocationPlanner.cpp'],False,True),
       ('class-planner.cpp',planning+['WUCPUAllocationPlanner.cpp'],False,True)]

with tempfile.TemporaryDirectory(prefix='fah-allocator-') as directory:
    build=Path(directory)
    for name in files: shutil.copyfile(root/'src/fah/client'/name,build/name)
    shutil.copyfile(here/'FixtureCPUResources.h',build/'FixtureCPUResources.h')
    windows=os.name=='nt'
    compiler=shlex.split(os.environ.get('CXX','cl' if windows else 'g++'))
    includes=['/I'+str(build)] if windows else ['-I'+str(build)]
    stub=['/I'+str(here/'stub')] if windows else ['-I'+str(here/'stub')]
    flags=['/nologo','/EHsc','/std:c++17'] if windows else ['-std=c++17','-Wall','-Wextra','-pedantic']
    def compile(sources,extra):
        subprocess.run(compiler+flags+includes+sources+extra,cwd=build,check=True)
    for unsupported in [False,True]:
        define=(['/DTEST_UNSUPPORTED_PLATFORM'] if windows else ['-DTEST_UNSUPPORTED_PLATFORM']) if unsupported else []
        check=['/c','/Fo'+str(build/('unsupported.obj' if unsupported else 'native.obj'))] if windows else ['-fsyntax-only']
        compile([str(here/'platform-support.cpp')],define+check)
    if not windows:
        compile([str(build/'CPUResources.cpp'),str(build/'CPUAllocationPlanner.cpp')],stub+['-Werror','-fsyntax-only'])
    for fixture,sources,with_stub,werror in cases:
        exe=build/(Path(fixture).stem+('-test.exe' if windows else '-test'))
        extra=list(stub) if with_stub else []
        if not windows and werror: extra+=['-Werror']
        extra+=['/Fe'+str(exe)] if windows else ['-o',str(exe)]
        compile([str(build/name) for name in sources]+[str(here/fixture)],extra)
        subprocess.run([str(exe)],cwd=build,check=True)
