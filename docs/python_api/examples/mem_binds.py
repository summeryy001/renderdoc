# these imports are not strictly necessary, but are convenient
import newman
import qnewman

# this is here to give autocomplete when editing the example
# in VS Code where it doesn't know about this global
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    pynewman = qnewman.CaptureContext()

if not pynewman.IsCaptureLoaded():
    filename = pynewman.Extensions().OpenFileName("Choose a capture", "", "*.rdc")

    pynewman.LoadCapture(filename, newman.ReplayOptions(), filename, False, True)

mems = {}

for t in pynewman.GetTextures():
    if t.memory == newman.ResourceId():
        continue

    entry = (
        t.memoryOffset,
        t.memoryOffset + t.byteSize,
        f"[Texture] {pynewman.GetResourceName(t.resourceId)}",
    )

    if t.memory not in mems:
        mems[t.memory] = []

    mems[t.memory].append(entry)

for b in pynewman.GetBuffers():
    if b.memory == newman.ResourceId():
        continue

    entry = (
        b.memoryOffset,
        b.memoryOffset + b.length,
        f"[Buffer]  {pynewman.GetResourceName(b.resourceId)}",
    )

    if b.memory not in mems:
        mems[b.memory] = []

    mems[b.memory].append(entry)

for m in mems:
    binds = mems[m]

    for idx, bind1 in enumerate(binds):
        for bind2 in binds[idx + 1 :]:
            if bind1[0] < bind2[0] and bind1[1] > bind2[0]:
                print(f"In memory {pynewman.GetResourceName(m)} overlap:")
                print(f"    {bind1[0]:08x} - {bind1[1]:08x}: {bind1[2]} ")
                print(f"    {bind2[0]:08x} - {bind2[1]:08x}: {bind2[2]}")
