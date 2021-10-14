/*
  PokeMini - Pokémon-Mini Emulator
  Copyright (C) 2009-2015  JustBurn

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL/SDL.h>

#include "PokeMini.h"
#include "Hardware.h"
#include "Joystick.h"

#include "UI.h"
#include "Video_x1.h"
#include "Video_x2.h"
#include "PokeMini_BGrs90.h"

const char *AppName = "PokeMini " PokeMini_Version " Dingux";

int cfg_scaling = 0;
int cfg_vsync = 1;

TCommandLineCustom plat_cfg[] = {
	{
		.name = "scaling",
		.ref = &cfg_scaling,
		.type = COMMANDLINE_INT,
		.numa = 0,
		.numb = 1,
	},	{
		.name = "vsync",
		.ref = &cfg_vsync,
		.type = COMMANDLINE_BOOL,
	},	{
		.type = COMMANDLINE_EOL,
	},
};

// Sound buffer size
#define SOUNDBUFFER	1024
#define PMSOUNDBUFF	(SOUNDBUFFER*2)

#define RS90_W		240
#define RS90_H		160
#define PM_W		96
#define PM_H		64
#define BDR2X_W	((RS90_W - PM_W * 2) / 2)
#define BDR2X_H	((RS90_H - PM_H * 2) / 2)

#define ScOffP	((BDR2X_H * RS90_W) + BDR2X_W)

static SDL_Surface *screen, *sketch, *scrdst;
// --------

inline uint16_t mix_rgb565(uint16_t p1, uint16_t p2) {
	return ((p1 & 0xF7DE) >> 1) + ((p2 & 0xF7DE) >> 1) + (p1 & p2 & 0x0821);
}

/* source     target
 * [p 1|p 2]  [p 1|p 1|m12|p 2|p 2]
 * [p 3|p 4]  [p 1|p 1|m11|p 2|p 2]
 *            [m13|m13|all|m24|m24]
 *            [p 3|p 3|m34|p 4|p 4]
 *            [p 3|p 3|m34|p 4|p 4]
 * mab = a mix b, all = p1 mix p2 mix p3 mix p4
 */
static void scale_250percent(uint16_t* src, uint16_t* dst)
{
	uint16_t* dst1 = dst + RS90_W * 0;
	uint16_t* dst2 = dst + RS90_W * 1;
	uint16_t* dst3 = dst + RS90_W * 2;
	uint16_t p1, p2, p3, p4;

	for (uint16_t y = 0; y < PM_H / 2; y++) {
		/* dst1: line 1 */
		/* dst2: line 2 */
		/* dst3: line 3 */
		for (uint16_t x = 0; x < PM_W / 2; x++) {
			p1 = *(src++);
			p3 = *(src++);
			p2 = mix_rgb565(p1, p3);
			*(dst3++) = *(dst2++) = *(dst1++) = p1; /* pix 1 */
			  dst3++,   *(dst2++) = *(dst1++) = p1; /* pix 2 */
			*(dst3++) = *(dst2++) = *(dst1++) = p2; /* pix 3 */
			*(dst3++) = *(dst2++) = *(dst1++) = p3; /* pix 4 */
			  dst3++,   *(dst2++) = *(dst1++) = p3; /* pix 5 */
		}
		dst1  = dst2;
		dst2  = dst3;
		dst3 += RS90_W;
		src  += RS90_W - PM_W;
		/* dst1: line 3 (mix) */
		/* dst2: line 4 */
		/* dst3: line 5 */
		for (uint16_t x = 0; x < PM_W / 2; x++) {
			p1 = *(src++);
			p3 = *(src++);
			p2 = mix_rgb565(p1, p3);
			p4 = mix_rgb565(p1, *dst1);
			*(dst1++) = p4; /* pix 1 */
			*(dst1++) = p4; /* pix 2 */
			p4 = mix_rgb565(p2, *dst1);
			*(dst1++) = p4; /* pix 3 */
			p4 = mix_rgb565(p3, *dst1);
			*(dst1++) = p4; /* pix 4 */
			*(dst1++) = p4; /* pix 5 */
			*(dst2++) = *(dst3++) = p1; /* pix 1 */
			*(dst2++) = *(dst3++) = p1; /* pix 2 */
			*(dst2++) = *(dst3++) = p2; /* pix 3 */
			*(dst2++) = *(dst3++) = p3; /* pix 4 */
			*(dst2++) = *(dst3++) = p3; /* pix 5 */
		}
		dst1  = dst3;
		dst2 += RS90_W * 2;
		dst3 += RS90_W * 2;
		src  += RS90_W - PM_W;
	}
}

