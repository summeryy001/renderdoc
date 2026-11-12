if not pynewman.IsCaptureLoaded():
    filename = pynewman.Extensions().OpenFileName("Choose a capture", "", "*.rdc")

    pynewman.LoadCapture(filename, newman.ReplayOptions(), filename, False, True)

eid = pynewman.CurEvent()

name = pynewman.GetEventBrowser().GetEventName(eid)

print(f"Currently we are at EID {eid} named: '{name}'")
if eid > 1:
    prevname = pynewman.GetEventBrowser().GetEventName(eid - 1)
    print(f"    > the previous event {eid-1} is named: '{prevname}'")

pipe = pynewman.CurPipelineState()

outputs = pipe.GetOutputTargets()

for idx, out in enumerate(outputs):
    if out.resource != newman.ResourceId.Null():
        name = pynewman.GetResourceName(out.resource)
        print(f"Output {idx} is: {name}")

depth = pipe.GetDepthTarget()

name = pynewman.GetResourceName(depth.resource)
print(f"Depth is: {name}")