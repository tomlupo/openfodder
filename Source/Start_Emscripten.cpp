/*
 *  Open Fodder
 *  ---------------
 *
 *  Copyright (C) 2008-2026 Open Fodder
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#ifdef EMSCRIPTEN
#include "stdafx.hpp"

cAbout* About = 0;

// -1 = Phase Try Again, 0 = Phase Won, 1 = Phase Running
static int16 sPhaseResult = -1;
bool g_WebInMission = false;	// Read by the pointer drawing (Mouse.cpp)

/**
 * State the page shell polls to decide which touch controls to show
 *
 * 1: a mission is running  2: it is paused
 * 4: rockets are armed  8: grenades are armed  16: the squad has grenades  32: it has rockets
 * 64, 128, 256: squads 1, 2 and 3 have troops  512: the About screen is showing
 */
extern "C" EMSCRIPTEN_KEEPALIVE int of_state() {
	if (!g_Fodder)
		return 0;
	if (!g_WebInMission)
		return About ? 512 : 0;

	int State = 1;
	if (g_Fodder->mPhase_Paused)
		State |= 2;

	const int16 Squad = g_Fodder->mSquad_Selected;
	if (Squad >= 0 && Squad < 3) {
		if (g_Fodder->mSquad_CurrentWeapon[Squad] == eWeapon_Rocket)
			State |= 4;
		if (g_Fodder->mSquad_CurrentWeapon[Squad] == eWeapon_Grenade)
			State |= 8;
		if (g_Fodder->mSquad_Grenades[Squad])
			State |= 16;
		if (g_Fodder->mSquad_Rockets[Squad])
			State |= 32;
	}
	for (int Count = 0; Count < 3; ++Count) {
		if (g_Fodder->mSquads_TroopCount[Count])
			State |= 64 << Count;
	}
	return State;
}

/**
 * The loaded map's size in tiles, height << 16 | width, or 0 outside a mission
 *
 * A phone's wide view can be wider than a small map, which leaves black beyond its edge;
 * the page crops the picture to the map.
 */
extern "C" EMSCRIPTEN_KEEPALIVE int of_map() {
	if (!g_Fodder || !g_WebInMission || !g_Fodder->mMapLoaded)
		return 0;

	return ((g_Fodder->mMapLoaded->getHeight() & 0x7FFF) << 16) | (g_Fodder->mMapLoaded->getWidth() & 0xFFFF);
}

/**
 * Called as the page's grenade button goes down: choose what it throws
 *
 * A mouse player picks grenades or rockets in the sidebar, and each phase starts with
 * grenades chosen even when the squad has none. The button keeps the chosen weapon while
 * it has ammo, else takes grenades, else rockets, the click a mouse player would make.
 */
extern "C" EMSCRIPTEN_KEEPALIVE void of_arm() {
	if (!g_Fodder || !g_WebInMission)
		return;

	const int16 Squad = g_Fodder->mSquad_Selected;
	if (Squad < 0 || Squad >= 3)
		return;

	const int16 Weapon = g_Fodder->mSquad_CurrentWeapon[Squad];
	const bool Grenades = g_Fodder->mSquad_Grenades[Squad] != 0;
	const bool Rockets = g_Fodder->mSquad_Rockets[Squad] != 0;

	if ((Weapon == eWeapon_Grenade && Grenades) || (Weapon == eWeapon_Rocket && Rockets))
		return;

	if (Grenades)
		g_Fodder->Squad_Select_Grenades();
	else if (Rockets)
		g_Fodder->Squad_Select_Rockets();
}

/**
 * How many engine frames are due now
 *
 * The engine is paced by the Amiga's 50 Hz vertical blank (mSleepDelta, 20 ms), but
 * requestAnimationFrame fires at the display's rate: 60, 120 or 144 Hz, or 30 in iOS
 * Low Power Mode. Run engine frames on wall-clock time instead of once per callback.
 */
static int Frames_Due() {
	static double Last = emscripten_get_now();
	static double Pending = 0;

	const double Now = emscripten_get_now();
	const double Step = g_Fodder->mParams->mSleepDelta ? (double)g_Fodder->mParams->mSleepDelta : 20.0;

	Pending += Now - Last;
	Last = Now;

	// Back from a hidden tab or a long stall: carry on rather than fast-forward
	if (Pending > Step * 4)
		Pending = Step;

	int Due = 0;
	while (Pending >= Step && Due < 2) {
		Pending -= Step;
		++Due;
	}
	return Due;
}

void phase_loop();
void menu_loop();

static void Switch_To_Menu() {
	g_WebInMission = false;
	sPhaseResult = -1;
	g_Fodder->mPhase_Paused = false;

	emscripten_cancel_main_loop();
	emscripten_set_main_loop(menu_loop, 0, true);
}