// Joystick names and mapping (NEW IN 0.5.0)
char *Joy_KeysNames[] = {
	"Off",		// -1
	"Select",	// 0
	"Start",	// 1
	"Up",		// 2
	"Down",		// 3
	"Left",		// 4
	"Right",	// 5
	"A",		// 6
	"B",		// 7
	"X",		// 8
	"Y",		// 9
	"L",		// 10
	"R"		// 11
};
int Joy_KeysMapping[] = {
	0,		// Menu
	6,		// A
	7,		// B
	11,		// C
	2,		// Up
	3,		// Down
	4,		// Left
	5,		// Right
	1,		// Power
	10		// Shake
};

// Platform menu (REQUIRED >= 0.4.4)
int UIItems_PlatformC(int index, int reason);
void ScalingEnterMenu(void);

TUIMenu_Item UIItems_Platform[] = {
	PLATFORMDEF_GOBACK,
	{ 0,  2, "Scaling...", UIItems_PlatformC },
	{ 0,  3, "Define Joystick...", UIItems_PlatformC },
	{ 0,  4, "V-Sync", UIItems_PlatformC },
	PLATFORMDEF_SAVEOPTIONS,
	PLATFORMDEF_END(UIItems_PlatformC)
};

int UIItems_PlatformC(int index, int reason)
{
	UIMenu_ChangeItem(UIItems_Platform, 2, "Scaling: %s", cfg_scaling ? "2.5x (No Filter)" : "2x");
	UIMenu_ChangeItem(UIItems_Platform, 4, "V-Sync: %s", cfg_vsync ? "Yes" : "No");
	
	if (reason == UIMENU_OK) reason = UIMENU_RIGHT;
	if (reason == UIMENU_CANCEL) UIMenu_PrevMenu();
	if (reason == UIMENU_LEFT) {
		switch (index)
		{
			case 2:
				cfg_scaling = 0;
				UIMenu_ChangeItem(UIItems_Platform, 2, "Scaling: %s", cfg_scaling ? "2.5x (No Filter)" : "2x");
			break;
			case 4:
				cfg_vsync = 0;
				UIMenu_ChangeItem(UIItems_Platform, 4, "V-Sync: %s", cfg_vsync ? "Yes" : "No");
			break;
		}
	}
	if (reason == UIMENU_RIGHT) {
		switch (index)
		{
			case 2:
				cfg_scaling = 1;
				UIMenu_ChangeItem(UIItems_Platform, 2, "Scaling: %s", cfg_scaling ? "2.5x (No Filter)" : "2x");
			break;
			case 3:
				JoystickEnterMenu();
			break;
			case 4:
				cfg_vsync = 1;
				UIMenu_ChangeItem(UIItems_Platform, 4, "V-Sync: %s", cfg_vsync ? "Yes" : "No");
			break;
		}
	}
	return 1;
}


// For the emulator loop
int emurunning = 1;

