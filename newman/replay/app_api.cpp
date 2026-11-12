/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <string.h>
#include "api/app/newman_app.h"
#include "api/replay/apidefs.h"    // for NEWMAN_API to export the NEWMAN_GetAPI function
#include "common/common.h"
#include "common/formatting.h"
#include "core/core.h"
#include "hooks/hooks.h"
#include "serialise/rdcfile.h"

static void SetFocusToggleKeys(NEWMAN_InputButton *keys, int num)
{
  NewMan::Inst().SetFocusKeys(keys, num);
}

static void SetCaptureKeys(NEWMAN_InputButton *keys, int num)
{
  NewMan::Inst().SetCaptureKeys(keys, num);
}

static uint32_t GetOverlayBits()
{
  return NewMan::Inst().GetOverlayBits();
}

static void MaskOverlayBits(uint32_t And, uint32_t Or)
{
  NewMan::Inst().MaskOverlayBits(And, Or);
}

static void RemoveHooks()
{
  NewMan::Inst().RemoveHooks();
  LibraryHooks::RemoveHooks();
}

static void UnloadCrashHandler()
{
  NewMan::Inst().UnloadCrashHandler();
}

static void SetCaptureFilePathTemplate(const char *pathtemplate)
{
  RDCLOG("Using capture file template %s", pathtemplate);
  NewMan::Inst().SetCaptureFileTemplate(pathtemplate);
}

static const char *GetCaptureFilePathTemplate()
{
  return NewMan::Inst().GetCaptureFileTemplate();
}

static uint32_t GetNumCaptures()
{
  return (uint32_t)NewMan::Inst().GetCaptures().size();
}

static uint32_t GetCapture(uint32_t idx, char *filename, uint32_t *pathlength, uint64_t *timestamp)
{
  rdcarray<CaptureData> caps = NewMan::Inst().GetCaptures();

  if(idx >= (uint32_t)caps.size())
  {
    if(filename)
      filename[0] = 0;
    if(pathlength)
      *pathlength = 0;
    if(timestamp)
      *timestamp = 0;
    return 0;
  }

  CaptureData &c = caps[idx];

  if(filename)
    memcpy(filename, c.path.c_str(), sizeof(char) * (c.path.size() + 1));
  if(pathlength)
    *pathlength = uint32_t(c.path.size() + 1);
  if(timestamp)
    *timestamp = c.timestamp;

  return 1;
}

static void SetCaptureFileComments(const char *filePath, const char *comments)
{
  rdcstr path;
  if(filePath == NULL || filePath[0] == 0)
  {
    rdcarray<CaptureData> caps = NewMan::Inst().GetCaptures();
    if(caps.empty())
    {
      RDCERR(
          "SetCaptureFileComments called with NULL/empty filePath, but no captures have been made");
      return;
    }

    path = caps.back().path;
  }
  else
  {
    path = filePath;
  }

  RDCFile rdc;
  rdc.Open(path);
  if(rdc.Error() != ResultCode::Succeeded)
  {
    RDCERR("Error adding capture file comments: %s", ResultDetails(rdc.Error()).Message().c_str());
    return;
  }

  SectionProperties props;
  props.type = SectionType::Notes;
  props.version = 1;

  StreamWriter *writer = rdc.WriteSection(props);

  if(comments)
  {
    rdcstr commentsjson = "{\"comments\":\"";

    commentsjson.reserve(strlen(comments));

    const char *c = comments;

    while(*c)
    {
      // escape some characters
      if(*c == '"')
        commentsjson += "\\\"";
      else if(*c == '\\')
        commentsjson += "\\\\";
      else if(*c == '\b')
        commentsjson += "\\b";
      else if(*c == '\f')
        commentsjson += "\\f";
      else if(*c == '\n')
        commentsjson += "\\n";
      else if(*c == '\r')
        commentsjson += "\\r";
      else if(*c == '\t')
        commentsjson += "\\t";
      else
        commentsjson.push_back(*c);

      c++;
    }

    commentsjson += "\"}";

    writer->Write(commentsjson.c_str(), commentsjson.size());
  }

  delete writer;
}

static void TriggerCapture()
{
  NewMan::Inst().TriggerCapture(1);
}

static void TriggerMultiFrameCapture(uint32_t numFrames)
{
  NewMan::Inst().TriggerCapture(numFrames);
}

static uint32_t IsTargetControlConnected()
{
  return NewMan::Inst().IsTargetControlConnected();
}

static uint32_t LaunchReplayUI(uint32_t connectTargetControl, const char *cmdline)
{
  rdcstr replayapp = FileIO::GetReplayAppFilename();

  if(replayapp.empty())
    return 0;

  rdcstr cmd = cmdline ? cmdline : "";
  if(connectTargetControl)
    cmd += StringFormat::Fmt(" --targetcontrol localhost:%u",
                             NewMan::Inst().GetTargetControlIdent());

  return Process::LaunchProcess(replayapp, "", cmd, false);
}

