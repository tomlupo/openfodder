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

#include "stdafx.hpp"
#include <sstream>

#ifdef EMSCRIPTEN
#include <map>

/**
 * Touch in the browser
 *
 * A finger on the canvas is the pointer. What pressing it does is chosen by the page's
 * thumb buttons, which sit outside the canvas so SDL never sees them as touches:
 * nothing held moves the squad (left button), FIRE shoots (right button), and THROW
 * throws the squad's grenade or rocket (right button, then left while right is held).
 *
 * The engine has one pointer and one of each button, and it samples them once per frame:
 * - fingers share the buttons, which go down with the first finger and up with the last
 * - the newest finger steers the pointer
 * - a lifted finger lets go only once the engine has sampled its latest press twice, so a
 *   quick tap isn't pressed and released inside one frame (menus act on the second sample)
 */
enum eTouchMode {
	eTouchMode_Move = 0,
	eTouchMode_Fire = 1,
	eTouchMode_Throw = 2,
};

static const unsigned int TOUCH_LEFT = 1;
static const unsigned int TOUCH_RIGHT = 2;
static const int16 TOUCH_MIN_SAMPLES = 2;

struct sTouchFinger {
	unsigned int	mButtons = 0;			// Buttons this finger holds down
	bool			mLeftPending = false;	// A throw's left press, sent once the engine has seen the right button
	bool			mLifted = false;		// Up, and letting go once the engine has seen its press
	int16			mPressTick = 0;			// The engine's interrupt tick when this finger last pressed
	uint64			mOrder = 0;				// Higher went down later; the newest finger steers
	cPosition		mPosition;				// Where the finger is, in window pixels
	cPosition		mLeadFrom;				// Where it last gave a walk order, dragging
	bool			mLeading = false;		// It has dragged out a path
};

static int sTouchMode = eTouchMode_Move;
static std::map<SDL_FingerID, sTouchFinger> sTouchFingers;
static int sTouchHolds[2];					// Fingers holding the left and right buttons
static uint64 sTouchOrder = 0;
static cPosition sTouchPointer;				// Where the fingers have put the pointer
static std::vector<cEvent> sPageEvents;	// Queued by the page shell between frames
static bool sTouchInUse = false;			// Touch rather than a mouse: no pointer is drawn

// Engine samples of the mouse since pTick; a press sent at pTick is first seen at pTick + 1
static int16 Touch_SamplesSince(int16 pTick) {
	return (int16)(g_Fodder->mInterruptTick - pTick);
}

static void Touch_Button(sTouchFinger& pFinger, unsigned int pButton, bool pDown, std::vector<cEvent>& pEvents) {
	int& Holds = sTouchHolds[pButton == TOUCH_LEFT ? 0 : 1];

	if (pDown == !!(pFinger.mButtons & pButton))
		return;

	if (pDown) {
		pFinger.mButtons |= pButton;
		pFinger.mPressTick = g_Fodder->mInterruptTick;
		if (Holds++)
			return;
	} else {
		pFinger.mButtons &= ~pButton;
		if (--Holds)
			return;
	}

	cEvent Event;
	if (pButton == TOUCH_LEFT) {
		Event.mType = pDown ? eEvent_MouseLeftDown : eEvent_MouseLeftUp;
		Event.mButton = 1;
	} else {
		Event.mType = pDown ? eEvent_MouseRightDown : eEvent_MouseRightUp;
		Event.mButton = 3;
	}
	Event.mButtonCount = 1;
	Event.mPosition = sTouchPointer;
	pEvents.push_back(Event);
}

/**
 * Move a finger to the buttons pMode wants it to hold, releasing and pressing only the difference
 *
 * pStarting is a new touch; a finger already down when the mode changes only lets go when
 * returning to Move, so lifting FIRE while aiming doesn't send the squad to the aim point.
 */
static void Touch_Apply(sTouchFinger& pFinger, int pMode, bool pStarting, std::vector<cEvent>& pEvents) {
	if (pFinger.mLifted)
		return;

	unsigned int Wanted = 0;
	bool Throw = false;

	switch (pMode) {
	case eTouchMode_Fire:
		Wanted = TOUCH_RIGHT;
		break;
	case eTouchMode_Throw:
		Wanted = TOUCH_RIGHT;
		Throw = true;
		break;
	default:
		Wanted = pStarting ? TOUCH_LEFT : 0;
		break;
	}

	for (unsigned int Button : { TOUCH_LEFT, TOUCH_RIGHT }) {
		if (!(Wanted & Button))
			Touch_Button(pFinger, Button, false, pEvents);
	}
	for (unsigned int Button : { TOUCH_RIGHT, TOUCH_LEFT }) {
		if (Wanted & Button)
			Touch_Button(pFinger, Button, true, pEvents);
	}
	pFinger.mLeftPending = Throw;
}

static bool Touch_Steers(SDL_FingerID pFinger) {
	uint64 Newest = 0;
	SDL_FingerID Steering = 0;

	for (auto& Finger : sTouchFingers) {
		if (!Finger.second.mLifted && Finger.second.mOrder > Newest) {
			Newest = Finger.second.mOrder;
			Steering = Finger.first;
		}
	}
	return Newest && Steering == pFinger;
}

