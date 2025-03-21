/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2015, Tom Charlesworth, Michael Pohoreski

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Description: Save-state (snapshot) module
 *
 * Author: Copyright (c) 2004-2015 Tom Charlesworth
 */

#include "StdAfx.h"

#include "SaveState.h"
#include "YamlHelper.h"

#include "Interface.h"
#include "CardManager.h"
#include "CopyProtectionDongles.h"
#include "Debug.h"
#include "Joystick.h"
#include "Keyboard.h"
#include "Memory.h"
#include "Pravets.h"
#include "Speaker.h"
#include "Speech.h"
#include "Harddisk.h"

#include "Configuration/Config.h"
#include "Configuration/IPropertySheet.h"


#define DEFAULT_SNAPSHOT_NAME "SaveState.aws.yaml"

bool g_bSaveStateOnExit = false;

static std::string g_strSaveStateFilename;
static std::string g_strSaveStatePathname;
static std::string g_strSaveStatePath;

static YamlHelper yamlHelper;

#define SS_FILE_VER 2

// Unit version history:
// v2: Extended: keyboard (added 'Key Waiting'), memory (LC mem type for II/II+, inverted MF_INTCXROM bit)
// v3: Extended: memory (added 'AnnunciatorN')
// v4: Extended: video (added 'Video Refresh Rate')
// v5: Extended: cpu (added 'Defer IRQ By 1 Opcode')
// v6: Added 'Unit Miscellaneous' for NoSlotClock(NSC)
// v7: Extended: joystick (added 'Paddle Inactive Cycle')
// v8: Added 'Unit Game I/O Connector' for Game I/O Connector
// v9: Extended: memory (added 'Last Slot to Set Main Mem LC', 'MMU LC Mode')
#define UNIT_APPLE2_VER 9

#define UNIT_SLOTS_VER 1

// See CopyProtectionDongle.cppS
#define UNIT_GAME_IO_CONNECTOR_VER 3

#define UNIT_MISC_VER 1

//-----------------------------------------------------------------------------

static bool g_ignoreHdcFirmware = false;

bool Snapshot_GetIgnoreHdcFirmware()
{
	return g_ignoreHdcFirmware;
}

void Snapshot_SetIgnoreHdcFirmware(const bool ignoreHdcFirmware)
{
	g_ignoreHdcFirmware = ignoreHdcFirmware;
}

//-----------------------------------------------------------------------------

static void Snapshot_SetPathname(const std::string& strPathname)
{
	if (strPathname.empty())
	{
		g_strSaveStateFilename = DEFAULT_SNAPSHOT_NAME;

		g_strSaveStatePathname = g_sCurrentDir;
		if (!g_strSaveStatePathname.empty() && *g_strSaveStatePathname.rbegin() != PATH_SEPARATOR)
			g_strSaveStatePathname += PATH_SEPARATOR;
		g_strSaveStatePathname.append(DEFAULT_SNAPSHOT_NAME);

		g_strSaveStatePath = g_sCurrentDir;
		return;
	}

	std::string strFilename = strPathname;	// Set default, as maybe there's no path
	g_strSaveStatePath.clear();

	int nIdx = strPathname.find_last_of(PATH_SEPARATOR);
	if (nIdx >= 0 && nIdx+1 < (int)strPathname.length())	// path exists?
	{
		strFilename = &strPathname[nIdx+1];
		g_strSaveStatePath = strPathname.substr(0, nIdx+1); // Bugfix: 1.25.0.2 // Snapshot_LoadState() -> SetCurrentImageDir() -> g_sCurrentDir 
	}

	g_strSaveStateFilename = strFilename;
	g_strSaveStatePathname = strPathname;
}

void Snapshot_SetFilename(const std::string& filename, const std::string& path/*=""*/)
{
	if (path.empty())
		return Snapshot_SetPathname(filename);

	_ASSERT(filename.find(PATH_SEPARATOR) == std::string::npos);	// since we have a path, then filename mustn't contain a path too!

	// Ensure path is suffixed with '\' before adding filename
	std::string pathname = path;
	if (*pathname.rbegin() != PATH_SEPARATOR)
		pathname += PATH_SEPARATOR;

	Snapshot_SetPathname(pathname+filename);
}

const std::string& Snapshot_GetFilename(void)
{
	return g_strSaveStateFilename;
}

const std::string& Snapshot_GetPath(void)
{
	return g_strSaveStatePath;
}

