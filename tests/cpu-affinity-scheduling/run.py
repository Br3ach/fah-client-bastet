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
import subprocess, sys
from compile_harness import compile_and_run
here=Path(__file__).resolve().parent
root=here.parents[1]
g=(root/'src/fah/client/Group.cpp').read_text()
a=g.index('  // Allocate GPUs with minimum CPU requirements');b=g.index('  // Allocate remaining CPUs to existing CPU WUs',a)
s=(here/'harness.cpp.in').read_text().replace('// GPU_SCHEDULING_IMPLEMENTATION',g[a:b])
compile_and_run(s, ['CPUOwnershipPolicy.cpp'])

for name in (
    'gpu-affinity.py',
    'config-update.py',
    'gpu-validation.py',
    'launch-recovery.py',
    'pool-scheduling.py',
    'topology-watcher.py',
    'wu-metadata.py',
    'assignment-reconciliation.py',
    'windows-startup.py',
    'config-validation.py',
    'core-launch.py',
    'gpu-priority.py',
    'runtime-demand.py',
):
    subprocess.run([sys.executable, str(here/name)], check=True)