static void Stick_Update(std::vector<cEvent>& pEvents);

// Events the page queued, a throw's left press, and lifted fingers whose press the engine has now seen
static void Touch_Deliver(std::vector<cEvent>& pEvents) {
	pEvents.insert(pEvents.end(), sPageEvents.begin(), sPageEvents.end());
	sPageEvents.clear();

	if (!g_Fodder)
		return;

	Stick_Update(pEvents);

	for (auto Finger = sTouchFingers.begin(); Finger != sTouchFingers.end();) {
		auto& Touch = Finger->second;

		if (Touch.mLeftPending && g_Fodder->mButtonPressRight) {
			Touch.mLeftPending = false;
			Touch_Button(Touch, TOUCH_LEFT, true, pEvents);
		}

		// A throw lifted before the engine saw its right button gives up after a few frames. The
		// tick only runs forward, so a negative count means it wrapped after a very long hold
		const int16 Samples = Touch_SamplesSince(Touch.mPressTick);
		const bool GiveUp = Touch.mLeftPending && (Samples < 0 || Samples > TOUCH_MIN_SAMPLES * 3);
		if (Touch.mLifted && (GiveUp || (!Touch.mLeftPending && (Samples < 0 || Samples >= TOUCH_MIN_SAMPLES)))) {
			Touch_Button(Touch, TOUCH_LEFT, false, pEvents);
			Touch_Button(Touch, TOUCH_RIGHT, false, pEvents);
			Finger = sTouchFingers.erase(Finger);
			continue;
		}
		++Finger;
	}
}

/**
 * Twin sticks, the page's other way to play
 *
 * The move stick gives walk orders a short way ahead of the squad leader, each replacing the last,
 * and the camera leads that way. The aim stick points the pointer ahead of the leader and holds
 * fire through a finger of its own, so it shares the buttons with real fingers.
 */
extern bool g_WebInMission;

static const SDL_FingerID STICK_AIM_FINGER = 0x7FFFFFF0;
static const SDL_FingerID STICK_THROW_FINGER = 0x7FFFFFF1;
static const float STICK_DEAD_ZONE = 0.3f;
static const int16 STICK_WALK_AHEAD = 40;		// pixels ahead of the leader for each walk order
static const int16 STICK_AIM_AHEAD = 80;
static const int16 STICK_ORDER_TICKS = 12;		// a new walk order at least this often, in interrupts

static float sStickMoveX, sStickMoveY, sStickAimX, sStickAimY;
static float sStickOrderX, sStickOrderY;		// direction of the last walk order
static int16 sStickOrderTick;
static bool sStickMoving, sStickThrow;

// A squad in a mission that is running, and taking orders: pOnFoot also rules out a vehicle
static bool Walk_Ready(bool pOnFoot) {
	cFodder& Fodder = *g_Fodder;
	const int16 Squad = Fodder.mSquad_Selected;

	return g_WebInMission && !Fodder.mPhase_Paused && !Fodder.mPhase_Finished &&
		Squad >= 0 && Squad < 3 && Fodder.mSquad_Leader && Fodder.mSquad_Leader != INVALID_SPRITE_PTR &&
		!(pOnFoot && Fodder.mSquad_CurrentVehicle);
}

/**
 * A walk order for the selected squad, the one a click on the map gives (Mouse_Inputs_Check)
 * without the rest of a click: choosing vehicles, sidebar buttons or squads to join
 *
 * pQueue adds the point to the path the squad is walking, as the engine does with an order
 * given while the squad is still taking the last; otherwise it replaces the path.
 * The engine adds only within 8 ticks of the last order, so a finger that rested longer
 * would start a new path from where the squad stands; pQueue adds anyway, because a finger
 * still down is still drawing the same path.
 */
static bool Walk_Order(int16 pX, int16 pY, bool pQueue) {
	if (!Walk_Ready(true))
		return false;

	cFodder& Fodder = *g_Fodder;
	const int16 Squad = Fodder.mSquad_Selected;

	// Squad_Walk_Target_Set writes past the end of the squad's walk list once it is full
	if (pQueue && Fodder.mSquad_Walk_Target_Indexes[Squad] >= 28)
		return false;

	if (Fodder.mMapLoaded) {
		pX = SDL_clamp(pX, (int16)0, (int16)(Fodder.mMapLoaded->getWidth() * 16 - 1));
		pY = SDL_clamp(pY, (int16)3, (int16)(Fodder.mMapLoaded->getHeight() * 16 - 1));
	}

	if (!pQueue) {
		Fodder.mSquad_Walk_Target_Steps[Squad] = 0;
		Fodder.mSquad_WalkTargetX = 0;
		Fodder.mSquad_WalkTargetY = 0;
	} else if (!Fodder.mSquad_Walk_Target_Steps[Squad]) {
		Fodder.mSquad_Walk_Target_Steps[Squad] = 1;	// reopen the window, see above
	}

	Fodder.mCamera_PanTargetX = pX;
	Fodder.mCamera_PanTargetY = pY;
	Fodder.mMouse_Locked = false;
	for (auto& Troop : Fodder.mGame_Data.mSoldiers_Allocated) {
		if (Troop.mSprite == INVALID_SPRITE_PTR || Troop.mSprite == 0 || Troop.mSprite->field_32 != Squad)
			continue;
		Troop.mSprite->mVehicleWalkTarget = 0;
		Troop.mSprite->field_44 = 0;
		Troop.mSprite->mPosXFrac = 0;
		Troop.mSprite->mPosYFrac = 0;
	}
	Fodder.Squad_Walk_Target_Set(pX, pY, Squad, Fodder.mSquad_Leader->mPosX);

	for (auto& JoinTargetSquad : Fodder.mSquad_Join_TargetSquad) {
		if (JoinTargetSquad == Squad)
			JoinTargetSquad = -1;
	}
	return true;
}

