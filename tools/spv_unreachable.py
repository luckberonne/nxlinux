#!/usr/bin/env python3
"""Counts the translated shaders (.spv) that hold a block nobody jumps to: spv_unreachable.py DIR

A translator fault left the else branch of an `if` / `else if` chain unreachable (the code was there, no branch led to it), so the shader ran with its
default values. The fault is fixed in third_party/ryujinx (StructuredIr/AstOptimizer.cs); this finds it again in a library built with another translator.
"""
import sys, struct, glob, os
def scan(fn):
    d=open(fn,'rb').read(); w=struct.unpack('<%dI'%(len(d)//4),d)
    i=5; labels=[]; tgt=set(); firsts=set(); infn=False; first=False
    while i<len(w):
        op=w[i]&0xffff; n=w[i]>>16
        if op==54: first=True            # OpFunction
        elif op==248:                    # OpLabel
            labels.append(w[i+1])
            if first: firsts.add(w[i+1]); first=False
        elif op==249: tgt.add(w[i+1])    # OpBranch
        elif op==250: tgt.add(w[i+2]); tgt.add(w[i+3])   # OpBranchConditional
        elif op==251:                    # OpSwitch
            tgt.add(w[i+2]); 
            for k in range(i+3,i+n,2): tgt.add(w[k+1])
        elif op==246: tgt.add(w[i+1]); tgt.add(w[i+2])    # OpLoopMerge
        elif op==247: tgt.add(w[i+1])    # OpSelectionMerge
        i+=n
    return [l for l in labels if l not in tgt and l not in firsts]
files=sorted(glob.glob(sys.argv[1]+'/*.spv'))
bad=0
for f in files:
    u=scan(f)
    if u:
        bad+=1
        if bad<=8: print(os.path.basename(f), len(u))
print(len(files),'shaders',bad,'with blocks nobody jumps to')