// Handle keyboard and quit events
void handleevents(SDL_Event *event)
{
	switch (event->type) {
	case SDL_KEYDOWN:
		if (event->key.keysym.sym == SDLK_ESCAPE) {		// Dingoo Select
			JoystickButtonsEvent(0, 1);
		} else if (event->key.keysym.sym == SDLK_RETURN) {	// Dingoo Start
			JoystickButtonsEvent(1, 1);
		} else if (event->key.keysym.sym == SDLK_UP) {		// Dingoo Up
			JoystickButtonsEvent(2, 1);
		} else if (event->key.keysym.sym == SDLK_DOWN) {	// Dingoo Down
			JoystickButtonsEvent(3, 1);
		} else if (event->key.keysym.sym == SDLK_LEFT) {	// Dingoo Left
			JoystickButtonsEvent(4, 1);
		} else if (event->key.keysym.sym == SDLK_RIGHT) {	// Dingoo Right
			JoystickButtonsEvent(5, 1);
		} else if (event->key.keysym.sym == SDLK_LCTRL) {	// Dingoo A
			JoystickButtonsEvent(6, 1);
		} else if (event->key.keysym.sym == SDLK_LALT) {	// Dingoo B
			JoystickButtonsEvent(7, 1);
		} else if (event->key.keysym.sym == SDLK_SPACE) {	// Dingoo X
			JoystickButtonsEvent(8, 1);
		} else if (event->key.keysym.sym == SDLK_LSHIFT) {	// Dingoo Y
			JoystickButtonsEvent(9, 1);
		} else if (event->key.keysym.sym == SDLK_TAB) {		// Dingoo L
			JoystickButtonsEvent(10, 1);
		} else if (event->key.keysym.sym == SDLK_BACKSPACE) {	// Dingoo R
			JoystickButtonsEvent(11, 1);
		}
		break;
	case SDL_KEYUP:
		if (event->key.keysym.sym == SDLK_ESCAPE) {		// Dingoo Select
			JoystickButtonsEvent(0, 0);
		} else if (event->key.keysym.sym == SDLK_RETURN) {	// Dingoo Start
			JoystickButtonsEvent(1, 0);
		} else if (event->key.keysym.sym == SDLK_UP) {		// Dingoo Up
			JoystickButtonsEvent(2, 0);
		} else if (event->key.keysym.sym == SDLK_DOWN) {	// Dingoo Down
			JoystickButtonsEvent(3, 0);
		} else if (event->key.keysym.sym == SDLK_LEFT) {	// Dingoo Left
			JoystickButtonsEvent(4, 0);
		} else if (event->key.keysym.sym == SDLK_RIGHT) {	// Dingoo Right
			JoystickButtonsEvent(5, 0);
		} else if (event->key.keysym.sym == SDLK_LCTRL) {	// Dingoo A
			JoystickButtonsEvent(6, 0);
		} else if (event->key.keysym.sym == SDLK_LALT) {	// Dingoo B
			JoystickButtonsEvent(7, 0);
		} else if (event->key.keysym.sym == SDLK_SPACE) {	// Dingoo X
			JoystickButtonsEvent(8, 0);
		} else if (event->key.keysym.sym == SDLK_LSHIFT) {	// Dingoo Y
			JoystickButtonsEvent(9, 0);
		} else if (event->key.keysym.sym == SDLK_TAB) {		// Dingoo L
			JoystickButtonsEvent(10, 0);
		} else if (event->key.keysym.sym == SDLK_BACKSPACE) {	// Dingoo R
			JoystickButtonsEvent(11, 0);
		}
		break;
	case SDL_QUIT:
		emurunning = 0;
		break;
	};
}

// Used to fill the sound buffer
void emulatorsound(void *unused, Uint8 *stream, int len)
{
	MinxAudio_GetSamplesS16((int16_t *)stream, len>>1);
}

// Enable / Disable sound
void enablesound(int sound)
{
	MinxAudio_ChangeEngine(sound);
	if (AudioEnabled) SDL_PauseAudio(!sound);
}

static SDL_Rect *rumbtop, *rumbbtm;
static SDL_Rect rumbtop2x = {.x = BDR2X_W, .y = BDR2X_H - 2,        .w = PM_W * 2, .h = 2};
static SDL_Rect rumbbtm2x = {.x = BDR2X_W, .y = BDR2X_H + PM_H * 2, .w = PM_W * 2, .h = 2};
static SDL_Rect rumbtop1x = {.x = BDR2X_W, .y = BDR2X_H - 2,        .w = PM_W * 1, .h = 2};
static SDL_Rect rumbbtm1x = {.x = BDR2X_W, .y = BDR2X_H + PM_H * 1, .w = PM_W * 1, .h = 2};

void Setup_Screen()
{
	TPokeMini_VideoSpec* videospec;
	
	screen = SDL_SetVideoMode(RS90_W, RS90_H, 16, SDL_HWSURFACE | (cfg_vsync ? SDL_TRIPLEBUF : 0));
	if (screen == NULL) {
		fprintf(stderr, "Couldn't set video mode: %s\n", SDL_GetError());
		exit(1);
	}

	if (cfg_scaling) {
		/* 2.5x : Blit -> 1x (96x64) sketch -> 2.5x (240x160) screen */
		videospec = (TPokeMini_VideoSpec *) &PokeMini_Video1x1;
		scrdst    = sketch;
		rumbtop   = &rumbtop1x;
		rumbbtm   = &rumbbtm1x;
	} else {
		/* 2x : Blit -> 2x (192x128) screen */
		videospec = (TPokeMini_VideoSpec *) &PokeMini_Video2x2;
		scrdst    = screen;
		rumbtop   = &rumbtop2x;
		rumbbtm   = &rumbbtm2x;
	}

	if (!PokeMini_SetVideo(videospec, 16, CommandLine.lcdfilter, CommandLine.lcdmode)) {
		fprintf(stderr, "Couldn't set video spec\n");
		exit(1);
	}
}