// The map position under a point of the window, as Mouse_Inputs_Check turns the pointer into one;
// false over the sidebar
static bool Walk_MapPosition(const cPosition& pWindow, int16& pX, int16& pY) {
	const cDimension Scale = g_Window->GetScale();
	const int X = (int)pWindow.mX / (int)Scale.getWidth();
	const int Y = (int)pWindow.mY / (int)Scale.getHeight();

	if (X < SIDEBAR_WIDTH)
		return false;

	pX = (int16)(X - 32 + (g_Fodder->mCameraX >> 16) - 22);
	pY = (int16)(Y + 4 + (g_Fodder->mCameraY >> 16) - 3);
	return true;
}

// Move the pointer to a map position: the engine fires at it and the camera leans toward it
static void Stick_PointAt(int16 pX, int16 pY, std::vector<cEvent>& pEvents) {
	const cDimension Scale = g_Window->GetScale();
	const cDimension Window = g_Window->GetWindowSize();

	// The inverse of how Mouse_Inputs_Check turns the pointer into a map position, kept on the
	// map view: over the sidebar the pointer would press its buttons instead
	const int MouseX = pX - (g_Fodder->mCameraX >> 16) + 22;
	const int MouseY = pY - (g_Fodder->mCameraY >> 16) + 3;
	const int WindowX = SDL_clamp((MouseX + 32) * (int)Scale.getWidth(), (SIDEBAR_WIDTH + 8) * (int)Scale.getWidth(), (int)Window.getWidth() - 1);
	const int WindowY = SDL_clamp((MouseY - 4) * (int)Scale.getHeight(), 1, (int)Window.getHeight() - 1);

	sTouchPointer = cPosition(WindowX, WindowY);
	cEvent Event(eEvent_MouseMove);
	Event.mPosition = sTouchPointer;
	pEvents.push_back(Event);
}

static void Stick_Update(std::vector<cEvent>& pEvents) {
	cFodder& Fodder = *g_Fodder;

	// A vehicle drives by a tap, as in classic play, but its guns take the aim stick
	const bool Ready = Walk_Ready(false);
	const float MoveLength = SDL_sqrtf(sStickMoveX * sStickMoveX + sStickMoveY * sStickMoveY);
	const float AimLength = SDL_sqrtf(sStickAimX * sStickAimX + sStickAimY * sStickAimY);
	const bool Aiming = Ready && AimLength >= STICK_DEAD_ZONE;

	if (Walk_Ready(true) && MoveLength >= STICK_DEAD_ZONE) {
		const float DirX = sStickMoveX / MoveLength, DirY = sStickMoveY / MoveLength;
		const bool Turned = !sStickMoving || (DirX * sStickOrderX + DirY * sStickOrderY) < 0.94f;
		const bool Due = (int16)(Fodder.mInterruptTick - sStickOrderTick) >= STICK_ORDER_TICKS;

		if (Turned || Due) {
			const int16 LeaderX = Fodder.mSquad_Leader->mPosX, LeaderY = Fodder.mSquad_Leader->mPosY;
			Walk_Order(LeaderX + (int16)(DirX * STICK_WALK_AHEAD), LeaderY + (int16)(DirY * STICK_WALK_AHEAD), false);
			if (!Aiming)
				Stick_PointAt(LeaderX + (int16)(DirX * STICK_AIM_AHEAD), LeaderY + (int16)(DirY * STICK_AIM_AHEAD), pEvents);

			sStickOrderX = DirX;
			sStickOrderY = DirY;
			sStickOrderTick = Fodder.mInterruptTick;
			sStickMoving = true;
		}
	} else if (sStickMoving) {
		// Let go: stop where the leader stands
		if (Walk_Ready(true))
			Walk_Order(Fodder.mSquad_Leader->mPosX, Fodder.mSquad_Leader->mPosY, false);
		sStickMoving = false;
	}

	auto Aim = sTouchFingers.find(STICK_AIM_FINGER);
	if (Aiming) {
		const float DirX = sStickAimX / AimLength, DirY = sStickAimY / AimLength;
		Stick_PointAt(Fodder.mSquad_Leader->mPosX + (int16)(DirX * STICK_AIM_AHEAD),
			Fodder.mSquad_Leader->mPosY + (int16)(DirY * STICK_AIM_AHEAD), pEvents);

		if (Aim == sTouchFingers.end() || Aim->second.mLifted) {
			auto& Finger = sTouchFingers[STICK_AIM_FINGER];
			Touch_Button(Finger, TOUCH_LEFT, false, pEvents);
			Touch_Button(Finger, TOUCH_RIGHT, false, pEvents);
			Finger = sTouchFinger();
			Finger.mOrder = ++sTouchOrder;
			Touch_Button(Finger, TOUCH_RIGHT, true, pEvents);
		}
	} else if (Aim != sTouchFingers.end()) {
		Aim->second.mLifted = true;
	}

	// The grenade button: throw at the pointer, which the sticks keep ahead of the squad. Not
	// from a vehicle, where right then left is a click that drives it or gets the squad out
	if (sStickThrow) {
		sStickThrow = false;
		if (Walk_Ready(true)) {
			auto& Finger = sTouchFingers[STICK_THROW_FINGER];
			Touch_Button(Finger, TOUCH_LEFT, false, pEvents);
			Touch_Button(Finger, TOUCH_RIGHT, false, pEvents);
			Finger = sTouchFinger();
			Finger.mOrder = ++sTouchOrder;
			Touch_Apply(Finger, eTouchMode_Throw, true, pEvents);
			Finger.mLifted = true;
		}
	}
}