static void SetActiveWindow(void *device, void *wndHandle)
{
  NewMan::Inst().SetActiveWindow(DeviceOwnedWindow(device, wndHandle));
}

static void StartFrameCapture(void *device, void *wndHandle)
{
  DeviceOwnedWindow devWnd(device, wndHandle);

  NewMan::Inst().StartFrameCapture(devWnd);

  if(devWnd.device == NULL || devWnd.windowHandle == NULL)
    NewMan::Inst().MatchClosestWindow(devWnd);

  if(devWnd.device != NULL && devWnd.windowHandle != NULL)
    NewMan::Inst().SetActiveWindow(devWnd);
}

static uint32_t IsFrameCapturing()
{
  return NewMan::Inst().IsFrameCapturing() ? 1 : 0;
}

static uint32_t EndFrameCapture(void *device, void *wndHandle)
{
  return NewMan::Inst().EndFrameCapture(DeviceOwnedWindow(device, wndHandle)) ? 1 : 0;
}

static void SetCaptureTitle(const char *title)
{
  NewMan::Inst().SetCaptureTitle(title);
}

static uint32_t DiscardFrameCapture(void *device, void *wndHandle)
{
  return NewMan::Inst().DiscardFrameCapture(DeviceOwnedWindow(device, wndHandle)) ? 1 : 0;
}

static uint32_t ShowReplayUI()
{
  return NewMan::Inst().ShowReplayUI() ? 1 : 0;
}

static uint32_t SetObjectAnnotation(void *device, void *object, const char *key,
                                    NEWMAN_AnnotationType valueType, uint32_t valueVectorWidth,
                                    const NEWMAN_AnnotationValue *value)
{
  if(object == NULL)
  {
    RDCWARN("Invalid annotation - object must not be NULL.");
    return 3;
  }

  if((valueType == eNEWMAN_Empty && value != NULL) ||
     (valueType != eNEWMAN_Empty && value == NULL))
  {
    RDCWARN("Invalid annotation - value should be NULL and type should be empty");
    return 3;
  }

  if((valueType == eNEWMAN_Empty || valueType == eNEWMAN_String ||
      valueType == eNEWMAN_APIObject) &&
     valueVectorWidth != 0)
  {
    RDCWARN(
        "Invalid annotation - for deletion, or setting strings and objects, vector width must be "
        "0");
    return 3;
  }

  if(key == NULL || key[0] == 0 || key[0] == '.')
  {
    RDCWARN("Invalid annotation - key should not be NULL, empty, or start with a .");
    return 3;
  }

  DeviceOwnedWindow devWnd(device, NULL);

  IFrameCapturer *capturer = NewMan::Inst().MatchFrameCapturer(devWnd);

  if(!capturer)
    return 1;

  return capturer->SetObjectAnnotation(object, key, valueType, valueVectorWidth, value);
}

static uint32_t SetCommandAnnotation(void *device, void *queueOrCommandBuffer, const char *key,
                                     NEWMAN_AnnotationType valueType, uint32_t valueVectorWidth,
                                     const NEWMAN_AnnotationValue *value)
{
  if((valueType == eNEWMAN_Empty && value != NULL) ||
     (valueType != eNEWMAN_Empty && value == NULL))
  {
    RDCWARN("Invalid annotation - value should be NULL and type should be empty");
    return 3;
  }

  if(key == NULL || key[0] == 0 || key[0] == '.')
  {
    RDCWARN("Invalid annotation - key should not be NULL, empty, or start with a .");
    return 3;
  }

  if((valueType == eNEWMAN_Empty || valueType == eNEWMAN_String ||
      valueType == eNEWMAN_APIObject) &&
     valueVectorWidth != 0)
  {
    RDCWARN(
        "Invalid annotation - for deletion, or setting strings and objects, vector width must be "
        "0");
    return 3;
  }

  DeviceOwnedWindow devWnd(device, NULL);

  IFrameCapturer *capturer = NewMan::Inst().MatchFrameCapturer(devWnd);

  if(!capturer)
    return 1;

  return capturer->SetCommandAnnotation(queueOrCommandBuffer, key, valueType, valueVectorWidth,
                                        value);
}

// defined in capture_options.cpp
int NEWMAN_CC SetCaptureOptionU32(NEWMAN_CaptureOption opt, uint32_t val);
int NEWMAN_CC SetCaptureOptionF32(NEWMAN_CaptureOption opt, float val);
uint32_t NEWMAN_CC GetCaptureOptionU32(NEWMAN_CaptureOption opt);
float NEWMAN_CC GetCaptureOptionF32(NEWMAN_CaptureOption opt);

