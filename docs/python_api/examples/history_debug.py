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

from typing import List


# a callback to repeatedly ask the user if they're ready, with a 5 second
# arbitrary wait each time they say no to give them a bit of time
def check_ready():
    choice = pynewman.Extensions().QuestionDialog(
        "Are you at an interesting event with pixel selected?",
        [
            qnewman.DialogButton.Yes,
            qnewman.DialogButton.No,
            qnewman.DialogButton.Cancel,
        ],
        "Ready?",
    )

    if choice == qnewman.DialogButton.Cancel:
        return

    if choice == qnewman.DialogButton.No:
        pynewman.DelayedCallback(5000, check_ready)
        return

    prepare_history()


def prepare_history():
    tex_view = pynewman.GetTextureViewer()

    # find the selected texture and location
    id = tex_view.GetCurrentResource()
    sub = tex_view.GetSelectedSubresource()
    x, y = tex_view.GetPickedLocation()

    name = pynewman.GetResourceName(id)

    print(f'Analysing on "{name}"@{newman.DumpObject(sub)} at {x},{y}')

    disp = newman.TextureDisplay()
    disp.subresource = sub

    # show the window first, as the results will likely take some time to come back
    history_window = pynewman.ViewPixelHistory(id, x, y, sub.slice, disp)
    pynewman.AddDockWindow(
        history_window.Widget(),
        qnewman.DockReference.RightOf,
        pynewman.GetPythonShell().Widget(),
    )

    # get a blocking controller. This means long-running work like pixel history and
    # shader debugging will block the running thread. In a UI extension this is not
    # good but for python scripts it will block the script thread.
    controller = pynewman.GetBlockingController()

    history = controller.PixelHistory(id, x, y, sub, disp.typeCast)

    history_window.SetHistory(history)

    print(f"{len(history)} modifications to that pixel:")

    for h in history:
        col = lambda x: [int(c * 100.0) / 100.0 for c in x.col.floatValue]

        if h.Passed():
            print(
                f"  at EID {h.eventId} changed from {col(h.preMod)} to {col(h.postMod)}"
            )
        else:
            print(f"  at EID {h.eventId} modification failed")

    # get a list of all drawcalls that passed with a pixel shader bound
    eb = pynewman.GetEventBrowser()

    passed_draws = list(
        filter(
            lambda x: eb.GetActionForEID(x.eventId).flags
            & newman.ActionFlags.Drawcall,
            [h for h in history if h.Passed() and not h.unboundPS],
        )
    )

    if len(passed_draws) == 0:
        print("No draws wrote to this pixel with a pixel shader")
    else:
        p = passed_draws[0]

        pynewman.SetEventID([], p.eventId, p.eventId, False)

        pipe = pynewman.CurPipelineState()

        refl = pipe.GetShaderReflection(newman.ShaderStage.Pixel)

        if refl is None or not refl.debugInfo.debuggable:
            print("Shader can't be debugged:")
            print(refl.debugInfo.debugStatus)
            return

        inputs = newman.DebugPixelInputs()

        inputs.primitive = p.primitiveID
        inputs.sample = newman.ReplayController.NoPreference
        inputs.view = newman.ReplayController.NoPreference

        trace = controller.DebugPixel(x, y, inputs)

        if trace.debugger is None:
            print("Debug failed :(")
            controller.FreeTrace(trace)
            return

        # the trace holds static global information about this debugged instance,
        # such as the information of which resources are bound or reflected
        # information per-instruction

        states: List[newman.ShaderDebugState] = []

        # continually simulate the shader until it completes
        while True:
            more = controller.ContinueDebug(trace.debugger)
            if more == []:
                break
            states += more

        # look at the mid-point state
        if states == []:
            print("Shader debug failed!")
        else:
            state = states[len(states) // 2]
            print(
                f"Examining step {state.stepIndex}, before instruction {state.nextInstruction}"
            )
            stack = "\n".join(state.callstack)
            print(f"Callstack:\n{stack}")

            print()

            print(f"{len(state.changes)} debug variable changes")
            for ch in state.changes:
                print(f" '{ch.before.name}' -> '{ch.after.name}'")

            print()

            infos = [
                i for i in trace.instInfo if i.instruction <= state.nextInstruction
            ]

            if infos == []:
                info = trace.instInfo[0]
            else:
                info = infos[-1]

            disasm = controller.DisassembleShader(
                pipe.GetGraphicsPipelineObject(),
                refl,
                "",
            )

            disline = disasm.splitlines()[info.lineInfo.disassemblyLine - 1]

            srcline = ""
            if info.lineInfo.fileIndex >= 0:
                src = refl.debugInfo.files[info.lineInfo.fileIndex].contents
                srcline = src.splitlines()[info.lineInfo.lineStart - 1]

            print(f"Examining instruction {info.instruction}.")
            print(f"  which has {len(info.sourceVars)} source vars:")
            if srcline != "":
                print(srcline)
            print(disline)
            print()
            for s in info.sourceVars:
                debugVars = ", ".join(
                    [v.name + "." + ("xyzw"[v.component % 4]) for v in s.variables]
                )
                print(f"   {s.name} is {str(s.type)} stored in: {debugVars}")

        # if we want to display the shader debugger, we need to pass a new debugger
        # as the UI wants to process the set of states itself
        trace = controller.DebugPixel(x, y, inputs)
        shad = pynewman.DebugShader(
            refl,
            pipe.GetGraphicsPipelineObject(),
            trace,
            "Debugged From Python",
        )

        pynewman.AddDockWindow(
            shad.Widget(),
            qnewman.DockReference.BottomOf,
            history_window.Widget(),
        )


# start by calling our function that checks if the user is ready to debug
check_ready()