// Called by the page shell's sticks: 0 moves, 1 aims; x and y in -1..1, both 0 when let go
extern "C" EMSCRIPTEN_KEEPALIVE void of_stick(int pStick, float pX, float pY) {
	sTouchInUse = true;
	if (pStick == 0) {
		sStickMoveX = pX;
		sStickMoveY = pY;
	} else {
		sStickAimX = pX;
		sStickAimY = pY;
	}
}

// Called by the page shell's grenade button in twin-stick play
extern "C" EMSCRIPTEN_KEEPALIVE void of_stick_throw() {
	sTouchInUse = true;
	sStickThrow = true;
}

// Called by the page shell while a thumb button is held
extern "C" EMSCRIPTEN_KEEPALIVE void of_touch_mode(int pMode) {
	if (pMode < eTouchMode_Move || pMode > eTouchMode_Throw || pMode == sTouchMode || !g_Fodder)
		return;

	sTouchMode = pMode;
	for (auto& Finger : sTouchFingers)
		Touch_Apply(Finger.second, sTouchMode, false, sPageEvents);
}

// Called by the page shell's buttons: an SDL scancode, pressed or released
extern "C" EMSCRIPTEN_KEEPALIVE void of_key(int pScancode, int pDown) {
	cEvent Event(pDown ? eEvent_KeyDown : eEvent_KeyUp);
	Event.mButton = pScancode;
	sPageEvents.push_back(Event);
}
#endif

cWindow::cWindow() {

	mOriginalResolution.mWidth = 320;
	mOriginalResolution.mHeight = 200;

	mScaler = mScalerPrevious = 2;

	mScreenSize.mWidth = mOriginalResolution.mWidth;
	mScreenSize.mHeight = mOriginalResolution.mHeight;

	mWindowMode = true;

	mWindow = 0;

	mRenderer = 0;

    mHasFocus = true;
	mResized = false;
}

cWindow::~cWindow() {

	SDL_DestroyRenderer( mRenderer );
	SDL_DestroyWindow( mWindow );

	SDL_Quit();
}

bool cWindow::InitWindow( const std::string& pWindowTitle ) {
	
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        g_Debugger->Error("Failed to initialise SDL");
		exit( 1 );
		return false;
	}
	
    ToggleFullscreen();

	mWindow = SDL_CreateWindow(pWindowTitle.c_str(), GetWindowSize().mWidth, GetWindowSize().mHeight, 0);
	if (!mWindow) {
        g_Debugger->Error("Failed to create window");
		exit( 1 );
		return false;
	}

    PositionWindow();
	mRenderer = SDL_CreateRenderer(mWindow, nullptr);
	if (!mRenderer) {
        g_Debugger->Error("Failed to create rendered");
		exit( 1 );
		return false;
	}

	SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION, "1", SDL_HINT_OVERRIDE);
	SetMouseSpeed(g_Fodder ? (float)g_Fodder->mStartParams->mMouseSpeed : 1.5f);


    if (g_Fodder->mParams->mWindowMode) {
        ToggleFullscreen();
        CalculateWindowSize();
    }
    else {
        ToggleFullscreen();
        ToggleFullscreen();
    }

	if (!g_Fodder->mParams->mMouseAlternative || (g_Fodder->mParams->mMouseAlternative && g_Fodder->mParams->mMouseLocked)) {
		SDL_SetWindowRelativeMouseMode(mWindow, true);

		// Warp mouse to 0,0 to fix an issue with the mouse cursor appearing on first click in the window
		SDL_WarpMouseInWindow(mWindow, 0, 0);
	}
	
	SDL_HideCursor();

	return true;
}

