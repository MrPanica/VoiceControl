#pragma once

#include <array>
#include <map>
#include <vector>
#include <convar.h>
#include <iclient.h>
#include <iserver.h>
#include <ISDKTools.h>
#include "smsdk_ext.h"
#include "CDetour/detours.h"
#include "voicecontrol_processor.h"

extern ConVar vc_enabled;

class VoiceControlExt : public SDKExtension, public IConCommandBaseAccessor
{
public:
	bool SDK_OnLoad(char* error, size_t maxlength, bool late) override;
	void SDK_OnAllLoaded() override;
	bool SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlength, bool late) override;
	bool RegisterConCommandBase(ConCommandBase* pCommand) override;
};