static void Clear_Screen()
{
	uint32_t i;
	for(i=0;i<3;i++)
	{
		SDL_FillRect(screen, NULL, 0);
		SDL_FillRect(sketch, NULL, 0);
		SDL_Flip(screen);
	}
}

// Menu loop
void menuloop()
{
	SDL_Event event;
	
	// Stop sound
	SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
	enablesound(0);

	// Update EEPROM
	PokeMini_SaveFromCommandLines(0);

	// Menu's loop
	Clear_Screen();
	
	while (emurunning && (UI_Status == UI_STATUS_MENU)) {
		// Slowdown to approx. 60fps
		SDL_Delay(16);

		// Process UI
		UIMenu_Process();

		// Screen rendering
		// Render the menu or the game screen
		UIMenu_Display_16((uint16_t *)sketch->pixels, RS90_W);
		SDL_BlitSurface(sketch, NULL, screen, NULL);
		SDL_Flip(screen);

		// Handle events
		while (SDL_PollEvent(&event)) handleevents(&event);
	}
	
	Clear_Screen();

	// Apply configs
	Setup_Screen();
	PokeMini_ApplyChanges();
	if (UI_Status == UI_STATUS_EXIT) emurunning = 0;
	else enablesound(CommandLine.sound);
	SDL_EnableKeyRepeat(0, 0);
}

char home_path[PMTMPV];
char save_path[PMTMPV];
char conf_path[PMTMPV];
char plat_path[PMTMPV];

