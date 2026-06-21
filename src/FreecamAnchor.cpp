#define FMT_UNICODE 0

#include "FreecamAnchor.h"

#include <random>

#include "Events.h"
#include "Functions.h"
#include "Logging.h"

#include <Glacier/ZActor.h>
#include <Glacier/SGameUpdateEvent.h>
#include <Glacier/ZAction.h>
#include <Glacier/ZCameraEntity.h>
#include <Glacier/ZApplicationEngineWin32.h>
#include <Glacier/ZEngineAppCommon.h>
#include <Glacier/ZFreeCamera.h>
#include <Glacier/ZRender.h>
#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZHM5InputManager.h>
#include <Glacier/ZItem.h>
#include <Glacier/ZInputActionManager.h>

#include "IconsMaterialDesign.h"

FreecamAnchor::FreecamAnchor() :
	m_FreeCamActive(false),
	m_ShouldToggle(false),
	m_FreeCamFrozen(false),
	m_ControlsVisible(false),
	m_DebugMenuActive(false),
	m_NeedsToMove(false),
	m_isTaser(false),
	m_AnchoredItemSpatial(nullptr),
	m_ToggleFreeCamAction("ToggleFreeCamera"),
    m_FreezeFreeCamActionGc("ActivateGameControl0"),
    m_FreezeFreeCamActionKb("KBMInspectNode"),
	m_AnchoredObjectAction("ToggleFollowObject"),
	m_IncreaseXOffset("IncreaseXOffset"),
	m_DecreaseXOffset("DecreaseXOffset"),
	m_IncreaseYOffset("IncreaseYOffset"),
	m_DecreaseYOffset("DecreaseYOffset"),
	m_IncreaseZOffset("IncreaseZOffset"),
	m_DecreaseZOffset("DecreaseZOffset"),
	m_ResetOffset("ResetOffset"),
	m_IncreaseOffsetStep("IncreaseOffsetStep"),
	m_DecreaseOffsetStep("DecreaseOffsetStep"),
	m_OffsetStep(0.5),
	m_Unanchor("Unanchor")
{
    m_PcControls = {
        { "K", "Toggle freecam" },
        { "F3", "Lock camera and enable 47 input" },
        { "Ctrl + W/S", "Change FOV" },
        { "Ctrl + A/D", "Roll camera" },
	    { "Ctrl + X", "Reset roll" },
        { "Alt + W/S", "Change camera speed" },
        { "Space + Q/E", "Change camera height" },
        { "Space + W/S", "Move camera on axis" },
        { "Shift", "Increase camera speed" },
        { "Ctrl + F9", "Follow Object" },
    	{ "Shift + (8,9,0)", "Add to offset on (x,y,z) axis"},
    	{ "Ctrl + (8,9,0)", "Subtract to offset on (x,y,z) axis"},
		{ "l", "Unanchor from object (does not close freecam)" },
		{ ";", "Reset Offset"},
    	{ "[", "Decrease offset step"},
    	{ "]", "Increase offset step"},
    };
}

FreecamAnchor::~FreecamAnchor()
{
    const ZMemberDelegate<FreecamAnchor, void(const SGameUpdateEvent&)> s_Delegate(this, &FreecamAnchor::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

    // Reset the camera to default when unloading with freecam active.
    if (m_FreeCamActive)
    {
        TEntityRef<IRenderDestinationEntity> s_RenderDest;
        Functions::ZCameraManager_GetActiveRenderDestinationEntity->Call(Globals::CameraManager, &s_RenderDest);

        s_RenderDest.m_pInterfaceRef->SetSource(&m_OriginalCam);

        // Enable Hitman input.
        TEntityRef<ZHitman5> s_LocalHitman = SDK()->GetLocalPlayer();

        if (s_LocalHitman)
        {
            auto* s_InputControl = Functions::ZHM5InputManager_GetInputControlForLocalPlayer->Call(Globals::InputManager);

            if (s_InputControl)
            {
                Logger::Debug("Got local hitman entity and input control! Enabling input. {} {}", fmt::ptr(s_InputControl), fmt::ptr(s_LocalHitman.m_pInterfaceRef));
                s_InputControl->m_bActive = true;
            }
        }
    }
}

void FreecamAnchor::Init()
{
    Hooks::ZInputAction_Digital->AddDetour(this, &FreecamAnchor::ZInputAction_Digital);
    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &FreecamAnchor::OnLoadScene);
    Hooks::ZEntitySceneContext_ClearScene->AddDetour(this, &FreecamAnchor::OnClearScene);
}

