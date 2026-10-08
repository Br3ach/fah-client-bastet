#!/usr/bin/env python3
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

"""Compile shipped transaction/notification bodies with real SQLite and adapters."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess, tempfile

def function(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 0; quote = None; escape = False
    for i in range(begin, len(source)):
        char = source[i]
        if quote:
            if escape: escape = False
            elif char == '\\': escape = True
            elif char == quote: quote = None
            continue
        if char in '\"\'': quote = char; continue
        if char == '{': depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0: return source[start:i+1]
    raise RuntimeError('Unterminated function ' + signature)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cbang', required=True, type=Path)
    parser.add_argument('--cxx', default='cl' if os.name == 'nt' else 'g++')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    pinned = json.loads((here / 'CBANG-SOURCES.json').read_text())
    sources = {}
    for rel, expected in pinned.items():
        data = (args.cbang / rel).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(data).hexdigest() != expected:
            raise RuntimeError('Pinned cbang source mismatch: ' + rel)
        if rel.endswith('.cpp'): sources[rel] = data.decode()
    db = sources['src/cbang/db/Database.cpp']
    tx = sources['src/cbang/db/Transaction.cpp']
    groups = (root / 'src/fah/client/Groups.cpp').read_text()
    app = (root / 'src/fah/client/App.cpp').read_text()
    implementations = []
    for source, signature in [
        (db, 'SmartPointer<Transaction> Database::begin('),
        (db, 'void Database::commit()'), (db, 'void Database::rollback()'),
        (tx, 'Transaction::~Transaction()'), (tx, 'void Transaction::commit()'),
        (app, 'void App::beginGroupConfigNotifications()'),
        (app, 'void App::reconcileSavedConfiguration()'),
        (app, 'void App::endGroupConfigNotifications('),
        (app, 'void App::notify('),
        (groups, 'void Groups::configure(')]:
        implementations.append(function(source, signature))
    harness = (here / 'harness.cpp.in').read_text().replace(
        '// EXTRACTED_IMPLEMENTATIONS', '\n\n'.join(implementations))
    guard_start = groups.index('  struct ConfigureGuard {')
    guard = groups[guard_start:groups.index('  } guard{', guard_start)] + '  };'
    guard_harness = r"""
#include <cassert>
#include <stdexcept>
using namespace std;
struct CbangError:runtime_error {using runtime_error::runtime_error;};
#ifdef DEBUG
#define TRY_CATCH_ERROR(expr) do {try {expr;} catch(const CbangError&) {}} while(false)
#else
#define TRY_CATCH_ERROR(expr) do {try {expr;} catch(...) {}} while(false)
#endif
bool failLogging=false;unsigned logs=0;
#define CBANG_CATCH_ALL(level,msg) catch(...) {++logs;if(failLogging)throw runtime_error("logging");}
struct App {
 unsigned calls=0;int failure=0;
 void endGroupConfigNotifications(bool publish){assert(publish);++calls;
  if(failure==1)throw runtime_error("publication");if(failure==2)throw 42;}
};
int main(){
 for(int failure:{0,1,2}) for(bool logFailure:{false,true}) {
  App app;app.failure=failure;bool flag=true;logs=0;failLogging=logFailure;
""" + guard + r"""
  {ConfigureGuard guard{flag,app};}
  assert(!flag&&app.calls==1&&logs==(failure?1u:0u));
 }
}
"""
    sqlite = args.cbang.resolve() / 'src/sqlite3'
    with tempfile.TemporaryDirectory(prefix='fah-transaction-qa-') as directory:
        build = Path(directory)
        cpp = build / 'harness.cpp'; cpp.write_bytes(harness.encode())
        if os.name == 'nt':
            subprocess.run([args.cxx, '/nologo', '/TC', '/D_CRT_SECURE_NO_WARNINGS',
                '/DSQLITE_THREADSAFE=0', '/DSQLITE_OMIT_LOAD_EXTENSION', '/c',
                str(sqlite / 'sqlite3.c'), '/Fosqlite3.obj'], cwd=build, check=True)
            subprocess.run([args.cxx, '/nologo', '/EHsc', '/std:c++17',
                '/I'+str(sqlite), str(cpp), 'sqlite3.obj', '/Feharness.exe'], cwd=build, check=True)
            executable = build / 'harness.exe'
        else:
            subprocess.run([os.environ.get('CC','cc'), '-DSQLITE_THREADSAFE=0',
                '-DSQLITE_OMIT_LOAD_EXTENSION', '-c', str(sqlite / 'sqlite3.c'),
                '-o', 'sqlite3.o'], cwd=build, check=True)
            subprocess.run([args.cxx, '-std=c++17', '-I'+str(sqlite), str(cpp),
                'sqlite3.o', '-lm', '-o', 'harness'], cwd=build, check=True)
            executable = build / 'harness'
        subprocess.run([str(executable)], cwd=build, check=True)
        guard_cpp = build / 'guard.cpp'; guard_cpp.write_text(guard_harness)
        for debug in (False, True):
            if os.name == 'nt':
                command = [args.cxx, '/nologo', '/EHsc', '/std:c++17',
                    str(guard_cpp), '/Feguard.exe']
                if debug: command.insert(1, '/DDEBUG')
                guard_exe = build / 'guard.exe'
            else:
                command = [args.cxx, '-std=c++17', str(guard_cpp), '-o', str(build / 'guard')]
                if debug: command.insert(1, '-DDEBUG')
                guard_exe = build / 'guard'
            subprocess.run(command, cwd=build, check=True)
            subprocess.run([str(guard_exe)], cwd=build, check=True)
        print('PASS: production notification guard contains publication and logging failures in release and DEBUG')


if __name__ == '__main__': main()
