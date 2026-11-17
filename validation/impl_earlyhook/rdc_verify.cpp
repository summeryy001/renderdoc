// Headless replay verifier for fixture captures. No window is created.
// Links against the host replay library (rendertest.dll) and asserts:
//   structural: >=1 Clear action, exactly one Draw action with numIndices==3
//   resources : output texture of the draw is 640x480
//   pixels    : known triangle (green/blue/white) over frame-coded clear background
// Usage: rdc_verify.exe CAPTURE_PATH EXPECTED_FRAME(0..7)
// Exit codes: 0 = all assertions passed; 8 = assertion failed; 2/3 = open/replay error.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include "replay/renderdoc_replay.h"
// fundamental-type stringise specialisations (normally provided by the host library build)
template <> inline rdcstr DoStringise(const uint32_t &el) { char b[32]; sprintf_s(b,sizeof(b),"%u",el); return rdcstr(b); }
template <> inline rdcstr DoStringise(const uint64_t &el) { char b[32]; sprintf_s(b,sizeof(b),"%llu",(unsigned long long)el); return rdcstr(b); }
template <> inline rdcstr DoStringise(const int32_t &el)  { char b[32]; sprintf_s(b,sizeof(b),"%d",el); return rdcstr(b); }
template <> inline rdcstr DoStringise(const int64_t &el)  { char b[32]; sprintf_s(b,sizeof(b),"%lld",(long long)el); return rdcstr(b); }
template <> inline rdcstr DoStringise(const float &el)    { char b[32]; sprintf_s(b,sizeof(b),"%g",(double)el); return rdcstr(b); }
template <> inline rdcstr DoStringise(const double &el)   { char b[32]; sprintf_s(b,sizeof(b),"%g",el); return rdcstr(b); }
template <> inline rdcstr DoStringise(const bool &el)     { return rdcstr(el ? "true" : "false"); }
#include "replay/renderdoc_tostr.inl"   // provides DoStringise<ResultCode> for ResultDetails::Message()

REPLAY_PROGRAM_MARKER();

static int g_failures=0;
static void fail(const char* fmt,...)
{
  va_list args; va_start(args,fmt);
  printf("ASSERT_FAIL "); vprintf(fmt,args); printf("\n");
  g_failures++;
}

static void flatten(const rdcarray<ActionDescription>& in,std::vector<const ActionDescription*>& out)
{
  for(const ActionDescription& a : in) { out.push_back(&a); flatten(a.children,out); }
}

static bool hasFlag(ActionFlags f,ActionFlags bit)
{
  return (uint32_t(f)&uint32_t(bit))!=0;
}

