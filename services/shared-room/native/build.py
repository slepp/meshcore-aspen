#!/usr/bin/env python3
# Compile unchanged vendored native primitives using LLVM; no runtime/libc import.
import os, pathlib, subprocess, hashlib, json
root=pathlib.Path(__file__).resolve().parent
clang=os.environ.get('WASM_CC','clang')
build=root.parent / '.wrangler/native-build'
build.mkdir(parents=True,exist_ok=True)
common=['--target=wasm32','-Oz','-fno-builtin','-ffunction-sections','-fdata-sections','-nostdlib','-I'+str(root/'compat'),'-I'+str(root/'ed25519'),'-I'+str(root/'crypto')]
files=list(sorted((root/'ed25519').glob('*.c')))+list(sorted((root/'crypto').glob('*.cpp')))+[root/'adapter.cpp']
objects=[]
for i,f in enumerate(files):
    obj=build/f'{i}.o'
    flags=['-std=c++17','-fno-exceptions','-fno-rtti','-fno-threadsafe-statics'] if f.suffix=='.cpp' else ['-std=c99']
    subprocess.run([clang,*common,*flags,'-c',str(f),'-o',str(obj)],check=True)
    objects.append(str(obj))
out=root.parent/'src/native-crypto.wasm'
exports=['mc_arena','mc_pub','mc_seed','mc_shared','mc_sha','mc_sign','mc_verify','mc_crypt']
subprocess.run([clang,'--target=wasm32','-nostdlib',*objects,'-Wl,--no-entry','-Wl,--strip-all','-Wl,--initial-memory=131072','-Wl,--max-memory=131072',*['-Wl,--export='+e for e in exports],'-o',str(out)],check=True)
out.chmod(0o644)
print(f'{out.name}: {out.stat().st_size} bytes sha256={hashlib.sha256(out.read_bytes()).hexdigest()}')
