rem /******************************************************************************\
rem
rem                   This file is part of the Folding@home Client.
rem
rem           The fah-client runs Folding@home protein folding simulations.
rem                     Copyright (c) 2001-2026, foldingathome.org
rem                                All rights reserved.
rem
rem        This program is free software; you can redistribute it and/or modify
rem        it under the terms of the GNU General Public License as published by
rem         the Free Software Foundation; either version 3 of the License, or
rem                        (at your option) any later version.
rem
rem          This program is distributed in the hope that it will be useful,
rem           but WITHOUT ANY WARRANTY; without even the implied warranty of
rem           MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
rem                    GNU General Public License for more details.
rem
rem      You should have received a copy of the GNU General Public License along
rem      with this program; if not, write to the Free Software Foundation, Inc.,
rem            51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
rem
rem                   For information regarding this software email:
rem                                  Joseph Coffland
rem                           joseph@cauldrondevelopment.com
rem
rem \******************************************************************************/

@echo off
py "%~dp0run.py" %*
exit /b %errorlevel%