void cWindow::ToggleVSync(bool pEnabled) {
	#ifdef EMSCRIPTEN
	return;
	#endif
	SDL_SetRenderVSync(mRenderer, pEnabled ? 1 : 0);
}

void cWindow::SetRelativeMouseMode(bool pEnable) {

    if (mWindow) {
        SDL_SetWindowRelativeMouseMode(mWindow, pEnable);
    }
}

void cWindow::SetMouseSpeed(float pSpeed) {
	if (pSpeed < 1.0f)
		pSpeed = 1.0f;
	if (pSpeed > 10.0f)
		pSpeed = 10.0f;

	std::ostringstream speed;
	speed.setf(std::ios::fixed);
	speed.precision(1);
	speed << pSpeed;
	SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, speed.str().c_str());
}

std::vector<cEvent>* cWindow::EventGet() {
    return &mEvents;
}

bool cWindow::Cycle() {

    EventCheck();


    return true;
}

void cWindow::EventCheck() {
	SDL_Event SysEvent;
    static float sMouseMotionRemainderX = 0.0f;
    static float sMouseMotionRemainderY = 0.0f;

#ifdef EMSCRIPTEN
	Touch_Deliver(mEvents);
#endif

	while (SDL_PollEvent(&SysEvent)) {

		cEvent Event;

#ifdef EMSCRIPTEN
		// Touches arrive as finger events; drop the mouse events SDL synthesizes from them
		if ((SysEvent.type == SDL_EVENT_MOUSE_MOTION && SysEvent.motion.which == SDL_TOUCH_MOUSEID) ||
			((SysEvent.type == SDL_EVENT_MOUSE_BUTTON_DOWN || SysEvent.type == SDL_EVENT_MOUSE_BUTTON_UP) && SysEvent.button.which == SDL_TOUCH_MOUSEID))
			continue;
#endif

		switch (SysEvent.type) {
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			Event.mType = eEvent_Focus;
			Event.mHasFocus = false;
			mHasFocus = false;
			break;

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			Event.mType = eEvent_Focus;
			Event.mHasFocus = true;
			mHasFocus = true;
			break;
		case SDL_EVENT_WINDOW_MOUSE_ENTER:
			Event.mType = eEvent_MouseEnter;
			break;
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			Event.mType = eEvent_MouseLeave;
			break;

		case SDL_EVENT_KEY_DOWN:
			Event.mType = eEvent_KeyDown;
			Event.mButton = SysEvent.key.scancode;
			break;

		case SDL_EVENT_KEY_UP:
			Event.mType = eEvent_KeyUp;
			Event.mButton = SysEvent.key.scancode;
			break;

#ifdef EMSCRIPTEN
		case SDL_EVENT_FINGER_MOTION:
		case SDL_EVENT_FINGER_DOWN:
		case SDL_EVENT_FINGER_UP:
		case SDL_EVENT_FINGER_CANCELED:
		{
			const float X = SDL_clamp(SysEvent.tfinger.x, 0.0f, 1.0f);
			const float Y = SDL_clamp(SysEvent.tfinger.y, 0.0f, 1.0f);
			const cPosition Position((unsigned int)(X * GetWindowWidth()), (unsigned int)(Y * GetWindowHeight()));

			if (SysEvent.type == SDL_EVENT_FINGER_DOWN) {
				auto& Finger = sTouchFingers[SysEvent.tfinger.fingerID];

				// A reused id whose last touch is still letting go: let go now
				Touch_Button(Finger, TOUCH_LEFT, false, mEvents);
				Touch_Button(Finger, TOUCH_RIGHT, false, mEvents);
				Finger = sTouchFinger();
				Finger.mOrder = ++sTouchOrder;
				Finger.mPosition = Finger.mLeadFrom = Position;
				sTouchInUse = true;

				// The newest finger takes the pointer before pressing
				sTouchPointer = Position;
				Event.mType = eEvent_MouseMove;
				Event.mPosition = Position;
				mEvents.push_back(Event);
				Event.mType = eEvent_None;

				// While the aim stick holds fire, a finger on the map aims: pressing left there
				// would throw a grenade (left while right is held)
				auto Aim = sTouchFingers.find(STICK_AIM_FINGER);
				const bool Aiming = Aim != sTouchFingers.end() && !Aim->second.mLifted;
				Touch_Apply(Finger, Aiming ? eTouchMode_Fire : sTouchMode, true, mEvents);
				break;
			}

			auto Finger = sTouchFingers.find(SysEvent.tfinger.fingerID);
			if (Finger == sTouchFingers.end() || Finger->second.mLifted)
				break;
			auto& Touch = Finger->second;
			Touch.mPosition = Position;

			// A finger dragged while moving leads the squad: every 12 pixels one more point on its
			// path, given as a walk order rather than a click, so it can't choose a vehicle or a button.
			// The first point starts a new path, as a press on one of the squad's own men gives none
			const bool Leads = sTouchMode == eTouchMode_Move && (Touch.mButtons & TOUCH_LEFT);
			const int DX = (int)Position.mX - (int)Touch.mLeadFrom.mX;
			const int DY = (int)Position.mY - (int)Touch.mLeadFrom.mY;
			const int Step = 12 * mScaler;

			if (SysEvent.type == SDL_EVENT_FINGER_MOTION) {
				if (!Touch_Steers(Finger->first))
					break;

				sTouchPointer = Position;
				Event.mType = eEvent_MouseMove;
				Event.mPosition = Position;

				int16 MapX, MapY;
				if (Leads && DX * DX + DY * DY >= Step * Step && Walk_MapPosition(Position, MapX, MapY) && Walk_Order(MapX, MapY, Touch.mLeading)) {
					Touch.mLeadFrom = Position;
					Touch.mLeading = true;
				}
				break;
			}

			// Up or cancelled: a dragged path ends at its last point. Touch_Deliver lets go once the
			// engine has seen the press; the pointer goes back to the finger that steers now, if
			// another is down
			const bool Steered = Touch_Steers(Finger->first);
			Touch.mLifted = true;
			for (auto& Other : sTouchFingers) {
				if (Steered && Touch_Steers(Other.first)) {
					sTouchPointer = Other.second.mPosition;
					Event.mType = eEvent_MouseMove;
					Event.mPosition = sTouchPointer;
					break;
				}
			}
			break;
		}
#else
		case SDL_EVENT_FINGER_MOTION:
			Event.mType = eEvent_MouseMove;
			Event.mPosition = cPosition((unsigned int)(SysEvent.tfinger.x * GetWindowWidth()),
										(unsigned int)(SysEvent.tfinger.y * GetWindowHeight()));
			break;

		case SDL_EVENT_FINGER_DOWN:
			
			Event.mType = eEvent_MouseLeftDown;
			Event.mButton = 1;
			Event.mPosition = cPosition((unsigned int)(SysEvent.tfinger.x * GetWindowWidth()),
										(unsigned int)(SysEvent.tfinger.y * GetWindowHeight()));

			Event.mButtonCount = 1;

			break;

		case SDL_EVENT_FINGER_UP:
			Event.mType = eEvent_MouseLeftUp;
			Event.mButton = 1;

			Event.mPosition = cPosition((unsigned int)(SysEvent.tfinger.x * GetWindowWidth()),
										(unsigned int)(SysEvent.tfinger.y * GetWindowHeight()));

			Event.mButtonCount = 1;
			mEvents.push_back(Event);

			Event.mType = eEvent_MouseRightUp;
			Event.mButton = 3;

			break;
#endif

		case SDL_EVENT_MOUSE_MOTION:
		{
#ifdef EMSCRIPTEN
			sTouchInUse = false;
#endif
            sMouseMotionRemainderX += SysEvent.motion.xrel;
            sMouseMotionRemainderY += SysEvent.motion.yrel;
            const int Xrel = (int)sMouseMotionRemainderX;
            const int Yrel = (int)sMouseMotionRemainderY;
            sMouseMotionRemainderX -= (float)Xrel;
			sMouseMotionRemainderY -= (float)Yrel;

			Event.mType = eEvent_MouseMove;
			Event.mPosition = cPosition((int)SysEvent.motion.x, (int)SysEvent.motion.y);
			Event.mPositionRelative = cPosition(Xrel, Yrel);
			break;
		}
		case SDL_EVENT_MOUSE_WHEEL:
			Event.mType = eEvent_MouseWheel;
			Event.mPosition = cPosition((int)SysEvent.wheel.x, (int)SysEvent.wheel.y);
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:

			switch (SysEvent.button.button) {

			case 1:
				Event.mType = eEvent_MouseLeftDown;
				Event.mButton = 1;
				break;

			case 3:
				Event.mType = eEvent_MouseRightDown;
				Event.mButton = 3;
				break;
			}

			Event.mPosition = cPosition((int)SysEvent.button.x, (int)SysEvent.button.y);
			Event.mButtonCount = SysEvent.button.clicks;
			break;

		case SDL_EVENT_MOUSE_BUTTON_UP:

			switch (SysEvent.button.button) {

			case 1:
				Event.mType = eEvent_MouseLeftUp;
				Event.mButton = 1;
				break;

			case 3:
				Event.mType = eEvent_MouseRightUp;
				Event.mButton = 3;
				break;
			}

			Event.mPosition = cPosition((int)SysEvent.button.x, (int)SysEvent.button.y);
			Event.mButtonCount = SysEvent.button.clicks;
			break;

		case SDL_EVENT_QUIT:
			Event.mType = eEvent_Quit;
			break;
		}

#ifdef EMSCRIPTEN
		// Drop events which have no x/y
		if (SysEvent.type == SDL_EVENT_MOUSE_MOTION || 
			SysEvent.type == SDL_EVENT_MOUSE_BUTTON_DOWN || 
			SysEvent.type == SDL_EVENT_MOUSE_BUTTON_UP) {

			if (Event.mPosition.mX == 0 || Event.mPosition.mY == 0)
				continue;
		}
#endif
		if ( Event.mType != eEvent_None )
			mEvents.push_back( Event );
	}
}