static void menu_frame() {
    g_Fodder->Interrupt_Sim_Tick();

	static int16 result = -1;
	if (result == -1) {
		g_Fodder->VersionSwitch(g_Fodder->mVersions->GetForCampaign("Amiga Format Christmas Special"));
		g_Fodder->mGame_Data.mCampaign.Clear();

		result = 0;
		g_Fodder->Campaign_Select_Setup();
		g_Fodder->MapTiles_ResetScrollState();
		g_Fodder->mInterruptCallback = []() {
			
			if (!g_Fodder->mStartParams->mDisableVideo) {
				g_Fodder->mGraphics->MapTiles_Draw();
			}
			g_Fodder->Sprites_Draw();
			g_Fodder->Campaign_Select_DrawMenu("OPEN FODDER", "SELECT CAMPAIGN");
			g_Fodder->mGraphics->SetActiveSpriteSheet(eGFX_IN_GAME);
			g_Fodder->Mouse_DrawCursor();

			
		};
		return;
	}

	if (About) {

		if (About->Cycle()) {
			g_Fodder->mWindow->RenderAt(g_Fodder->mSurface);
			g_Fodder->mWindow->FrameEnd();
			//g_Fodder->Cycle_End();
			return;
		}

		delete About;
		About = 0;
		g_Fodder->mGUI_SaveLoadAction = 0;
		result = -1;
		return;
	}
	g_Fodder->Campaign_Select_File_Cycle("OPEN FODDER", "SELECT CAMPAIGN");
	g_Fodder->Video_Sleep();

	if(g_Fodder->mGUI_SaveLoadAction == 3 || g_Fodder->mGUI_SaveLoadAction == 0) {
		return;
	}
	if (g_Fodder->mGUI_SaveLoadAction == 4) {
		g_Fodder->mGUI_SaveLoadAction = 0;
		g_Fodder->mInterruptCallback = nullptr;
		About = new cAbout();
		return;
	}

	// Exit (the Escape key): a browser tab has nowhere to exit to
	if (g_Fodder->mGUI_SaveLoadAction == 1) {
		g_Fodder->mGUI_SaveLoadAction = 0;
		g_Fodder->mPhase_Aborted = false;
		return;
	}

	g_Fodder->mPhase_Aborted = false;
	g_Fodder->mPhase_In_Progress = false;

	std::string Campaign = g_Fodder->mCampaignList[g_Fodder->mGUI_Select_File_CurrentIndex + g_Fodder->mGUI_Select_File_SelectedFileIndex];

	g_Fodder->VersionSwitch(g_Fodder->mVersions->GetForCampaign(Campaign));
	g_Fodder->mGame_Data.mCampaign.LoadCampaign(Campaign, Campaign != g_Fodder->mVersionCurrent->mName);
	g_Fodder->Game_Setup();

	g_Fodder->mInterruptCallback = nullptr;
	g_Fodder->mPhase_In_Progress = false;
	result = -1;
	sPhaseResult = -1;
	g_WebInMission = true;
	emscripten_cancel_main_loop();
	emscripten_set_main_loop(phase_loop, 0, true);
}

static void phase_frame() {
    g_Fodder->Interrupt_Sim_Tick();

	// No recruits left?
	if (sPhaseResult != 1) {
		if (!g_Fodder->mGame_Data.mRecruits_Available_Count) {
			Switch_To_Menu();
			return;
		}
	}

	if (sPhaseResult == 0) {
		// Game Won?
		if (!g_Fodder->mGame_Data.Phase_Next()) {
			// Break to version screen
			Switch_To_Menu();
			return;
		}
		sPhaseResult = -1;
	}

	if (sPhaseResult == -1) {
		if (g_Fodder->mPhase_Aborted2) {
			g_Fodder->mPhase_Aborted2 = false;
			Switch_To_Menu();
			return;
		}
		g_Fodder->Phase_EngineReset();
		g_Fodder->Phase_SquadPrepare();
		g_Fodder->Phase_Prepare();
	}

	sPhaseResult = g_Fodder->Phase_Cycle();
	g_Fodder->Video_Sleep();
}

// Both loops run engine frames only when due, so the browser just keeps showing the last frame in between
void menu_loop() {
	for (int Due = Frames_Due(); Due > 0; --Due)
		menu_frame();
}

void phase_loop() {
	for (int Due = Frames_Due(); Due > 0; --Due)
		phase_frame();
}

int start(int argc, char *argv[]) {

	g_Debugger = std::make_shared<cDebugger>();
	g_Window = std::make_shared<cWindow>();
	g_ResourceMan = std::make_shared<cResourceMan>();
	g_Fodder = std::make_shared<cFodder>(g_Window);

	auto Params = std::make_shared<sFodderParameters>();
	Params->Process(argc, argv);

	if (Params->mShowHelp)
		return 0;

	// Cheats come from the page's ?cheats URL flag, passed as --cheats
	Params->mMouseAlternative = true;
	g_Fodder->Prepare(Params);
	g_Fodder->Phase_SquadPrepare();

	emscripten_set_main_loop(menu_loop, 0, true);

	//g_Fodder->Service_Show();
	return 0;
}

#endif
