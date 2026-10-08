/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

       This program is free software; you can redistribute it and/or modify
       it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 3 of the License, or
                       (at your option) any later version.

         This program is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
          MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
                   GNU General Public License for more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
           51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#pragma once

namespace FAH {
  namespace Client {
    // Compile-time launcher availability only; keep in sync with execStrict.
    // Managed mode also requires readable availability and representable masks.
    // Windows uses one DWORD_PTR mask (IDs < its bit width: 64 on x64);
    // topology must disable managed mode on multi-processor-group systems.
    // Linux uses fixed cpu_set_t masks, so IDs must be < CPU_SETSIZE.
    // Strict launch applies and verifies the exact non-empty mask before the
    // core executes; rejection must never retry through unrestricted Subprocess.
    // macOS/BSD have no strict launcher: use legacy scheduling only. An explicit
    // strict-launch request there fails rather than silently ignoring affinity.
    constexpr bool supportsStrictAffinity() {
#if defined(_WIN32) || defined(__linux__)
      return true;
#else
      return false;
#endif
    }
  }
}