void cWindow::CalculateWindowSize() {
	SDL_DisplayID display = mWindow ? SDL_GetDisplayForWindow(mWindow) : SDL_GetPrimaryDisplay();
	const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(display);
	if (!current) {
		return;
	}

	while ((mOriginalResolution.mWidth * mScaler) <= (unsigned int) (current->w / 2) && 
			(mOriginalResolution.mHeight * mScaler) <= (unsigned int) (current->h / 2) ) {
		++mScaler;
	}

	SetWindowSize( mScaler );
}

int16 cWindow::CalculateFullscreenSize() {
	SDL_DisplayID display = mWindow ? SDL_GetDisplayForWindow(mWindow) : SDL_GetPrimaryDisplay();
	const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(display);
	if (!current) {
		return 1;
	}
	int16 Multiplier = 1;

	while ((mOriginalResolution.mWidth * Multiplier) <= (unsigned int) current->w && (mOriginalResolution.mHeight * Multiplier) <= (unsigned int) current->h ) {
		++Multiplier;
	}

	return --Multiplier;
}

bool cWindow::CanChangeToMultiplier( const int pNewMultiplier ) {
	SDL_DisplayID display = mWindow ? SDL_GetDisplayForWindow(mWindow) : SDL_GetPrimaryDisplay();
	const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(display);
	if (!current) {
		return false;
	}

	if (	(mOriginalResolution.getWidth()  * pNewMultiplier >= current->w ||
			mOriginalResolution.getHeight() * pNewMultiplier >= current->h) ||
			pNewMultiplier <= 0 )
		return false;

	return true;
}