void NEWMAN_CC GetAPIVersion_1_7_0(int *major, int *minor, int *patch)
{
  if(major)
    *major = 1;
  if(minor)
    *minor = 7;
  if(patch)
    *patch = 0;
}

NEWMAN_API_1_7_0 api_1_7_0;
void Init_1_7_0()
{
  NEWMAN_API_1_7_0 &api = api_1_7_0;

  api.GetAPIVersion = &GetAPIVersion_1_7_0;

  api.SetCaptureOptionU32 = &SetCaptureOptionU32;
  api.SetCaptureOptionF32 = &SetCaptureOptionF32;

  api.GetCaptureOptionU32 = &GetCaptureOptionU32;
  api.GetCaptureOptionF32 = &GetCaptureOptionF32;

  api.SetFocusToggleKeys = &SetFocusToggleKeys;
  api.SetCaptureKeys = &SetCaptureKeys;

  api.GetOverlayBits = &GetOverlayBits;
  api.MaskOverlayBits = &MaskOverlayBits;

  api.RemoveHooks = &RemoveHooks;
  api.UnloadCrashHandler = &UnloadCrashHandler;

  api.SetCaptureFilePathTemplate = &SetCaptureFilePathTemplate;
  api.GetCaptureFilePathTemplate = &GetCaptureFilePathTemplate;

  api.GetNumCaptures = &GetNumCaptures;
  api.GetCapture = &GetCapture;

  api.TriggerCapture = &TriggerCapture;

  api.IsTargetControlConnected = &IsTargetControlConnected;
  api.LaunchReplayUI = &LaunchReplayUI;

  api.SetActiveWindow = &SetActiveWindow;

  api.StartFrameCapture = &StartFrameCapture;
  api.IsFrameCapturing = &IsFrameCapturing;
  api.EndFrameCapture = &EndFrameCapture;

  api.TriggerMultiFrameCapture = &TriggerMultiFrameCapture;

  api.SetCaptureFileComments = &SetCaptureFileComments;

  api.DiscardFrameCapture = &DiscardFrameCapture;

  api.ShowReplayUI = &ShowReplayUI;

  api.SetCaptureTitle = &SetCaptureTitle;

  api.SetObjectAnnotation = &SetObjectAnnotation;
  api.SetCommandAnnotation = &SetCommandAnnotation;
}

extern "C" NEWMAN_API int NEWMAN_CC NEWMAN_GetAPI(NEWMAN_Version version,
                                                           void **outAPIPointers)
{
  if(outAPIPointers == NULL)
  {
    RDCERR("Invalid call to NEWMAN_GetAPI with NULL outAPIPointers");
    return 0;
  }

  int ret = 0;
  int major = 0, minor = 0, patch = 0;

  rdcstr supportedVersions = "";

#define API_VERSION_HANDLE(enumver, actualver)                     \
  supportedVersions += " " STRINGIZE(CONCAT(API_, enumver));       \
  if(version == CONCAT(eNEWMAN_API_Version_, enumver))          \
  {                                                                \
    CONCAT(Init_, actualver)();                                    \
    *outAPIPointers = &CONCAT(api_, actualver);                    \
    CONCAT(api_, actualver).GetAPIVersion(&major, &minor, &patch); \
    ret = 1;                                                       \
  }

  API_VERSION_HANDLE(1_0_0, 1_7_0);
  API_VERSION_HANDLE(1_0_1, 1_7_0);
  API_VERSION_HANDLE(1_0_2, 1_7_0);
  API_VERSION_HANDLE(1_1_0, 1_7_0);
  API_VERSION_HANDLE(1_1_1, 1_7_0);
  API_VERSION_HANDLE(1_1_2, 1_7_0);
  API_VERSION_HANDLE(1_2_0, 1_7_0);
  API_VERSION_HANDLE(1_3_0, 1_7_0);
  API_VERSION_HANDLE(1_4_0, 1_7_0);
  API_VERSION_HANDLE(1_4_1, 1_7_0);
  API_VERSION_HANDLE(1_4_2, 1_7_0);
  API_VERSION_HANDLE(1_5_0, 1_7_0);
  API_VERSION_HANDLE(1_6_0, 1_7_0);
  API_VERSION_HANDLE(1_7_0, 1_7_0);

#undef API_VERSION_HANDLE

  if(ret)
  {
    RDCLOG("Initialising NewMan API version %d.%d.%d for requested version %d", major, minor,
           patch, version);
    return 1;
  }

  RDCERR("Unrecognised API version '%d'. Supported versions:%s", version, supportedVersions.c_str());

  return 0;
}