const std::string& Snapshot_GetPathname(void)
{
	return g_strSaveStatePathname;
}

// Called on successful insertion and on prompting to save/load a save-state
void Snapshot_GetDefaultFilenameAndPath(std::string& defaultFilename, std::string& defaultPath)
{
	// Attempt to get a default filename/path based on harddisk plugged-in or floppy disk inserted
	// . Priority given to harddisk over floppy images

	if (GetCardMgr().QuerySlot(SLOT7) == CT_GenericHDD)
		dynamic_cast<HarddiskInterfaceCard&>(GetCardMgr().GetRef(SLOT7)).GetFilenameAndPathForSaveState(defaultFilename, defaultPath);

	if (defaultFilename.empty())
		GetCardMgr().GetDisk2CardMgr().GetFilenameAndPathForSaveState(defaultFilename, defaultPath);
}

// Called by Disk2InterfaceCard::InsertDisk() and HD_Insert() after a successful insertion
// Called by Disk2InterfaceCard::EjectDisk() and HD_Unplug()
// Called by RepeatInitialization() when Harddisk Controller card is disabled
void Snapshot_UpdatePath(void)
{
	std::string defaultFilename;
	std::string defaultPath;
	Snapshot_GetDefaultFilenameAndPath(defaultFilename, defaultPath);

	if (defaultPath.empty() || g_strSaveStatePath == defaultPath)
		return;

	if (!defaultFilename.empty())
		defaultFilename += ".aws.yaml";

	Snapshot_SetFilename(defaultFilename, defaultPath);
}

//-----------------------------------------------------------------------------

static const std::string& GetSnapshotUnitApple2Name(void)
{
	static const std::string name("Apple2");
	return name;
}

static const std::string& GetSnapshotUnitSlotsName(void)
{
	static const std::string name("Slots");
	return name;
}

static const std::string& GetSnapshotUnitGameIOConnectorName(void)
{
	static const std::string name("Game I/O Connector");
	return name;
}

static const std::string& GetSnapshotUnitMiscName(void)
{
	static const std::string name("Miscellaneous");
	return name;
}

#define SS_YAML_KEY_MODEL "Model"

#define SS_YAML_VALUE_APPLE2			"Apple]["
#define SS_YAML_VALUE_APPLE2PLUS		"Apple][+"
#define SS_YAML_VALUE_APPLE2JPLUS		"Apple][ J-Plus"
#define SS_YAML_VALUE_APPLE2E			"Apple//e"
#define SS_YAML_VALUE_APPLE2EENHANCED	"Enhanced Apple//e"
#define SS_YAML_VALUE_APPLE2C			"Apple2c"
#define SS_YAML_VALUE_PRAVETS82			"Pravets82"
#define SS_YAML_VALUE_PRAVETS8M			"Pravets8M"
#define SS_YAML_VALUE_PRAVETS8A			"Pravets8A"
#define SS_YAML_VALUE_TK30002E			"TK3000//e"
#define SS_YAML_VALUE_BASE64A			"Base 64A"

static eApple2Type ParseApple2Type(std::string type)
{
	if (type == SS_YAML_VALUE_APPLE2)				return A2TYPE_APPLE2;
	else if (type == SS_YAML_VALUE_APPLE2PLUS)		return A2TYPE_APPLE2PLUS;
	else if (type == SS_YAML_VALUE_APPLE2JPLUS)		return A2TYPE_APPLE2JPLUS;
	else if (type == SS_YAML_VALUE_APPLE2E)			return A2TYPE_APPLE2E;
	else if (type == SS_YAML_VALUE_APPLE2EENHANCED)	return A2TYPE_APPLE2EENHANCED;
	else if (type == SS_YAML_VALUE_APPLE2C)			return A2TYPE_APPLE2C;
	else if (type == SS_YAML_VALUE_PRAVETS82)		return A2TYPE_PRAVETS82;
	else if (type == SS_YAML_VALUE_PRAVETS8M)		return A2TYPE_PRAVETS8M;
	else if (type == SS_YAML_VALUE_PRAVETS8A)		return A2TYPE_PRAVETS8A;
	else if (type == SS_YAML_VALUE_TK30002E)		return A2TYPE_TK30002E;
	else if (type == SS_YAML_VALUE_BASE64A)			return A2TYPE_BASE64A;

	throw std::runtime_error("Load: Unknown Apple2 type");
}

