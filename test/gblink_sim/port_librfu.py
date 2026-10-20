#!/usr/bin/env python3
"""port_librfu.py -- copy pret's librfu into build/ with the few mechanical
edits it needs to run on a 64-bit host against the GBA model in sim_rfu.c.

    port_librfu.py SRC_DIR OUT_DIR

Each edit names what it changes and must match exactly as many times as it
says, so if pret's source moves, the build stops here instead of producing a
library that behaves differently. Nothing else is changed: every line that
talks to the adapter is pret's.

Why each edit exists:

  trampolines   Callback_Dummy_{M,S,ID} are naked `bx rN` (ARM assembly) that
                jump to a callback; on the host they become the call itself.
  error arg     callbackM is variadic; GBA C passes the volatile error field
                by value, and C++ (which the register proxies need, see
                gba_shim/global.h) cannot pass a volatile class through `...`.
  api buffer    rfu_initializeAPI carves its state out of the caller's buffer
                at the GBA's 32-bit struct offsets. Host structs hold 8-byte
                pointers, so they are carved by size instead, in the same
                order and aligned.
  fast copy     rfu_initializeAPI copies rfu_STC_fastCopy's Thumb code into
                RAM and calls the copy; on the host it calls the function.
"""
import os
import re
import sys

EDITS = {
    'librfu_intr.c': [
        ('trampoline M', 1, r'asm\("bx r2"\);', 'callbackM(reqCommandId, error);'),
        ('trampoline S', 1, r'asm\("bx r1"\);', 'callbackS(reqCommandId);'),
        ('trampoline ID', 1, r'asm\("bx r0"\);', 'callbackId();'),
    ],
    'librfu_stwi.c': [
        ('error arg', 5, r'(callbackM\([^;]*?)gSTWIStatus->error', r'\1(int)gSTWIStatus->error'),
    ],
    'librfu_sio32id.c': [],
    'librfu_rfu.c': [
        ('api buffer', 1,
         r'    gRfuLinkStatus = \(void \*\)APIBuffer \+ 0;.*?\n'
         r'    gRfuStatic = \(void \*\)APIBuffer \+ 0xb4;.*?\n'
         r'    gRfuFixed = \(void \*\)APIBuffer \+ 0xdc;.*?\n'
         r'    gRfuSlotStatusNI\[0\] = \(void \*\)APIBuffer \+ 0x1bc;.*?\n'
         r'    gRfuSlotStatusUNI\[0\] = \(void \*\)APIBuffer \+ 0x37c;.*?\n',
         '    /* host: carved by size (port_librfu.py, "api buffer") */\n'
         '    gRfuLinkStatus = (struct RfuLinkStatus *)APIBuffer;\n'
         '    gRfuStatic = (struct RfuStatic *)(((uintptr_t)(gRfuLinkStatus + 1) + 7) & ~(uintptr_t)7);\n'
         '    gRfuFixed = (struct RfuFixed *)(((uintptr_t)(gRfuStatic + 1) + 7) & ~(uintptr_t)7);\n'
         '    gRfuSlotStatusNI[0] = (struct RfuSlotStatusNI *)(((uintptr_t)(gRfuFixed + 1) + 7) & ~(uintptr_t)7);\n'
         '    gRfuSlotStatusUNI[0] = (struct RfuSlotStatusUNI *)(((uintptr_t)(gRfuSlotStatusNI[0] + RFU_CHILD_MAX) + 7) & ~(uintptr_t)7);\n'),
        ('fast copy', 1,
         r'    // rfu_REQ_changeMasterSlave is the function next to rfu_STC_fastCopy\n'
         r'#if LIBRFU_VERSION < 1026\n.*?#endif\n'
         r'    gRfuFixed->fastCopyPtr = \(void \*\)gRfuFixed->fastCopyBuffer \+ 1;\n',
         '    /* host: call it in place (port_librfu.py, "fast copy") */\n'
         '    gRfuFixed->fastCopyPtr = rfu_STC_fastCopy;\n'),
    ],
}


def port(src_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for name, edits in EDITS.items():
        path = os.path.join(src_dir, name)
        if not os.path.exists(path):
            continue
        text = open(path, encoding='latin-1').read()
        for what, count, pattern, repl in edits:
            text, n = re.subn(pattern, repl, text, flags=re.S)
            if n != count:
                sys.exit('port_librfu: %s: edit "%s" matched %d times, expected %d '
                         '(pret source changed?)' % (name, what, n, count))
        out = os.path.join(out_dir, name[:-2] + '.cpp')
        with open(out, 'w', encoding='latin-1') as f:
            f.write(text)


if __name__ == '__main__':
    port(sys.argv[1], sys.argv[2])
