#!/usr/bin/env python3
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
        (app, 'void App::endGroupConfigNotifications('),
        (app, 'void App::notify('),
        (groups, 'void Groups::configure(')]:
        implementations.append(function(source, signature))
    harness = (here / 'harness.cpp.in').read_text().replace(
        '// EXTRACTED_IMPLEMENTATIONS', '\n\n'.join(implementations))
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

if __name__ == '__main__': main()