static std::string GetApple2TypeAsString(void)
{
	switch ( GetApple2Type() )
	{
		case A2TYPE_APPLE2:			return SS_YAML_VALUE_APPLE2;
		case A2TYPE_APPLE2PLUS:		return SS_YAML_VALUE_APPLE2PLUS;
		case A2TYPE_APPLE2JPLUS:	return SS_YAML_VALUE_APPLE2JPLUS;
		case A2TYPE_APPLE2E:		return SS_YAML_VALUE_APPLE2E;
		case A2TYPE_APPLE2EENHANCED:return SS_YAML_VALUE_APPLE2EENHANCED;
		case A2TYPE_APPLE2C:		return SS_YAML_VALUE_APPLE2C;
		case A2TYPE_PRAVETS82:		return SS_YAML_VALUE_PRAVETS82;
		case A2TYPE_PRAVETS8M:		return SS_YAML_VALUE_PRAVETS8M;
		case A2TYPE_PRAVETS8A:		return SS_YAML_VALUE_PRAVETS8A;
		case A2TYPE_TK30002E:		return SS_YAML_VALUE_TK30002E;
		case A2TYPE_BASE64A:		return SS_YAML_VALUE_BASE64A;
		default:
			throw std::runtime_error("Save: Unknown Apple2 type");
	}
}

//---

static void ParseUnitApple2(YamlLoadHelper& yamlLoadHelper, UINT version)
{
	if (version == 0 || version > UNIT_APPLE2_VER)
		throw std::runtime_error(SS_YAML_KEY_UNIT ": Apple2: Version mismatch");

	std::string model = yamlLoadHelper.LoadString(SS_YAML_KEY_MODEL);
	SetApple2Type( ParseApple2Type(model) );	// NB. Sets default main CPU type

	CpuLoadSnapshot(yamlLoadHelper, version);	// NB. Overrides default main CPU type

	JoyLoadSnapshot(yamlLoadHelper, version);
	KeybLoadSnapshot(yamlLoadHelper, version);
	SpkrLoadSnapshot(yamlLoadHelper);
	GetVideo().VideoLoadSnapshot(yamlLoadHelper, version);
	MemLoadSnapshot(yamlLoadHelper, version);
}

//---

static void ParseSlots(YamlLoadHelper& yamlLoadHelper, UINT unitVersion)
{
	if (unitVersion != UNIT_SLOTS_VER)
		throw std::runtime_error(SS_YAML_KEY_UNIT ": Slots: Version mismatch");

	while (1)
	{
		std::string scalar = yamlLoadHelper.GetMapNextSlotNumber();
		if (scalar.empty())
			break;	// done all slots

		const int slot = strtoul(scalar.c_str(), NULL, 10);	// NB. aux slot supported as a different "unit"
															// NB. slot-0 only supported for Apple II or II+ (or similar clones)
		if (slot < SLOT0 || slot > SLOT7)
			throw std::runtime_error("Slots: Invalid slot #: " + scalar);

		yamlLoadHelper.GetSubMap(scalar);

		std::string card = yamlLoadHelper.LoadString(SS_YAML_KEY_CARD);
		UINT cardVersion = yamlLoadHelper.LoadUint(SS_YAML_KEY_VERSION);

		if (!yamlLoadHelper.GetSubMap(std::string(SS_YAML_KEY_STATE), true))	// NB. For some cards, State can be null
			throw std::runtime_error(SS_YAML_KEY_UNIT ": Expected sub-map name: " SS_YAML_KEY_STATE);

		SS_CARDTYPE type = Card::GetCardType(card);
		bool bRes = false;

		if (slot == SLOT0)
		{
			SetExpansionMemType(type);	// calls GetCardMgr().Insert() & InsertAux()
		}
		else
		{
			GetCardMgr().Insert(slot, type);
		}

		bRes = GetCardMgr().GetRef(slot).LoadSnapshot(yamlLoadHelper, cardVersion);

		yamlLoadHelper.PopMap();
		yamlLoadHelper.PopMap();
	}

}

//---