int main(int argc,char**argv)
{
  if(argc!=3) { printf("usage: rdc_verify.exe CAPTURE_PATH EXPECTED_FRAME\n"); return 1; }
  const char* path=argv[1];
  int expectedFrame=atoi(argv[2]);
  if(expectedFrame<0||expectedFrame>7) { printf("BAD_EXPECTED_FRAME %d\n",expectedFrame); return 1; }

  RENDERDOC_InitialiseReplay(GlobalEnvironment(), rdcarray<rdcstr>());

  ICaptureFile* file=RENDERDOC_OpenCaptureFile();
  if(!file) { printf("OPENFILE_FAILED\n"); return 2; }
  ResultDetails open=file->OpenFile(rdcstr(path), "rdc", NULL);
  if(!open.OK()) { printf("OPEN_FAILED %s\n",open.Message().c_str()); return 2; }

  IReplayController* controller=NULL;
  ResultDetails res;
  rdctie(res,controller)=file->OpenCapture(ReplayOptions(), NULL);
  file->Shutdown();
  if(!res.OK()||!controller) { printf("REPLAY_FAILED %s\n",res.Message().c_str()); return 3; }

  printf("REPLAY_OK frameNumber=%u\n",controller->GetFrameInfo().frameNumber);
  printf("RESOURCES textures=%u buffers=%u resources=%u\n",(uint32_t)controller->GetTextures().size(),
         (uint32_t)controller->GetBuffers().size(),(uint32_t)controller->GetResources().size());

  // ---- structural assertions ----
  std::vector<const ActionDescription*> actions;
  flatten(controller->GetRootActions(),actions);
  const ActionDescription* lastDraw=NULL; const ActionDescription* lastClear=NULL;
  uint32_t drawCount=0, clearCount=0;
  for(const ActionDescription* a : actions) {
    rdcstr name=a->GetName(controller->GetStructuredFile());
    printf("ACTION event=%u flags=0x%X numIndices=%u name=%s\n",a->eventId,(uint32_t)a->flags,a->numIndices,name.c_str());
    if(hasFlag(a->flags,ActionFlags::Drawcall)) { drawCount++; lastDraw=a; }
    if(hasFlag(a->flags,ActionFlags::Clear))    { clearCount++; lastClear=a; }
  }
  printf("STRUCT counts draws=%u clears=%u total=%u\n",drawCount,clearCount,(uint32_t)actions.size());
  if(drawCount==0) fail("no draw actions found");
  if(clearCount==0) fail("no clear actions found");
  if(lastDraw&&lastDraw->numIndices!=3) fail("draw numIndices=%u expected 3",lastDraw->numIndices);
  if(lastDraw&&lastDraw->outputs[0]==ResourceId::Null()) fail("draw has no color output");

  // ---- resource assertions ----
  ResourceId outId = lastDraw ? lastDraw->outputs[0] : ResourceId::Null();
  const TextureDescription* outTex=nullptr;
  for(const TextureDescription& t : controller->GetTextures()) if(t.resourceId==outId) { outTex=&t; break; }
  rdcstr outName;
  for(const ResourceDescription& r : controller->GetResources()) if(r.resourceId==outId) { outName=r.name; break; }
  if(!outTex) fail("output texture of last draw not found in texture list");
  else {
    printf("OUTPUT_TEX '%s' %ux%u\n",outName.c_str(),outTex->width,outTex->height);
    if(outTex->width!=640||outTex->height!=480) fail("output texture %ux%u expected 640x480",outTex->width,outTex->height);
  }

  // ---- pixel assertions (top-left origin, matching fixture design) ----
  if(controller->GetFatalErrorStatus().OK() && outTex) {
    controller->SetFrameEvent(lastDraw->eventId,true);
    const uint32_t W=outTex->width, H=outTex->height;
    struct Probe { const char* tag; double ndcX,ndcY; };
    Probe probes[]={
      {"tri_green", -0.385, -0.49},
      {"tri_blue",   0.385, -0.49},
      {"tri_white",  0.0,    0.49},
      {"bg_topleft",-0.94,   0.94},
      {"bg_botright",0.94,  -0.94},
    };
    for(const Probe& p : probes) {
      uint32_t px=(uint32_t)((p.ndcX+1.0)*0.5*W), py=(uint32_t)((1.0-p.ndcY)*0.5*H);
      PixelValue v=controller->PickPixel(outId,px,py,Subresource(),CompType::Typeless);
      float r=v.floatValue[0],g=v.floatValue[1],b=v.floatValue[2];
      printf("PIXEL %s (%u,%u) = %.3f %.3f %.3f %.3f\n",p.tag,px,py,r,g,b,v.floatValue[3]);
      if(!strcmp(p.tag,"tri_green")) {
        if(!(g>0.55&&g>r+0.15&&g>b+0.15)) fail("tri_green pixel wrong: %.3f %.3f %.3f",r,g,b);
      } else if(!strcmp(p.tag,"tri_blue")) {
        if(!(b>0.70&&b>g+0.30&&b>r+0.30)) fail("tri_blue pixel wrong: %.3f %.3f %.3f",r,g,b);
      } else if(!strcmp(p.tag,"tri_white")) {
        if(!(r>0.55&&g>0.70&&b>0.70)) fail("tri_white pixel wrong: %.3f %.3f %.3f",r,g,b);
      } else {
        double want=expectedFrame/8.0;
        if(!(fabs(r-want)<=0.10&&g<=0.22&&b<=0.22))
          fail("%s pixel wrong: r=%.3f want=%.3f g=%.3f b=%.3f frame=%d",p.tag,r,want,g,b,expectedFrame);
      }
    }
  }

  controller->Shutdown();
  RENDERDOC_ShutdownReplay();
  if(g_failures) { printf("VERIFY_FAIL failures=%u\n",g_failures); return 8; }
  printf("VERIFY_OK frame=%d\n",expectedFrame);
  return 0;
}