// Main function
int main(int argc, char **argv)
{
	SDL_Joystick *joy;
	SDL_Event event;

	// Get native resolution
	printf("Native resolution: %dx%d\n", RS90_W, RS90_H);

	// Process arguments
	printf("%s\n\n", AppName);
	PokeMini_InitDirs(argv[0], NULL);
	
	snprintf(home_path, sizeof(home_path) - 1, "%s/.pokemini", getenv("HOME"));
	snprintf(save_path, sizeof(save_path) - 1, "%s/saves", home_path);
	snprintf(conf_path, sizeof(conf_path) - 1, "%s/pokemini_rs90.cfg", home_path);
	snprintf(plat_path, sizeof(plat_path) - 1, "%s/platform_rs90.cfg", home_path);
	if (access( home_path, F_OK ) == -1)
	{ 
		mkdir(home_path, 0755);
	}
	
	if (access( save_path, F_OK ) == -1)
	{ 
		mkdir(save_path, 0755);
	}

	CommandLineInit();
	/* Extra cmdline init for rs90 */
	CommandLine.lcdmode = 0;	// LCD Mode
	CommandLine.sound = MINX_AUDIO_EMULATED;
	CommandLine.synccycles = 8;	// Sync cycles to 8 (Accurate)
	snprintf(CommandLine.bios_file, sizeof(CommandLine.bios_file) - 1, "%s/bios.min", home_path);
	CommandLineConfFile(conf_path, plat_path, &plat_cfg[0]);
	if (!CommandLineArgs(argc, argv, &plat_cfg[0])) {
		PrintHelpUsage(stdout);
		return 1;
	}
	JoystickSetup("Dingoo", 0, 30000, Joy_KeysNames, 12, Joy_KeysMapping);

	// Initialize SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
		fprintf(stderr, "Couldn't initialize SDL: %s\n", SDL_GetError());
		exit(1);
	}
	joy = SDL_JoystickOpen(0);
	atexit(SDL_Quit); // Clean up on exit

	// Disable key repeat and hide cursor
	SDL_EnableKeyRepeat(0, 0);
	SDL_ShowCursor(SDL_DISABLE);

	// Initialize the display
	sketch = SDL_CreateRGBSurface(SDL_SWSURFACE, RS90_W, RS90_H, 16, 0,0,0,0);

	// Set video spec and check if is supported
	Setup_Screen();
	UIMenu_SetDisplay(RS90_W, RS90_H, PokeMini_BGR16, (uint8_t *)PokeMini_BGrs90, (uint16_t *)PokeMini_BGrs90_PalBGR16, (uint32_t *)PokeMini_BGrs90_PalBGR32);

	// Initialize the sound
	SDL_AudioSpec audfmt, outfmt;
	audfmt.freq = 44100;
	audfmt.format = AUDIO_S16SYS;
	audfmt.channels = 1;
	audfmt.samples = SOUNDBUFFER;
	audfmt.callback = emulatorsound;
	audfmt.userdata = NULL;

	// Open the audio device
	if (SDL_OpenAudio(&audfmt, &outfmt) < 0) {
		fprintf(stderr, "Unable to open audio: %s\n", SDL_GetError());
		fprintf(stderr, "Audio will be disabled\n");
		AudioEnabled = 0;
	} else {
		AudioEnabled = 1;
	}

	// Initialize the emulator
	printf("Starting emulator...\n");
	if (!PokeMini_Create(0, PMSOUNDBUFF)) {
		fprintf(stderr, "Error while initializing emulator.\n");
	}

	// Setup palette and LCD mode
	PokeMini_VideoPalette_Init(PokeMini_BGR16, 1);
	PokeMini_VideoPalette_Index(CommandLine.palette, CommandLine.custompal, CommandLine.lcdcontrast, CommandLine.lcdbright);
	PokeMini_ApplyChanges();

	// Load stuff
	PokeMini_UseDefaultCallbacks();
	if (!PokeMini_LoadFromCommandLines("Using FreeBIOS", "EEPROM data will be discarded!")) {
		UI_Status = UI_STATUS_MENU;
	}

	// Enable sound & init UI
	printf("Starting emulator...\n");
	UIMenu_Init();
	enablesound(CommandLine.sound);

	// Emulator's loop
	int DropCount = 5;
	unsigned long NewTickSync = 0, CurrentTick = 0;
	SDL_FillRect(sketch, NULL, 0);
	while (emurunning) {
		PokeMini_EmulateFrame();
		// Screen rendering
		// Render the menu or the game screen
		if (cfg_vsync && !(--DropCount)) DropCount = 5; /* Drop 1 frame for every continuous 5 frames (75hz -> 60hz) */
		else if (LCDDirty || PokeMini_Rumbling) {
			SDL_FillRect(scrdst, rumbtop, 0);
			SDL_FillRect(scrdst, rumbbtm, 0);
			PokeMini_VideoBlit((uint16_t *)scrdst->pixels + ScOffP + (PokeMini_Rumbling ? PokeMini_GenRumbleOffset(RS90_W) : 0), RS90_W);
			if (cfg_scaling) scale_250percent((uint16_t*)scrdst->pixels + ScOffP, (uint16_t*)screen->pixels);
			LCDDirty = 0;
			SDL_Flip(screen);
		}

		// Emulate and syncronize
		if (RequireSoundSync) {
			// Sleep a little in the hope to free a few samples
			while (MinxAudio_SyncWithAudio()) SDL_Delay(1);
		} else {
			while ((CurrentTick = SDL_GetTicks()) < NewTickSync) SDL_Delay(1);	// This lower CPU usage
			if (NewTickSync + 14 * 4 < CurrentTick) NewTickSync = CurrentTick;
			else NewTickSync = NewTickSync + 14;	// Aprox 72 times per sec
		}

		// Handle events
		while (SDL_PollEvent(&event)) handleevents(&event);

		// Menu
		if (UI_Status == UI_STATUS_MENU) menuloop();
	}

	// Disable sound & free UI
	enablesound(0);
	UIMenu_Destroy();
	
	if (sketch) SDL_FreeSurface(sketch);
	if (screen) SDL_FreeSurface(screen);
	
	// Save Stuff
	PokeMini_SaveFromCommandLines(1);

	// Close joystick
	if (joy) SDL_JoystickClose(joy);

	// Terminate...
	printf("Shutdown emulator...\n");
	PokeMini_VideoPalette_Free();
	PokeMini_Destroy();

	return 0;
}