static void ParseUnit(void)
{
	yamlHelper.GetMapStartEvent();

	YamlLoadHelper yamlLoadHelper(yamlHelper);

	std::string unit = yamlLoadHelper.LoadString(SS_YAML_KEY_TYPE);
	UINT unitVersion = yamlLoadHelper.LoadUint(SS_YAML_KEY_VERSION);

	if (!yamlLoadHelper.GetSubMap(std::string(SS_YAML_KEY_STATE)))
		throw std::runtime_error(SS_YAML_KEY_UNIT ": Expected sub-map name: " SS_YAML_KEY_STATE);

	if (unit == GetSnapshotUnitApple2Name())
	{
		ParseUnitApple2(yamlLoadHelper, unitVersion);

		if (unitVersion < 6) MemInsertNoSlotClock();	// NSC always inserted
		else				 MemRemoveNoSlotClock();	// NSC only add if there's a misc unit
	}
	else if (unit == MemGetSnapshotUnitAuxSlotName())
	{
		MemLoadSnapshotAux(yamlLoadHelper, unitVersion);
	}
	else if (unit == GetSnapshotUnitSlotsName())
	{
		ParseSlots(yamlLoadHelper, unitVersion);
	}
	else if (unit == GetSnapshotUnitGameIOConnectorName())
	{
		CopyProtectionDongleLoadSnapshot(yamlLoadHelper, unitVersion, UNIT_GAME_IO_CONNECTOR_VER);
	}
	else if (unit == GetSnapshotUnitMiscName())
	{
		// NB. could extend for other misc devices - see how ParseSlots() calls GetMapNextSlotNumber()
		NoSlotClockLoadSnapshot(yamlLoadHelper);
	}
	else
	{
		throw std::runtime_error(SS_YAML_KEY_UNIT ": Unknown type: " + unit);
	}
}

static void Snapshot_LoadState_v2(void)
{
	bool restart = false;	// Only need to restart if any VM state has change
	HCURSOR oldcursor = SetCursor(LoadCursor(0,IDC_WAIT));

	FrameBase& frame = GetFrame();

	try
	{
		if (!yamlHelper.InitParser(g_strSaveStatePathname.c_str()))
			throw std::runtime_error("Failed to initialize parser or open file: " + g_strSaveStatePathname);

		if (yamlHelper.ParseFileHdr(SS_YAML_VALUE_AWSS) != SS_FILE_VER)
			throw std::runtime_error("Version mismatch");

		//

		restart = true;

		//m_ConfigNew.m_bEnableTheFreezesF8Rom = ?;	// todo: when support saving config

		for (UINT slot = SLOT0; slot < NUM_SLOTS; slot++)
			GetCardMgr().Remove(slot);
		GetCardMgr().RemoveAux();

		SetCopyProtectionDongleType(DT_EMPTY);

		MemReset();							// Also calls CpuInitialize()
		GetPravets().Reset();

		KeybReset();
		GetVideo().SetVidHD(false);			// Set true later only if VidHDCard is instantiated
		GetVideo().VideoResetState();
		GetVideo().SetVideoRefreshRate(VR_60HZ);	// Default to 60Hz as older save-states won't contain refresh rate

		MockingboardCardManager &mockingboardCardManager = GetCardMgr().GetMockingboardCardMgr();
		mockingboardCardManager.InitializeForLoadingSnapshot(); // GH#609

#ifdef USE_SPEECH_API
		g_Speech.Reset();
#endif

		std::string scalar;
		while(yamlHelper.GetScalar(scalar))
		{
			if (scalar == SS_YAML_KEY_UNIT)
				ParseUnit();
			else
				throw std::runtime_error("Unknown top-level scalar: " + scalar);
		}

		// Refresh the volume of any new Mockingboard card (and its SSI263 or SC01 chips)
		mockingboardCardManager.SetVolume(mockingboardCardManager.GetVolume(), GetPropertySheet().GetVolumeMax());
		mockingboardCardManager.SetCumulativeCycles();

		frame.SetLoadedSaveStateFlag(true);

		// NB. The following disparity should be resolved:
		// . A change in h/w via the Configuration property sheets results in a the VM completely restarting (via WM_USER_RESTART)
		// . A change in h/w via loading a save-state avoids this VM restart
		// The latter is the desired approach (as the former needs a "power-on" / F2 to start things again)

		const CConfigNeedingRestart configNew = CConfigNeedingRestart::Create();
		GetPropertySheet().ApplyNewConfigFromSnapshot(configNew);	// Saves new state to Registry (not slot/cards though)

		MemInitializeFromSnapshot();

		DebugReset();
		if (g_nAppMode == MODE_DEBUG)
			DebugDisplay(TRUE);

		frame.Initialize(false);	// don't reset the video state
		frame.ResizeWindow();

		// g_Apple2Type may've changed: so reload button bitmaps & redraw frame (title, buttons, leds, etc)
		frame.FrameUpdateApple2Type();	// NB. Calls VideoRedrawScreen()
	}
	catch(const std::exception & szMessage)
	{
		frame.FrameMessageBox(
					szMessage.what(),
					"Load State",
					MB_ICONEXCLAMATION | MB_SETFOREGROUND);

		if (restart)
			frame.Restart();		// Power-cycle VM (undoing all the new state just loaded)
	}

	SetCursor(oldcursor);
	yamlHelper.FinaliseParser();
}