void cWindow::FrameEnd() {
    #ifndef EMSCRIPTEN
	SDL_RenderPresent( mRenderer );
    
	SDL_SetRenderDrawColor(mRenderer, 0, 0, 0, 0);
	SDL_RenderClear( mRenderer );
	#else
	SDL_SetRenderDrawColor(mRenderer, 0, 0, 0, 0);
	//SDL_RenderClear( mRenderer );
#endif
}

void cWindow::PositionWindow() {
	
	SDL_SetWindowPosition( mWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED );
}

void cWindow::WindowIncrease() {
	
	if (!mWindowMode)
			return;

	// Once we reach the max window size, go to full screen
	if (!CanChangeToMultiplier( mScaler + 1 )) {

		ToggleFullscreen();
		mWindowMode = false;
		return;
	}

	if (!mWindowMode)
		return;

	SetWindowSize( mScaler + 1 );
}

void cWindow::WindowDecrease() {

	// If we're in full screen mode remove it
	if (!mWindowMode) { 

		mWindowMode = true;
		SetWindowSize(mScaler);
		//ToggleFullscreen();
		return;
	}

	if (!CanChangeToMultiplier(mScaler - 1))
		return;

	SetWindowSize( mScaler - 1 );
}

void cWindow::RenderAt( cSurface* pImage ) {
	SDL_FRect Src, Dest;

	Src.w = (float)mScreenSize.mWidth;
	Src.h = (float)mScreenSize.mHeight;
	Src.x = 16.0f;
	Src.y = 16.0f;

	if (g_Fodder->mParams->mIntegerScaling || mWindowMode) {
		Dest.w = (float)GetWindowSize().mWidth;
		Dest.h = (float)GetWindowSize().mHeight;
	}
	else {
		int windowW = 0;
		int windowH = 0;
		SDL_GetWindowSize(mWindow, &windowW, &windowH);
		Dest.h = (float)windowH;
		Dest.w = (float)(Dest.h * (float)(4.0/3.0));
	}

	if (mWindowMode) {
		Dest.x = 0.0f;
		Dest.y = 0.0f;
	}
	else {
		SDL_DisplayID display = mWindow ? SDL_GetDisplayForWindow(mWindow) : SDL_GetPrimaryDisplay();
		const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(display);
		if (current) {
			Dest.x = (float)((current->w - Dest.w) / 2.0f);
			Dest.y = (float)((current->h - Dest.h) / 2.0f);
		} else {
			Dest.x = 0.0f;
			Dest.y = 0.0f;
		}
	}

	SDL_RenderTexture( mRenderer, pImage->GetTexture(), &Src, &Dest );
}

void cWindow::RenderShrunk( cSurface* pImage ) {
	SDL_FRect Src, Dest;
	Src.w = (float)pImage->GetWidth();
	Src.h = (float)pImage->GetHeight();
	Src.x = 0.0f;
	Src.y = 0.0f;

	Dest.w = (float)GetWindowSize().mWidth;
	Dest.h = (float)GetWindowSize().mHeight;

	if (mWindowMode) {
		Dest.x = 0.0f;
		Dest.y = 0.0f;
	}
	else {
		SDL_DisplayID display = mWindow ? SDL_GetDisplayForWindow(mWindow) : SDL_GetPrimaryDisplay();
		const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(display);
		if (current) {
			Dest.x = (float)((current->w - Dest.w) / 2.0f);
			Dest.y = (float)((current->h - Dest.h) / 2.0f);
		} else {
			Dest.x = 0.0f;
			Dest.y = 0.0f;
		}
	}

	SDL_RenderTexture( mRenderer, pImage->GetTexture(), &Src, &Dest);
}