void FreecamAnchor::OnEngineInitialized()
{
	const ZMemberDelegate<FreecamAnchor, void(const SGameUpdateEvent&)> s_Delegate(this, &FreecamAnchor::OnFrameUpdate);
	Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);

	const char* binds = "FreeCameraInput={"
		"ToggleFreeCamera=tap(kb,k);"
		"ToggleFollowObject=& | hold(kb,lctrl) hold(kb,rctrl) tap(kb,f9);"
		"IncreaseXOffset=& | hold(kb,lshift) hold(kb,rshift) tap(kb,8);"
		"DecreaseXOffset=& | hold(kb,lctrl) hold(kb,rctrl) tap(kb,8);"
		"IncreaseYOffset=& | hold(kb,lshift) hold(kb,rshift) tap(kb,9);"
		"DecreaseYOffset=& | hold(kb,lctrl) hold(kb,rctrl) tap(kb,9);"
		"IncreaseZOffset=& | hold(kb,lshift) hold(kb,rshift) tap(kb,0);"
		"DecreaseZOffset=& | hold(kb,lctrl) hold(kb,rctrl) tap(kb,0);"
		"ResetOffset=tap(kb,semicolon);"
		"IncreaseOffsetStep=tap(kb,rbracket);"
		"DecreaseOffsetStep=tap(kb,lbracket);"
		"Unanchor=tap(kb,l);"
		"};";

	if (ZInputActionManager::AddBindings(binds))
	{
		Logger::Debug("Successfully added bindings.");
	}
	else
	{
		Logger::Debug("Failed to add bindings.");
	}
}

void FreecamAnchor::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent)
{
    if (!*Globals::ApplicationEngineWin32)
        return;

    if (!(*Globals::ApplicationEngineWin32)->m_pEngineAppCommon.m_pFreeCamera01.m_pInterfaceRef)
    {
        Logger::Debug("Creating free camera.");
        Functions::ZEngineAppCommon_CreateFreeCameraAndControl->Call(&(*Globals::ApplicationEngineWin32)->m_pEngineAppCommon);

        // If freecam was active we need to toggle.
        // This can happen after level restarts / changes.
        if (m_FreeCamActive)
            m_ShouldToggle = true;
    }

	(*Globals::ApplicationEngineWin32)->m_pEngineAppCommon.m_pFreeCameraControl01.m_pInterfaceRef->SetActive(m_FreeCamActive);

    if (Functions::ZInputAction_Digital->Call(&m_ToggleFreeCamAction, -1))
    {
        ToggleFreecam();
    }

    if (m_ShouldToggle)
    {
        m_ShouldToggle = false;

        if (m_FreeCamActive)
            EnableFreecam();
        else
            DisableFreecam();
    }

    // While freecam is active, only enable hitman input when the "freeze camera" button is pressed.
    if (m_FreeCamActive)
    {
	    if (Functions::ZInputAction_Digital->Call(&m_FreezeFreeCamActionKb, -1))
            m_FreeCamFrozen = !m_FreeCamFrozen;

	    const bool s_FreezeFreeCam = Functions::ZInputAction_Digital->Call(&m_FreezeFreeCamActionGc, -1) || m_FreeCamFrozen;

	    (*Globals::ApplicationEngineWin32)->m_pEngineAppCommon.m_pFreeCameraControl01.m_pInterfaceRef->m_bFreezeCamera = s_FreezeFreeCam;

	    TEntityRef<ZHitman5> s_LocalHitman = SDK()->GetLocalPlayer();
	    if (s_LocalHitman)
        {
            auto* s_InputControl = Functions::ZHM5InputManager_GetInputControlForLocalPlayer->Call(Globals::InputManager);

            if (s_InputControl)
                s_InputControl->m_bActive = s_FreezeFreeCam;
        }

    	if(m_NeedsToMove && m_AnchoredItemSpatial) {
    		auto s_Camera = (*Globals::ApplicationEngineWin32)->m_pEngineAppCommon.m_pFreeCamera01;
    		SMatrix43 updatedCamMatrix = s_Camera.m_pInterfaceRef->m_mTransform;
    		updatedCamMatrix.Trans = m_AnchoredItemSpatial->m_mTransform.Trans;

    		updatedCamMatrix.Trans.x += m_AnchorOffset.x;
    		updatedCamMatrix.Trans.y += m_AnchorOffset.y;
    		updatedCamMatrix.Trans.z += m_AnchorOffset.z;

    		s_Camera.m_pInterfaceRef->SetObjectToWorldMatrixFromEditor(updatedCamMatrix);
    	}

	    if (Functions::ZInputAction_Digital->Call(&m_Unanchor, -1)) {
    		m_NeedsToMove = false;
    	}

	    if (Functions::ZInputAction_Digital->Call(&m_AnchoredObjectAction, -1)) {
    		Logger::Debug("Anchored To Object :D");
    		AnchorToObject();
    	}

	    if(Functions::ZInputAction_Digital->Call(&m_ResetOffset, -1)) {
			Logger::Debug("OFFSET RESET");
			m_AnchorOffset.x = 0;
			m_AnchorOffset.y = 0;
			m_AnchorOffset.z = 0;
		}

	    if(Functions::ZInputAction_Digital->Call(&m_DecreaseXOffset, -1)) {
    		Logger::Debug("X OFFSET DECREASED");
    		m_AnchorOffset.x -= m_OffsetStep;
    	}
    	if(Functions::ZInputAction_Digital->Call(&m_IncreaseXOffset, -1)) {
    		Logger::Debug("X OFFSET INCREASED");
    		m_AnchorOffset.x += m_OffsetStep;
    	}
    	if(Functions::ZInputAction_Digital->Cal