void Snapshot_LoadState()
{
	const std::string ext_aws = (".aws");
	const size_t pos = g_strSaveStatePathname.size() - ext_aws.size();
	if (g_strSaveStatePathname.find(ext_aws, pos) != std::string::npos)	// find ".aws" at end of pathname
	{
		GetFrame().FrameMessageBox(
					"Save-state v1 no longer supported.\n"
					"Please load using AppleWin 1.27, and re-save as a v2 state file.",
					"Load State",
					MB_ICONEXCLAMATION | MB_SETFOREGROUND);

		return;
	}

	LogFileOutput("Loading Save-State from %s\n", g_strSaveStatePathname.c_str());
	Snapshot_LoadState_v2();
}

//-----------------------------------------------------------------------------

void Snapshot_SaveState(void)
{
	LogFileOutput("Saving Save-State to %s\n", g_strSaveStatePathname.c_str());
	try
	{
		YamlSaveHelper yamlSaveHelper(g_strSaveStatePathname);
		yamlSaveHelper.FileHdr(SS_FILE_VER);

		// Unit: Apple2
		{
			yamlSaveHelper.UnitHdr(GetSnapshotUnitApple2Name(), UNIT_APPLE2_VER);
			YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", SS_YAML_KEY_STATE);

			yamlSaveHelper.Save("%s: %s\n", SS_YAML_KEY_MODEL, GetApple2TypeAsString().c_str());
			CpuSaveSnapshot(yamlSaveHelper);
			JoySaveSnapshot(yamlSaveHelper);
			KeybSaveSnapshot(yamlSaveHelper);
			SpkrSaveSnapshot(yamlSaveHelper);
			GetVideo().VideoSaveSnapshot(yamlSaveHelper);
			MemSaveSnapshot(yamlSaveHelper);
		}

		// Unit: Aux slot
		MemSaveSnapshotAux(yamlSaveHelper);

		// Unit: Slots
		{
			yamlSaveHelper.UnitHdr(GetSnapshotUnitSlotsName(), UNIT_SLOTS_VER);
			YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", SS_YAML_KEY_STATE);

			GetCardMgr().SaveSnapshot(yamlSaveHelper);
		}

		// Unit: Game I/O Connector
		if (GetCopyProtectionDongleType() != DT_EMPTY)
		{
			yamlSaveHelper.UnitHdr(GetSnapshotUnitGameIOConnectorName(), UNIT_GAME_IO_CONNECTOR_VER);
			YamlSaveHelper::Label unit(yamlSaveHelper, "%s:\n", SS_YAML_KEY_STATE);

			CopyProtectionDongleSaveSnapshot(yamlSaveHelper);
		}

		// Miscellaneous
		if (MemHasNoSlotClock())
		{
			yamlSaveHelper.UnitHdr(GetSnapshotUnitMiscName(), UNIT_MISC_VER);
			YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", SS_YAML_KEY_STATE);

			NoSlotClockSaveSnapshot(yamlSaveHelper);
		}
	}
	catch(const std::exception & szMessage)
	{
		GetFrame().FrameMessageBox(
					szMessage.what(),
					"Save State",
					MB_ICONEXCLAMATION | MB_SETFOREGROUND);
	}
}

//-----------------------------------------------------------------------------

void Snapshot_Startup()
{
	static bool bDone = false;

	if(!g_bSaveStateOnExit || bDone)
		return;

	Snapshot_LoadState();

	bDone = true;	// Prevents a g_bRestart from loading an old save-state
}

void Snapshot_Shutdown()
{
	static bool bDone = false;

	_ASSERT(!bDone);
	_ASSERT(!g_bRestart);
	if(!g_bSaveStateOnExit || bDone)
		return;

	Snapshot_SaveState();

	bDone = true;	// Debug flag: this func should only be called once, and never on a g_bRestart
}