bool cWindow::isFullscreen() const {
    return !mWindowMode;
}

bool cWindow::isMouseInside() const {
	return mWindow == SDL_GetMouseFocus();
}

bool cWindow::isResized() const {
	return mResized;
}

/**
 * Is either mouse button currently pressed
 */
bool cWindow::isMouseButtonPressed_Global() const {
    return  (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) ||
            (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT));
}

/**
 * Is the window currently grabbed
 */
bool cWindow::isGrabbed() const {
    if (!mWindow) {
        return false;
    }
    return SDL_GetWindowMouseGrab(mWindow);
}

void cWindow::ToggleFullscreen() {

	if (mWindowMode) {

		mScalerPrevious = mScaler;
		SetWindowSize( CalculateFullscreenSize() );

        if (mWindow) {
		    SDL_SetWindowFullscreen( mWindow, true );
        }
		mWindowMode = false;
	} else {
		mWindowMode = true;
		SetWindowSize( mScalerPrevious );
	}
}

void cWindow::ClearResized() {
	mResized = false;
}

void cWindow::SetMousePosition(const cPosition& pPosition) {

    SDL_WarpMouseGlobal((float)pPosition.getX(), (float)pPosition.getY());
}

void cWindow::SetMousePositionInWindow(float pX, float pY) {
    if (!mWindow) {
        return;
    }
    SDL_WarpMouseInWindow(mWindow, pX, pY);
}

void cWindow::SetScreenSize( const cDimension& pDimension ) {

	mScreenSize = pDimension;
}

/**
 * Set the window size / resolution using the aspect ratio of the original resolution
 * 
 * @param pDimension Original resolution
 */
void cWindow::SetOriginalRes( const cDimension& pDimension ) {

    if (mOriginalResolution == pDimension)
        return;

	mOriginalResolution = pDimension;
	

	if (!mWindowMode) {
		ToggleFullscreen();
		ToggleFullscreen();
	} else
		SetWindowSize( mScaler );
}

void cWindow::SetWindowTitle( const std::string& pWindowTitle ) {

	SDL_SetWindowTitle( mWindow, pWindowTitle.c_str() );
}

bool cWindow::GetWindowBordersSize(int* pTop, int* pLeft, int* pBottom, int* pRight) const {
    if (!mWindow) {
        if (pTop) *pTop = 0;
        if (pLeft) *pLeft = 0;
        if (pBottom) *pBottom = 0;
        if (pRight) *pRight = 0;
        return false;
    }
    return SDL_GetWindowBordersSize(mWindow, pTop, pLeft, pBottom, pRight);
}

void cWindow::SetWindowSize( const int pMultiplier ) {

	if (pMultiplier < 1 )
		return;

	mScaler = pMultiplier;

	if (mWindow) {
		if (mWindowMode) {
			SDL_SetWindowFullscreen( mWindow, false );
		}
		SDL_SetWindowSize( mWindow, GetWindowSize().mWidth, GetWindowSize().mHeight );
		PositionWindow();
		
		mResized = true;
	}
}

cPosition cWindow::GetWindowPosition() const {
    cPosition Pos;

    SDL_GetWindowPosition(mWindow, &Pos.mX, &Pos.mY);

    return Pos;
}

cDimension cWindow::GetWindowSize() const {
	return cDimension( mOriginalResolution.mWidth * mScaler, mOriginalResolution.mHeight * mScaler ); 
}

int32 cWindow::GetWindowWidth() const {
	return mOriginalResolution.mWidth * mScaler;
}

int32 cWindow::GetWindowHeight() const {
	return mOriginalResolution.mHeight * mScaler;
}

cDimension cWindow::GetScale() const {
    cDimension Result = GetWindowSize() / GetScreenSize();
    if (Result.mHeight == 0)
        Result.mHeight = 1;
    if (Result.mWidth == 0)
        Result.mWidth = 1;
    return Result;
}

float cWindow::GetRefreshRate() {
	SDL_DisplayID display = mWindow ? SDL_GetDisplayForWindow(mWindow) : SDL_GetPrimaryDisplay();
	const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
	if (!mode) {
		std::cerr << "SDL_GetCurrentDisplayMode failed: " << SDL_GetError() << std::endl;
		return 50; // Fallback to 50Hz if query fails
	}
	return mode->refresh_rate;
}

#ifdef EMSCRIPTEN
bool cWindow::TouchInUse() const {
	return sTouchInUse;
}

bool cWindow::StickSteering() const {
	return sStickMoving;
}
#endif

bool cWindowNull::InitWindow(const std::string& pWindowTitle) {


    return true;
}
