
#ifdef HAVE_LUA

#include "port.h"
#include "snes9x.h"
#include "display.h"
#include "ppu.h"
#include "movie.h"
#include "snapshot.h"
#include "pixform.h"
#include "screenshot.h"
#include "controls.h"
#include "lua-engine.h"
#include <assert.h>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include "zlib.h"

#ifdef __WIN32__
#define NOMINMAX
#include <windows.h>
#include <direct.h>
#include "win32/wsnes9x.h"
#include "win32/render.h"

#define g_hWnd GUI.hWnd

#ifdef UNICODE
#undef fopen
#define fopen fopenA
#undef open
#define open openA
#undef _access
#define _access _accessA
#undef rename
#define rename renameA
#undef _unlink
#define _unlink _unlinkA
#undef _chdir
#define _chdir _chdirA
#undef _mkdir
#define _mkdir _mkdirA
#undef _rmdir
#define _rmdir _rmdirA
#undef _splitpath
#define _splitpath _splitpathA
#undef _makepath
#define _makepath _makepathA
//#undef strrchr
//#define strrchr strrchrA
#endif

#else // __WIN32

#define FORCEINLINE __inline__ __attribute__((always_inline))
#define ENSURE_FORCEINLINE __attribute__((always_inline)) // inline or die

int vscprintf (const char * format, va_list pargs)
{ 
    int retval; 
    va_list argcopy;
    va_copy(argcopy, pargs); 
    retval = vsnprintf(NULL, 0, format, argcopy); 
    va_end(argcopy); 
    return retval;
}

#define MAX_PATH PATH_MAX
#define _chdir chdir

#endif

bool g_disableStatestateWarnings = false;
bool g_onlyCallSavestateCallbacks = false;

// the emulator must provide these so that we can implement
// the various functions the user can call from their lua script
// (this interface with the emulator needs cleanup, I know)
// adapted from gens-rr, nitsuja + upthorn
extern int (*Update_Frame)();
extern int (*Update_Frame_Fast)();

extern "C" {
	#include "lua.h"
	#include "lauxlib.h"
	#include "lualib.h"
	#include "lstate.h"
};

enum SpeedMode
{
	SPEEDMODE_NORMAL,
	SPEEDMODE_NOTHROTTLE,
	SPEEDMODE_TURBO,
	SPEEDMODE_MAXIMUM,
};

struct LuaGUIData
{
	uint32 *data;
	int stridePix;
	int xOrigin, yOrigin;
	int xMin, yMin, xMax, yMax;
};

uint32 luaGuiDataBuf[SNES_WIDTH * SNES_HEIGHT_EXTENDED];

struct LuaContextInfo {
	lua_State* L; // the Lua state
	bool started; // script has been started and hasn't yet been terminated, although it may not be currently running
	bool running; // script is currently running code (either the main call to the script or the callbacks it registered)
	bool returned; // main call to the script has returned (but it may still be active if it registered callbacks)
	bool crashed; // true if script has errored out
	bool restart; // if true, tells the script-running code to restart the script when the script stops
	bool restartLater; // set to true when a still-running script is stopped so that RestartAllLuaScripts can know which scripts to restart
	unsigned int worryCount; // counts up as the script executes, gets reset when the application is able to process messages, triggers a warning prompt if it gets too high
	bool stopWorrying; // set to true if the user says to let the script run forever despite appearing to be frozen
	bool panic; // if set to true, tells the script to terminate as soon as it can do so safely (used because directly calling lua_close() or luaL_error() is unsafe in some contexts)
	bool ranExit; // used to prevent a registered exit callback from ever getting called more than once
	bool guiFuncsNeedDeferring; // true whenever GUI drawing would be cleared by the next emulation update before it would be visible, and thus needs to be deferred until after the next emulation update
	bool ranFrameAdvance; // false if emu.frameadvance() hasn't been called yet
	int transparencyModifier; // values less than 255 will scale down the opacity of whatever the GUI renders, values greater than 255 will increase the opacity of anything transparent the GUI renders
	SpeedMode speedMode; // determines how emu.frameadvance() acts
	char panicMessage [72]; // a message to print if the script terminates due to panic being set
	std::string lastFilename; // path to where the script last ran from so that restart can work (note: storing the script in memory instead would not be useful because we always want the most up-to-date script from file)
	std::string nextFilename; // path to where the script should run from next, mainly used in case the restart flag is true
	unsigned int dataSaveKey; // crc32 of the save data key, used to decide which script should get which data... by default (if no key is specified) it's calculated from the script filename
	unsigned int dataLoadKey; // same as dataSaveKey but set through registerload instead of registersave if the two differ
	bool dataSaveLoadKeySet; // false if the data save keys are unset or set to their default value
	bool rerecordCountingDisabled; // true if this script has disabled rerecord counting for the savestates it loads
	std::vector<std::string> persistVars; // names of the global variables to persist, kept here so their associated values can be output when the script exits
	LuaSaveData newDefaultData; // data about the default state of persisted global variables, which we save on script exit so we can detect when the default value has changed to make it easier to reset persisted variables
	unsigned int numMemHooks; // number of registered memory functions (1 per hooked byte)
	LuaGUIData guiData;
	// callbacks into the lua window... these don't need to exist per context the way I'm using them, but whatever
	void(*print)(int uid, const char* str);
	void(*onstart)(int uid);
	void(*onstop)(int uid, bool statusOK);
};
std::map<int, LuaContextInfo*> luaContextInfo;
std::map<lua_State*, int> luaStateToUIDMap;
int g_numScriptsStarted = 0;
bool g_anyScriptsHighSpeed = false;
bool g_stopAllScriptsEnabled = true;

#define USE_INFO_STACK
#ifdef USE_INFO_STACK
	std::vector<LuaContextInfo*> infoStack;
	#define GetCurrentInfo() *infoStack.front() // should be faster but relies on infoStack correctly being updated to always have the current info in the first element
#else
	std::map<lua_State*, LuaContextInfo*> luaStateToContextMap;
	#define GetCurrentInfo() *luaStateToContextMap[L] // should always work but might be slower
#endif

//#define ASK_USER_ON_FREEZE // dialog on freeze is disabled now because it seems to be unnecessary, but this can be re-defined to enable it


static std::map<lua_CFunction, const char*> s_cFuncInfoMap;

// using this macro you can define a callable-from-Lua function
// while associating with it some information about its arguments.
// that information will show up if the user tries to print the function
// or otherwise convert it to a string.
// (for example, "writebyte=function(addr,value)" instead of "writebyte=function:0A403490")
// note that the user can always use addressof(func) if they want to retrieve the address.
#define DEFINE_LUA_FUNCTION(name, argstring) \
	static int name(lua_State* L); \
	static const char* name##_args = s_cFuncInfoMap[name] = argstring; \
	static int name(lua_State* L)

#ifdef _MSC_VER
	#define snprintf _snprintf
	#define vscprintf _vscprintf
#else
	#define stricmp strcasecmp
	#define strnicmp strncasecmp
	#define __forceinline __attribute__((always_inline))
#endif


static const char* luaCallIDStrings [] =
{
	"CALL_BEFOREEMULATION",
	"CALL_AFTEREMULATION",
	"CALL_AFTEREMULATIONGUI",
	"CALL_BEFOREEXIT",
	"CALL_BEFORESAVE",
	"CALL_AFTERLOAD",
	"CALL_ONSTART",

	"CALL_HOTKEY_1",
	"CALL_HOTKEY_2",
	"CALL_HOTKEY_3",
	"CALL_HOTKEY_4",
	"CALL_HOTKEY_5",
	"CALL_HOTKEY_6",
	"CALL_HOTKEY_7",
	"CALL_HOTKEY_8",
	"CALL_HOTKEY_9",
	"CALL_HOTKEY_10",
	"CALL_HOTKEY_11",
	"CALL_HOTKEY_12",
	"CALL_HOTKEY_13",
	"CALL_HOTKEY_14",
	"CALL_HOTKEY_15",
	"CALL_HOTKEY_16",
};
static int _makeSureWeHaveTheRightNumberOfStrings [sizeof(luaCallIDStrings)/sizeof(*luaCallIDStrings) == LUACALL_COUNT ? 1 : 0];

static const char* luaMemHookTypeStrings [] =
{
	"MEMHOOK_WRITE",
	"MEMHOOK_READ",
	"MEMHOOK_EXEC",

	"MEMHOOK_WRITE_SUB",
	"MEMHOOK_READ_SUB",
	"MEMHOOK_EXEC_SUB",
};
static int _makeSureWeHaveTheRightNumberOfStrings2 [sizeof(luaMemHookTypeStrings)/sizeof(*luaMemHookTypeStrings) == LUAMEMHOOK_COUNT ? 1 : 0];

void StopScriptIfFinished(int uid, bool justReturned = false);
void SetSaveKey(LuaContextInfo& info, const char* key);
void SetLoadKey(LuaContextInfo& info, const char* key);
void RefreshScriptStartedStatus();
void RefreshScriptSpeedStatus();

static char* rawToCString(lua_State* L, int idx=0);
static const char* toCString(lua_State* L, int idx=0);

static void CalculateMemHookRegions(LuaMemHookType hookType);

static int memory_registerHook(lua_State* L, LuaMemHookType hookType, int defaultSize)
{
	// get first argument: address
	unsigned int addr = luaL_checkinteger(L,1);
	if((addr & ~0xFFFFFF) == ~0xFFFFFF)
		addr &= 0xFFFFFF;

	// get optional second argument: size
	int size = defaultSize;
	int funcIdx = 2;
	if(lua_isnumber(L,2))
	{
		size = luaL_checkinteger(L,2);
		if(size < 0)
		{
			size = -size;
			addr -= size;
		}
		funcIdx++;
	}

	// check last argument: callback function
	bool clearing = lua_isnil(L,funcIdx);
	if(!clearing)
		luaL_checktype(L, funcIdx, LUA_TFUNCTION);
	lua_settop(L,funcIdx);

	// get the address-to-callback table for this hook type of the current script
	lua_getfield(L, LUA_REGISTRYINDEX, luaMemHookTypeStrings[hookType]);

	// count how many callback functions we'll be displacing
	int numFuncsAfter = clearing ? 0 : size;
	int numFuncsBefore = 0;
	for(unsigned int i = addr; i != addr+size; i++)
	{
		lua_rawgeti(L, -1, i);
		if(lua_isfunction(L, -1))
			numFuncsBefore++;
		lua_pop(L,1);
	}

	// put the callback function in the address slots
	for(unsigned int i = addr; i != addr+size; i++)
	{
		lua_pushvalue(L, -2);
		lua_rawseti(L, -2, i);
	}

	// adjust the count of active hooks
	LuaContextInfo& info = GetCurrentInfo();
	info.numMemHooks += numFuncsAfter - numFuncsBefore;

	// re-cache regions of hooked memory across all scripts
	CalculateMemHookRegions(hookType);

	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}

LuaMemHookType MatchHookTypeToCPU(lua_State* L, LuaMemHookType hookType)
{
	int cpuID = 0;

	int cpunameIndex = 0;
	if(lua_type(L,2) == LUA_TSTRING)
		cpunameIndex = 2;
	else if(lua_type(L,3) == LUA_TSTRING)
		cpunameIndex = 3;

	if(cpunameIndex)
	{
		const char* cpuName = lua_tostring(L, cpunameIndex);
		//if(stricmp(cpuName, "sa1") == 0)
		//	cpuID = 1;
		lua_remove(L, cpunameIndex);
	}

	switch(cpuID)
	{
	case 0: // 65c816:
		return hookType;

//	case 1: // sa1:
//		switch(hookType)
//		{
//		case LUAMEMHOOK_WRITE: return LUAMEMHOOK_WRITE_SUB;
//		case LUAMEMHOOK_READ: return LUAMEMHOOK_READ_SUB;
//		case LUAMEMHOOK_EXEC: return LUAMEMHOOK_EXEC_SUB;
//		}
	}
	return hookType;
}

DEFINE_LUA_FUNCTION(memory_registerwrite, "address,[size=1,][cpuname=\"main\",]func")
{
	return memory_registerHook(L, MatchHookTypeToCPU(L,LUAMEMHOOK_WRITE), 1);
}
DEFINE_LUA_FUNCTION(memory_registerread, "address,[size=1,][cpuname=\"main\",]func")
{
	return memory_registerHook(L, MatchHookTypeToCPU(L,LUAMEMHOOK_READ), 1);
}
DEFINE_LUA_FUNCTION(memory_registerexec, "address,[size=2,][cpuname=\"main\",]func")
{
	return memory_registerHook(L, MatchHookTypeToCPU(L,LUAMEMHOOK_EXEC), 2);
}


DEFINE_LUA_FUNCTION(emu_registerbefore, "func")
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEMULATION]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEMULATION]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}
DEFINE_LUA_FUNCTION(emu_registerafter, "func")
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATION]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATION]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}
DEFINE_LUA_FUNCTION(emu_registerexit, "func")
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}
DEFINE_LUA_FUNCTION(emu_registerstart, "func")
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_ONSTART]);
	lua_insert(L,1);
	lua_pushvalue(L,-1); // copy the function so we can also call it
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_ONSTART]);
	if (!lua_isnil(L,-1) && !Settings.StopEmulation)
		lua_call(L,0,0); // call the function now since the game has already started and this start function hasn't been called yet
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}
DEFINE_LUA_FUNCTION(gui_register, "func")
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATIONGUI]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATIONGUI]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}
DEFINE_LUA_FUNCTION(state_registersave, "func[,savekey]")
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	if (!lua_isnoneornil(L,2))
		SetSaveKey(GetCurrentInfo(), rawToCString(L,2));
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFORESAVE]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFORESAVE]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}
DEFINE_LUA_FUNCTION(state_registerload, "func[,loadkey]")
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	if (!lua_isnoneornil(L,2))
		SetLoadKey(GetCurrentInfo(), rawToCString(L,2));
	lua_settop(L,1);
	lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERLOAD]);
	lua_insert(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERLOAD]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 1;
}

DEFINE_LUA_FUNCTION(input_registerhotkey, "keynum,func")
{
	int hotkeyNumber = luaL_checkinteger(L,1);
	if(hotkeyNumber < 1 || hotkeyNumber > 16)
	{
		luaL_error(L, "input.registerhotkey(n,func) requires 1 <= n <= 16, but got n = %d.", hotkeyNumber);
		return 0;
	}
	else
	{
		const char* key = luaCallIDStrings[LUACALL_SCRIPT_HOTKEY_1 + hotkeyNumber-1];
		lua_getfield(L, LUA_REGISTRYINDEX, key);
		lua_replace(L,1);
		if (!lua_isnil(L,2))
			luaL_checktype(L, 2, LUA_TFUNCTION);
		lua_settop(L,2);
		lua_setfield(L, LUA_REGISTRYINDEX, key);
		StopScriptIfFinished(luaStateToUIDMap[L]);
		return 1;
	}
}

static int doPopup(lua_State* L, const char* deftype, const char* deficon)
{
	const char* str = toCString(L,1);
	const char* type = lua_type(L,2) == LUA_TSTRING ? lua_tostring(L,2) : deftype;
	const char* icon = lua_type(L,3) == LUA_TSTRING ? lua_tostring(L,3) : deficon;

	int itype = -1, iters = 0;
	while(itype == -1 && iters++ < 2)
	{
		if(!stricmp(type, "ok")) itype = 0;
		else if(!stricmp(type, "yesno")) itype = 1;
		else if(!stricmp(type, "yesnocancel")) itype = 2;
		else if(!stricmp(type, "okcancel")) itype = 3;
		else if(!stricmp(type, "abortretryignore")) itype = 4;
		else type = deftype;
	}
	assert(itype >= 0 && itype <= 4);
	if(!(itype >= 0 && itype <= 4)) itype = 0;

	int iicon = -1; iters = 0;
	while(iicon == -1 && iters++ < 2)
	{
		if(!stricmp(icon, "message") || !stricmp(icon, "notice")) iicon = 0;
		else if(!stricmp(icon, "question")) iicon = 1;
		else if(!stricmp(icon, "warning")) iicon = 2;
		else if(!stricmp(icon, "error")) iicon = 3;
		else icon = deficon;
	}
	assert(iicon >= 0 && iicon <= 3);
	if(!(iicon >= 0 && iicon <= 3)) iicon = 0;

	static const char * const titles [] = {"Notice", "Question", "Warning", "Error"};
	const char* answer = "ok";
#ifdef __WIN32__
	static const int etypes [] = {MB_OK, MB_YESNO, MB_YESNOCANCEL, MB_OKCANCEL, MB_ABORTRETRYIGNORE};
	static const int eicons [] = {MB_ICONINFORMATION, MB_ICONQUESTION, MB_ICONWARNING, MB_ICONERROR};
	int uid = luaStateToUIDMap[L];
	EnableWindow(g_hWnd, false);
//	if (Full_Screen)
//	{
//		while (ShowCursor(false) >= 0);
//		while (ShowCursor(true) < 0);
//	}
	int ianswer = MessageBoxA((HWND)uid, str, titles[iicon], etypes[itype] | eicons[iicon]);
	EnableWindow(g_hWnd, true);
	switch(ianswer)
	{
		case IDOK: answer = "ok"; break;
		case IDCANCEL: answer = "cancel"; break;
		case IDABORT: answer = "abort"; break;
		case IDRETRY: answer = "retry"; break;
		case IDIGNORE: answer = "ignore"; break;
		case IDYES: answer = "yes"; break;
		case IDNO: answer = "no"; break;
	}
#else
	// NYI (assume first answer for now)
	switch(itype)
	{
		case 0: case 3: answer = "ok"; break;
		case 1: case 2: answer = "yes"; break;
		case 4: answer = "abort"; break;
	}
#endif

	lua_pushstring(L, answer);
	return 1;
}

// string gui.popup(string message, string type = "ok", string icon = "message")
// string input.popup(string message, string type = "yesno", string icon = "question")
DEFINE_LUA_FUNCTION(gui_popup, "message[,type=\"ok\"[,icon=\"message\"]]")
{
	return doPopup(L, "ok", "message");
}
DEFINE_LUA_FUNCTION(input_popup, "message[,type=\"yesno\"[,icon=\"question\"]]")
{
	return doPopup(L, "yesno", "question");
}
static const char* FilenameFromPath(const char* path)
{
	const char* slash1 = strrchr(path, '\\');
	const char* slash2 = strrchr(path, '/');
	if(slash1) slash1++;
	if(slash2) slash2++;
	const char* rv = path;
	rv = std::max(rv, slash1);
	rv = std::max(rv, slash2);
	if(!rv) rv = "";
	return rv;
}

void TrimFilenameFromPath(char* path)
{
	char* slash1 = strrchr(path, '\\');
	char* slash2 = strrchr(path, '/');
	char* slash = slash1;
	if (slash == NULL || slash2 > slash) {
		slash = slash2;
	}
	if (slash != NULL) {
		*(slash + 1) = '\0';
	}
}


static void toCStringConverter(lua_State* L, int i, char*& ptr, int& remaining);

// compare the contents of two items on the Lua stack to determine if they differ
// only works for relatively simple, saveable items (numbers, strings, bools, nil, and possibly-nested tables of those, up to a certain max length)
// not the best implementation, but good enough for what it's currently used for
static bool luaValueContentsDiffer(lua_State* L, int idx1, int idx2)
{
	static const int maxLen = 8192;
	static char str1[maxLen];
	static char str2[maxLen];
	str1[0] = 0;
	str2[0] = 0;
	char* ptr1 = str1;
	char* ptr2 = str2;
	int remaining1 = maxLen;
	int remaining2 = maxLen;
	toCStringConverter(L, idx1, ptr1, remaining1);
	toCStringConverter(L, idx2, ptr2, remaining2);
	return (remaining1 != remaining2) || (strcmp(str1,str2) != 0);
}


void Get_State_File_Name(char *name, int stateNumber = 0)
{
	char drive[_MAX_DRIVE + 1], dir[_MAX_DIR + 1], def[_MAX_FNAME + 1], ext[_MAX_EXT + 1];

	_splitpath(Memory.ROMFilename, drive, dir, def, ext);
	sprintf(name, "%s%s%s.%03d", S9xGetDirectory(SNAPSHOT_DIR), SLASH_STR, def, stateNumber);
}

// fills output with the path
// also returns a pointer to the first character in the filename (non-directory) part of the path
static char* ConstructScriptSaveDataPath(char* output, int bufferSize, LuaContextInfo& info)
{
	Get_State_File_Name(output);
	char* slash1 = strrchr(output, '\\');
	char* slash2 = strrchr(output, '/');
	if(slash1) slash1[1] = '\0';
	if(slash2) slash2[1] = '\0';
	char* rv = output + strlen(output);
	strncat(output, "u.", bufferSize-(strlen(output)+1));
	if(!info.dataSaveLoadKeySet)
		strncat(output, FilenameFromPath(info.lastFilename.c_str()), bufferSize-(strlen(output)+1));
	else
		snprintf(output+strlen(output), bufferSize-(strlen(output)+1), "%X", info.dataSaveKey);
	strncat(output, ".luasav", bufferSize-(strlen(output)+1));
	return rv;
}

// emu.persistglobalvariables({
//   variable1 = defaultvalue1,
//   variable2 = defaultvalue2,
//   etc
// })
// takes a table with variable names as the keys and default values as the values,
// and defines each of those variables names as a global variable,
// setting them equal to the values they had the last time the script exited,
// or (if that isn't available) setting them equal to the provided default values.
// as a special case, if you want the default value for a variable to be nil,
// then put the variable name alone in quotes as an entry in the table without saying "= nil".
// this special case is because tables in lua don't store nil valued entries.
// also, if you change the default value that will reset the variable to the new default.
DEFINE_LUA_FUNCTION(emu_persistglobalvariables, "variabletable")
{
	int uid = luaStateToUIDMap[L];
	LuaContextInfo& info = GetCurrentInfo();

	// construct a path we can load the persistent variables from
	char path [1024] = {0};
	char* pathTypeChrPtr = ConstructScriptSaveDataPath(path, 1024, info);

	// load the previously-saved final variable values from file
	LuaSaveData exitData;
	{
		*pathTypeChrPtr = 'e';
		FILE* persistFile = fopen(path, "rb");
		if(persistFile)
		{
			exitData.ImportRecords(persistFile);
			fclose(persistFile);
		}
	}

	// load the previously-saved default variable values from file
	LuaSaveData defaultData;
	{
		*pathTypeChrPtr = 'd';
		FILE* defaultsFile = fopen(path, "rb");
		if(defaultsFile)
		{
			defaultData.ImportRecords(defaultsFile);
			fclose(defaultsFile);
		}
	}

	// loop through the passed-in variables,
	// exposing a global variable to the script for each one
	// while also keeping a record of their names
	// so we can save them (to the persistFile) later when the script exits
	int numTables = lua_gettop(L);
	for(int i = 1; i <= numTables; i++)
	{
		luaL_checktype(L, i, LUA_TTABLE);

		lua_pushnil(L); // before first key
		int keyIndex = lua_gettop(L);
		int valueIndex = keyIndex + 1;
		while(lua_next(L, i))
		{
			int keyType = lua_type(L, keyIndex);
			int valueType = lua_type(L, valueIndex);
			if(keyType == LUA_TSTRING && valueType <= LUA_TTABLE && valueType != LUA_TLIGHTUSERDATA)
			{
				// variablename = defaultvalue,

				// duplicate the key first because lua_next() needs to eat that
				lua_pushvalue(L, keyIndex);
				lua_insert(L, keyIndex);
			}
			else if(keyType == LUA_TNUMBER && valueType == LUA_TSTRING)
			{
				// "variablename",
				// or [index] = "variablename",

				// defaultvalue is assumed to be nil
				lua_pushnil(L);
			}
			else
			{
				luaL_error(L, "'%s' = '%s' entries are not allowed in the table passed to emu.persistglobalvariables()", lua_typename(L,keyType), lua_typename(L,valueType));
			}

			int varNameIndex = valueIndex;
			int defaultIndex = valueIndex+1;

			// keep track of the variable name for later
			const char* varName = lua_tostring(L, varNameIndex);
			info.persistVars.push_back(varName);
			unsigned int varNameCRC = crc32(0, (const unsigned char*)varName, strlen(varName));
			info.newDefaultData.SaveRecordPartial(uid, varNameCRC, defaultIndex);

			// load the previous default value for this variable if it exists.
			// if the new default is different than the old one,
			// assume the user wants to set the value to the new default value
			// instead of the previously-saved exit value.
			bool attemptPersist = true;
			defaultData.LoadRecord(uid, varNameCRC, 1);
			lua_pushnil(L);
			if(luaValueContentsDiffer(L, defaultIndex, defaultIndex+1))
				attemptPersist = false;
			lua_settop(L, defaultIndex);

			if(attemptPersist)
			{
				// load the previous saved value for this variable if it exists
				exitData.LoadRecord(uid, varNameCRC, 1);
				if(lua_gettop(L) > defaultIndex)
					lua_remove(L, defaultIndex); // replace value with loaded record
				lua_settop(L, defaultIndex);
			}

			// set the global variable
			lua_settable(L, LUA_GLOBALSINDEX);

			assert(lua_gettop(L) == keyIndex);
		}
	}

	return 0;
}

static const char* deferredGUIIDString = "lazygui";
static const char* deferredJoySetIDString = "lazyjoy";
#define MAX_DEFERRED_COUNT 16384

// store the most recent C function call from Lua (and all its arguments)
// for later evaluation
void DeferFunctionCall(lua_State* L, const char* idstring)
{
	// there might be a cleaner way of doing this using lua_pushcclosure and lua_getref

	int num = lua_gettop(L);

	// get the C function pointer
	//lua_CFunction cf = lua_tocfunction(L, -(num+1));
	lua_CFunction cf = (L->ci->func)->value.gc->cl.c.f;
	assert(cf);
	lua_pushcfunction(L,cf);

	// make a list of the function and its arguments (and also pop those arguments from the stack)
	lua_createtable(L, num+1, 0);
	lua_insert(L, 1);
	for(int n = num+1; n > 0; n--)
		lua_rawseti(L, 1, n);

	// put the list into a global array
	lua_getfield(L, LUA_REGISTRYINDEX, idstring);
	lua_insert(L, 1);
	int curSize = lua_objlen(L, 1);
	lua_rawseti(L, 1, curSize+1);

	// clean the stack
	lua_settop(L, 0);
}
void CallDeferredFunctions(lua_State* L, const char* idstring)
{
	lua_settop(L, 0);
	lua_getfield(L, LUA_REGISTRYINDEX, idstring);
	int numCalls = lua_objlen(L, 1);
	for(int i = 1; i <= numCalls; i++)
	{
        lua_rawgeti(L, 1, i);  // get the function+arguments list
		int listSize = lua_objlen(L, 2);

		// push the arguments and the function
		for(int j = 1; j <= listSize; j++)
			lua_rawgeti(L, 2, j);

		// get and pop the function
		lua_CFunction cf = lua_tocfunction(L, -1);
		lua_pop(L, 1);

		// shift first argument to slot 1 and call the function
		lua_remove(L, 2);
		lua_remove(L, 1);
		cf(L);

		// prepare for next iteration
		lua_settop(L, 0);
		lua_getfield(L, LUA_REGISTRYINDEX, idstring);
	}

	// clear the list of deferred functions
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, idstring);
	LuaContextInfo& info = GetCurrentInfo();

	// clean the stack
	lua_settop(L, 0);
}

bool DeferGUIFuncIfNeeded(lua_State* L)
{
	return false;

	LuaContextInfo& info = GetCurrentInfo();
	if(info.speedMode == SPEEDMODE_MAXIMUM)
	{
		// if the mode is "maximum" then discard all GUI function calls
		// and pretend it was because we deferred them
		return true;
	}
	if(info.guiFuncsNeedDeferring)
	{
		// defer whatever function called this one until later
		DeferFunctionCall(L, deferredGUIIDString);
		return true;
	}

	// ok to run the function right now
	return false;
}

void worry(lua_State* L, int intensity)
{
	LuaContextInfo& info = GetCurrentInfo();
	info.worryCount += intensity;
}

static inline bool isalphaorunderscore(char c)
{
	return isalpha(c) || c == '_';
}

static std::vector<const void*> s_tableAddressStack; // prevents infinite recursion of a table within a table (when cycle is found, print something like table:parent)
static std::vector<const void*> s_metacallStack; // prevents infinite recursion if something's __tostring returns another table that contains that something (when cycle is found, print the inner result without using __tostring)

#define APPENDPRINT { int _n = snprintf(ptr, remaining,
#define END ); if(_n >= 0) { ptr += _n; remaining -= _n; } else { remaining = 0; } }
static void toCStringConverter(lua_State* L, int i, char*& ptr, int& remaining)
{
	if(remaining <= 0)
		return;

	const char* str = ptr; // for debugging

	// if there is a __tostring metamethod then call it
	int usedMeta = luaL_callmeta(L, i, "__tostring");
	if(usedMeta)
	{
		std::vector<const void*>::const_iterator foundCycleIter = std::find(s_metacallStack.begin(), s_metacallStack.end(), lua_topointer(L,i));
		if(foundCycleIter != s_metacallStack.end())
		{
			lua_pop(L, 1);
			usedMeta = false;
		}
		else
		{
			s_metacallStack.push_back(lua_topointer(L,i));
			i = lua_gettop(L);
		}
	}

	switch(lua_type(L, i))
	{
		case LUA_TNONE: break;
		case LUA_TNIL: APPENDPRINT "nil" END break;
		case LUA_TBOOLEAN: APPENDPRINT lua_toboolean(L,i) ? "true" : "false" END break;
		case LUA_TSTRING: APPENDPRINT "%s",lua_tostring(L,i) END break;
		case LUA_TNUMBER: APPENDPRINT "%.12Lg",lua_tonumber(L,i) END break;
		case LUA_TFUNCTION: 
			if((L->base + i-1)->value.gc->cl.c.isC)
			{
				lua_CFunction func = lua_tocfunction(L, i);
				std::map<lua_CFunction, const char*>::iterator iter = s_cFuncInfoMap.find(func);
				if(iter == s_cFuncInfoMap.end())
					goto defcase;
				APPENDPRINT "function(%s)", iter->second END 
			}
			else
			{
				APPENDPRINT "function(" END 
				Proto* p = (L->base + i-1)->value.gc->cl.l.p;
				int numParams = p->numparams + (p->is_vararg?1:0);
				for (int n=0; n<p->numparams; n++)
				{
					APPENDPRINT "%s", getstr(p->locvars[n].varname) END 
					if(n != numParams-1)
						APPENDPRINT "," END
				}
				if(p->is_vararg)
					APPENDPRINT "..." END
				APPENDPRINT ")" END
			}
			break;
defcase:default: APPENDPRINT "%s:%p",luaL_typename(L,i),lua_topointer(L,i) END break;
		case LUA_TTABLE:
		{
			// first make sure there's enough stack space
			if(!lua_checkstack(L, 4))
			{
				// note that even if lua_checkstack never returns false,
				// that doesn't mean we didn't need to call it,
				// because calling it retrieves stack space past LUA_MINSTACK
				goto defcase;
			}

			std::vector<const void*>::const_iterator foundCycleIter = std::find(s_tableAddressStack.begin(), s_tableAddressStack.end(), lua_topointer(L,i));
			if(foundCycleIter != s_tableAddressStack.end())
			{
				int parentNum = s_tableAddressStack.end() - foundCycleIter;
				if(parentNum > 1)
					APPENDPRINT "%s:parent^%d",luaL_typename(L,i),parentNum END
				else
					APPENDPRINT "%s:parent",luaL_typename(L,i) END
			}
			else
			{
				s_tableAddressStack.push_back(lua_topointer(L,i));
				struct Scope { ~Scope(){ s_tableAddressStack.pop_back(); } } scope;

				APPENDPRINT "{" END

				lua_pushnil(L); // first key
				int keyIndex = lua_gettop(L);
				int valueIndex = keyIndex + 1;
				bool first = true;
				bool skipKey = true; // true if we're still in the "array part" of the table
				lua_Number arrayIndex = (lua_Number)0;
				while(lua_next(L, i))
				{
					if(first)
						first = false;
					else
						APPENDPRINT ", " END
					if(skipKey)
					{
						arrayIndex += (lua_Number)1;
						bool keyIsNumber = (lua_type(L, keyIndex) == LUA_TNUMBER);
						skipKey = keyIsNumber && (lua_tonumber(L, keyIndex) == arrayIndex);
					}
					if(!skipKey)
					{
						bool keyIsString = (lua_type(L, keyIndex) == LUA_TSTRING);
						bool invalidLuaIdentifier = (!keyIsString || !isalphaorunderscore(*lua_tostring(L, keyIndex)));
						if(invalidLuaIdentifier)
							if(keyIsString)
								APPENDPRINT "['" END
							else
								APPENDPRINT "[" END

						toCStringConverter(L, keyIndex, ptr, remaining); // key

						if(invalidLuaIdentifier)
							if(keyIsString)
								APPENDPRINT "']=" END
							else
								APPENDPRINT "]=" END
						else
							APPENDPRINT "=" END
					}

					bool valueIsString = (lua_type(L, valueIndex) == LUA_TSTRING);
					if(valueIsString)
						APPENDPRINT "'" END

					toCStringConverter(L, valueIndex, ptr, remaining); // value

					if(valueIsString)
						APPENDPRINT "'" END

					lua_pop(L, 1);

					if(remaining <= 0)
					{
						lua_settop(L, keyIndex-1); // stack might not be clean yet if we're breaking early
						break;
					}
				}
				APPENDPRINT "}" END
			}
		}	break;
	}

	if(usedMeta)
	{
		s_metacallStack.pop_back();
		lua_pop(L, 1);
	}
}

static const int s_tempStrMaxLen = 64 * 1024;
static char s_tempStr [s_tempStrMaxLen];

static char* rawToCString(lua_State* L, int idx)
{
	int a = idx>0 ? idx : 1;
	int n = idx>0 ? idx : lua_gettop(L);

	char* ptr = s_tempStr;
	*ptr = 0;

	int remaining = s_tempStrMaxLen;
	for(int i = a; i <= n; i++)
	{
		toCStringConverter(L, i, ptr, remaining);
		if(i != n)
			APPENDPRINT " " END
	}

	if(remaining < 3)
	{
		while(remaining < 6)
			remaining++, ptr--;
		APPENDPRINT "..." END
	}
	APPENDPRINT "\r\n" END
	// the trailing newline is so print() can avoid having to do wasteful things to print its newline
	// (string copying would be wasteful and calling info.print() twice can be extremely slow)
	// at the cost of functions that don't want the newline needing to trim off the last two characters
	// (which is a very fast operation and thus acceptable in this case)

	return s_tempStr;
}
#undef APPENDPRINT
#undef END


// replacement for luaB_tostring() that is able to show the contents of tables (and formats numbers better, and show function prototypes)
// can be called directly from lua via tostring(), assuming tostring hasn't been reassigned
DEFINE_LUA_FUNCTION(tostring, "...")
{
	char* str = rawToCString(L);
	str[strlen(str)-2] = 0; // hack: trim off the \r\n (which is there to simplify the print function's task)
	lua_pushstring(L, str);
	return 1;
}

// like rawToCString, but will check if the global Lua function tostring()
// has been replaced with a custom function, and call that instead if so
static const char* toCString(lua_State* L, int idx)
{
	int a = idx>0 ? idx : 1;
	int n = idx>0 ? idx : lua_gettop(L);
	lua_getglobal(L, "tostring");
	lua_CFunction cf = lua_tocfunction(L,-1);
	if(cf == tostring) // optimization: if using our own C tostring function, we can bypass the call through Lua and all the string object allocation that would entail
	{
		lua_pop(L,1);
		return rawToCString(L, idx);
	}
	else // if the user overrided the tostring function, we have to actually call it and store the temporarily allocated string it returns
	{
		lua_pushstring(L, "");
		for (int i=a; i<=n; i++) {
			lua_pushvalue(L, -2);  // function to be called
			lua_pushvalue(L, i);   // value to print
			lua_call(L, 1, 1);
			if(lua_tostring(L, -1) == NULL)
				luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));
			lua_pushstring(L, (i<n) ? " " : "\r\n");
			lua_concat(L, 3);
		}
		const char* str = lua_tostring(L, -1);
		strncpy(s_tempStr, str, s_tempStrMaxLen);
		s_tempStr[s_tempStrMaxLen-1] = 0;
		lua_pop(L, 2);
		return s_tempStr;
	}
}

// replacement for luaB_print() that goes to the appropriate textbox instead of stdout
DEFINE_LUA_FUNCTION(print, "...")
{
	const char* str = toCString(L);

	int uid = luaStateToUIDMap[L];
	LuaContextInfo& info = GetCurrentInfo();

	if(info.print)
		info.print(uid, str);
	else
		puts(str);

	worry(L, 100);
	return 0;
}



DEFINE_LUA_FUNCTION(emu_message, "str")
{
	const char* str = toCString(L);

	static char msg[1024];
	strncpy(msg, str, 1024);
	msg[1023] = '\0';

	S9xSetInfoString(msg);
	return 0;
}


// provides an easy way to copy a table from Lua
// (simple assignment only makes an alias, but sometimes an independent table is desired)
// currently this function only performs a shallow copy,
// but I think it should be changed to do a deep copy (possibly of configurable depth?)
// that maintains the internal table reference structure
DEFINE_LUA_FUNCTION(copytable, "origtable")
{
	int origIndex = 1; // we only care about the first argument
	int origType = lua_type(L, origIndex);
	if(origType == LUA_TNIL)
	{
		lua_pushnil(L);
		return 1;
	}
	if(origType != LUA_TTABLE)
	{
		luaL_typerror(L, 1, lua_typename(L, LUA_TTABLE));
		lua_pushnil(L);
		return 1;
	}
	
	lua_createtable(L, lua_objlen(L,1), 0);
	int copyIndex = lua_gettop(L);

	lua_pushnil(L); // first key
	int keyIndex = lua_gettop(L);
	int valueIndex = keyIndex + 1;

	while(lua_next(L, origIndex))
	{
		lua_pushvalue(L, keyIndex);
		lua_pushvalue(L, valueIndex);
		lua_rawset(L, copyIndex); // copytable[key] = value
		lua_pop(L, 1);
	}

	// copy the reference to the metatable as well, if any
	if(lua_getmetatable(L, origIndex))
		lua_setmetatable(L, copyIndex);

	return 1; // return the new table
}

// because print traditionally shows the address of tables,
// and the print function I provide instead shows the contents of tables,
// I also provide this function
// (otherwise there would be no way to see a table's address, AFAICT)
DEFINE_LUA_FUNCTION(addressof, "table_or_function")
{
	const void* ptr = lua_topointer(L,-1);
	lua_pushinteger(L, (lua_Integer)ptr);
	return 1;
}

// the following bit operations are ported from LuaBitOp 1.0.12
// because it can handle the sign bit (bit 31) correctly.

/*
** Lua BitOp -- a bit operations library for Lua 5.1/5.2.
** http://bitop.luajit.org/
**
** Copyright (C) 2008-2012 Mike Pall. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: http://www.opensource.org/licenses/mit-license.php ]
*/

#ifdef _MSC_VER
/* MSVC is stuck in the last century and doesn't have C99's stdint.h. */
typedef __int32 int32_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#else
#include <stdint.h>
#endif

typedef int32_t SBits;
typedef uint32_t UBits;

typedef union {
  lua_Number n;
#ifdef LUA_NUMBER_DOUBLE
  uint64_t b;
#else
  UBits b;
#endif
} BitNum;

/* Convert argument to bit type. */
static UBits barg(lua_State *L, int idx)
{
  BitNum bn;
  UBits b;
#if LUA_VERSION_NUM < 502
  bn.n = lua_tonumber(L, idx);
#else
  bn.n = luaL_checknumber(L, idx);
#endif
#if defined(LUA_NUMBER_DOUBLE)
  bn.n += 6755399441055744.0;  /* 2^52+2^51 */
#ifdef SWAPPED_DOUBLE
  b = (UBits)(bn.b >> 32);
#else
  b = (UBits)bn.b;
#endif
#elif defined(LUA_NUMBER_INT) || defined(LUA_NUMBER_LONG) || \
      defined(LUA_NUMBER_LONGLONG) || defined(LUA_NUMBER_LONG_LONG) || \
      defined(LUA_NUMBER_LLONG)
  if (sizeof(UBits) == sizeof(lua_Number))
    b = bn.b;
  else
    b = (UBits)(SBits)bn.n;
#elif defined(LUA_NUMBER_FLOAT)
#error "A 'float' lua_Number type is incompatible with this library"
#else
#error "Unknown number type, check LUA_NUMBER_* in luaconf.h"
#endif
#if LUA_VERSION_NUM < 502
  if (b == 0 && !lua_isnumber(L, idx)) {
    luaL_typerror(L, idx, "number");
  }
#endif
  return b;
}

/* Return bit type. */
#define BRET(b)  lua_pushnumber(L, (lua_Number)(SBits)(b)); return 1;

DEFINE_LUA_FUNCTION(bit_tobit, "x") { BRET(barg(L, 1)) }
DEFINE_LUA_FUNCTION(bit_bnot, "x") { BRET(~barg(L, 1)) }

#define BIT_OP(func, opr) \
  DEFINE_LUA_FUNCTION(func, "x1 [,x2...]") { int i; UBits b = barg(L, 1); \
    for (i = lua_gettop(L); i > 1; i--) b opr barg(L, i); BRET(b) }
BIT_OP(bit_band, &=)
BIT_OP(bit_bor, |=)
BIT_OP(bit_bxor, ^=)

#define bshl(b, n)  (b << n)
#define bshr(b, n)  (b >> n)
#define bsar(b, n)  ((SBits)b >> n)
#define brol(b, n)  ((b << n) | (b >> (32-n)))
#define bror(b, n)  ((b << (32-n)) | (b >> n))
#define BIT_SH(func, fn) \
  DEFINE_LUA_FUNCTION(func, "x, n") { \
    UBits b = barg(L, 1); UBits n = barg(L, 2) & 31; BRET(fn(b, n)) }
BIT_SH(bit_lshift, bshl)
BIT_SH(bit_rshift, bshr)
BIT_SH(bit_arshift, bsar)
BIT_SH(bit_rol, brol)
BIT_SH(bit_ror, bror)

DEFINE_LUA_FUNCTION(bit_bswap, "x")
{
  UBits b = barg(L, 1);
  b = (b >> 24) | ((b >> 8) & 0xff00) | ((b & 0xff00) << 8) | (b << 24);
  BRET(b)
}

DEFINE_LUA_FUNCTION(bit_tohex, "x [,n]")
{
  UBits b = barg(L, 1);
  SBits n = lua_isnone(L, 2) ? 8 : (SBits)barg(L, 2);
  const char *hexdigits = "0123456789abcdef";
  char buf[8];
  int i;
  if (n < 0) { n = -n; hexdigits = "0123456789ABCDEF"; }
  if (n > 8) n = 8;
  for (i = (int)n; --i >= 0; ) { buf[i] = hexdigits[b & 15]; b >>= 4; }
  lua_pushlstring(L, buf, (size_t)n);
  return 1;
}

static const struct luaL_Reg bit_funcs[] = {
  { "tobit",	bit_tobit },
  { "bnot",	bit_bnot },
  { "band",	bit_band },
  { "bor",	bit_bor },
  { "bxor",	bit_bxor },
  { "lshift",	bit_lshift },
  { "rshift",	bit_rshift },
  { "arshift",	bit_arshift },
  { "rol",	bit_rol },
  { "ror",	bit_ror },
  { "bswap",	bit_bswap },
  { "tohex",	bit_tohex },
  { NULL, NULL }
};

/* Signed right-shifts are implementation-defined per C89/C99.
** But the de facto standard are arithmetic right-shifts on two's
** complement CPUs. This behaviour is required here, so test for it.
*/
#define BAD_SAR		(bsar(-8, 2) != (SBits)-2)

bool luabitop_validate(lua_State *L) // originally named as luaopen_bit
{
  UBits b;
  lua_pushnumber(L, (lua_Number)1437217655L);
  b = barg(L, -1);
  if (b != (UBits)1437217655L || BAD_SAR) {  /* Perform a simple self-test. */
    const char *msg = "compiled with incompatible luaconf.h";
#ifdef LUA_NUMBER_DOUBLE
#ifdef __WIN32__
    if (b == (UBits)1610612736L)
      msg = "use D3DCREATE_FPU_PRESERVE with DirectX";
#endif
    if (b == (UBits)1127743488L)
      msg = "not compiled with SWAPPED_DOUBLE";
#endif
    if (BAD_SAR)
      msg = "arithmetic right-shift broken";
    luaL_error(L, "bit library self-test failed (%s)", msg);
    return false;
  }
#if LUA_VERSION_NUM < 502
  luaL_register(L, "bit", bit_funcs);
#else
  luaL_newlib(L, bit_funcs);
#endif
  return true;
}

// LuaBitOp ends here

DEFINE_LUA_FUNCTION(bitshift, "num,shift")
{
	int shift = luaL_checkinteger(L,2);
	if (shift < 0) {
		lua_pushinteger(L, -shift);
		lua_replace(L, 2);
		return bit_lshift(L);
	}
	else
		return bit_rshift(L);
}

DEFINE_LUA_FUNCTION(bitbit, "whichbit")
{
	int rv = 0;
	int numArgs = lua_gettop(L);
	for(int i = 1; i <= numArgs; i++) {
		int where = luaL_checkinteger(L,i);
		if (where >= 0 && where < 32)
			rv |= (1 << where);
	}
	lua_settop(L,0);
	BRET(rv);
}

//int emu_wait(lua_State* L);

void indicateBusy(lua_State* L, bool busy)
{
	// disabled because there have been complaints about this message being useless spam.
	// the script window's title changing should be sufficient, I guess.
/*	if(busy)
	{
		const char* fmt = "script became busy (frozen?)";
		va_list argp;
		va_start(argp, fmt);
		luaL_where(L, 0);
		lua_pushvfstring(L, fmt, argp);
		va_end(argp);
		lua_concat(L, 2);
		LuaContextInfo& info = GetCurrentInfo();
		int uid = luaStateToUIDMap[L];
		if(info.print)
		{
			info.print(uid, lua_tostring(L,-1));
			info.print(uid, "\r\n");
		}
		else
		{
			fprintf(stderr, "%s\n", lua_tostring(L,-1));
		}
		lua_pop(L, 1);
	}
*/
#ifdef __WIN32__
	int uid = luaStateToUIDMap[L];
	HWND hDlg = (HWND)uid;
	TCHAR str [1024];
	GetWindowText(hDlg, str, 1000);
	TCHAR* extra = _tcschr(str, '<');
	if(busy)
	{
		if(!extra)
			extra = str + lstrlen(str), *extra++ = ' ';
		_tcscpy(extra, TEXT("<BUSY>"));
	}
	else
	{
		if(extra)
			extra[-1] = 0;
	}
	SetWindowText(hDlg, str);
#endif
}

#define HOOKCOUNT 4096
#define MAX_WORRY_COUNT 6000
void LuaRescueHook(lua_State* L, lua_Debug *dbg)
{
	LuaContextInfo& info = GetCurrentInfo();

	info.worryCount++;

	if(info.stopWorrying && !info.panic)
	{
		if(info.worryCount > (MAX_WORRY_COUNT >> 2))
		{
			// the user already said they're OK with the script being frozen,
			// but we don't trust their judgement completely,
			// so periodically update the main loop so they have a chance to manually stop it
			info.worryCount = 0;
//			emu_wait(L);
			info.stopWorrying = true;
		}
		return;
	}

	if(info.worryCount > MAX_WORRY_COUNT || info.panic)
	{
		info.worryCount = 0;
		info.stopWorrying = false;

		bool stoprunning = true;
		bool stopworrying = true;
		if(!info.panic)
		{
#if defined(ASK_USER_ON_FREEZE) && defined(__WIN32__)
			int answer = MessageBox(HWnd, "A Lua script has been running for quite a while. Maybe it is in an infinite loop.\n\nWould you like to stop the script?\n\n(Yes to stop it now,\n No to keep running and not ask again,\n Cancel to keep running but ask again later)", "Lua Alert", MB_YESNOCANCEL | MB_DEFBUTTON3 | MB_ICONASTERISK);
			if(answer == IDNO)
				stoprunning = false;
			if(answer == IDCANCEL)
				stopworrying = false;
#else
			stoprunning = false;
#endif
		}

		if(!stoprunning && stopworrying)
		{
			info.stopWorrying = true; // don't remove the hook because we need it still running for RequestAbortLuaScript to work
			indicateBusy(info.L, true);
		}

		if(stoprunning)
		{
			//lua_sethook(L, NULL, 0, 0);
			assert(L->errfunc || L->errorJmp);
			luaL_error(L, info.panic ? info.panicMessage : "terminated by user");
		}

		info.panic = false;
	}
}

void printfToOutput(const char* fmt, ...)
{
	va_list list;
	va_start(list, fmt);
	int len = vscprintf(fmt, list);
	char* str = new char[len+1];
	vsprintf(str, fmt, list);
	va_end(list);
	LuaContextInfo& info = GetCurrentInfo();
	if(info.print)
	{
		lua_State* L = info.L;
		int uid = luaStateToUIDMap[L];
		info.print(uid, str);
		info.print(uid, "\r\n");
		worry(L,300);
	}
	else
	{
		fprintf(stdout, "%s\n", str);
	}
	delete[] str;
}

bool FailVerifyAtFrameBoundary(lua_State* L, const char* funcName, int unstartedSeverity=2, int inframeSeverity=2)
{
	if (Settings.StopEmulation)
	{
		static const char* msg = "cannot call %s() when emulation has not started.";
		switch(unstartedSeverity)
		{
		case 0: break;
		case 1: printfToOutput(msg, funcName); break;
		default: case 2: luaL_error(L, msg, funcName); break;
		}
		return true;
	}
	if(IPPU.InMainLoop)
	{
		static const char* msg = "cannot call %s() inside an emulation frame.";
		switch(inframeSeverity)
		{
		case 0: break;
		case 1: printfToOutput(msg, funcName); break;
		default: case 2: luaL_error(L, msg, funcName); break;
		}
		return true;
	}
	return false;
}
/*
// acts similar to normal emulation update
// except without the user being able to activate emulator commands
DEFINE_LUA_FUNCTION(emu_emulateframe, "")
{
	if(FailVerifyAtFrameBoundary(L, "emu.emulateframe", 0,1))
		return 0;

	Update_Emulation_One(HWnd);
	Prevent_Next_Frame_Skipping(); // so we don't skip a whole bunch of frames immediately after emulating many frames by this method

	worry(L,300);
	return 0;
}

// acts as a fast-forward emulation update that still renders every frame
// and the user is unable to activate emulator commands during it
DEFINE_LUA_FUNCTION(emu_emulateframefastnoskipping, "")
{
	if(FailVerifyAtFrameBoundary(L, "emu.emulateframefastnoskipping", 0,1))
		return 0;

	Update_Emulation_One_Before(HWnd);
	Update_Frame_Hook();
	Update_Emulation_After_Controlled(HWnd, true);
	Prevent_Next_Frame_Skipping(); // so we don't skip a whole bunch of frames immediately after a bout of fast-forward frames

	worry(L,200);
	return 0;
}

// acts as a (very) fast-forward emulation update
// where the user is unable to activate emulator commands
DEFINE_LUA_FUNCTION(emu_emulateframefast, "")
{
	if(FailVerifyAtFrameBoundary(L, "emu.emulateframefast", 0,1))
		return 0;

	disableVideoLatencyCompensationCount = VideoLatencyCompensation + 1;

	Update_Emulation_One_Before(HWnd);

	if(FrameCount%16 == 0) // skip rendering 15 out of 16 frames
	{
		// update once and render
		Update_Frame_Hook();
		Update_Emulation_After_Controlled(HWnd, true);
	}
	else
	{
		// update once but skip rendering
		Update_Frame_Fast_Hook();
		Update_Emulation_After_Controlled(HWnd, false);
	}

	Prevent_Next_Frame_Skipping(); // so we don't skip a whole bunch of frames immediately AFTER a bout of fast-forward frames

	worry(L,150);
	return 0;
}

// acts as an extremely-fast-forward emulation update
// that also doesn't render any graphics or generate any sounds,
// and the user is unable to activate emulator commands during it.
// if you load a savestate after calling this function,
// it should leave no trace of having been called,
// so you can do things like generate future emulation states every frame
// while the user continues to see and hear normal emulation
DEFINE_LUA_FUNCTION(emu_emulateframeinvisible, "")
{
	if(FailVerifyAtFrameBoundary(L, "emu.emulateframeinvisible", 0,1))
		return 0;

	int oldDisableSound2 = disableSound2;
	int oldDisableRamSearchUpdate = disableRamSearchUpdate;
	disableSound2 = true;
	disableRamSearchUpdate = true;

	Update_Emulation_One_Before_Minimal();
	Update_Frame_Fast();
	UpdateLagCount();

	disableSound2 = oldDisableSound2;
	disableRamSearchUpdate = oldDisableRamSearchUpdate;

	// disable video latency compensation for a few frames
	// because it can get pretty slow if that's doing prediction updates every frame
	// when the lua script is also doing prediction updates
	disableVideoLatencyCompensationCount = VideoLatencyCompensation + 1;

	worry(L,100);
	return 0;
}

DEFINE_LUA_FUNCTION(emu_speedmode, "mode")
{
	SpeedMode newSpeedMode = SPEEDMODE_NORMAL;
	if(lua_isnumber(L,1))
		newSpeedMode = (SpeedMode)luaL_checkinteger(L,1);
	else
	{
		const char* str = luaL_checkstring(L,1);
		if(!stricmp(str, "normal"))
			newSpeedMode = SPEEDMODE_NORMAL;
		else if(!stricmp(str, "nothrottle"))
			newSpeedMode = SPEEDMODE_NOTHROTTLE;
		else if(!stricmp(str, "turbo"))
			newSpeedMode = SPEEDMODE_TURBO;
		else if(!stricmp(str, "maximum"))
			newSpeedMode = SPEEDMODE_MAXIMUM;
	}

	LuaContextInfo& info = GetCurrentInfo();
	info.speedMode = newSpeedMode;
	RefreshScriptSpeedStatus();
	return 0;
}

// tells the emulator to wait while the script is doing calculations
// can call this periodically instead of emu.frameadvance
// note that the user can use hotkeys at this time
// (e.g. a savestate could possibly get loaded before emu.wait() returns)
DEFINE_LUA_FUNCTION(emu_wait, "")
{
	LuaContextInfo& info = GetCurrentInfo();

	switch(info.speedMode)
	{
		default:
		case SPEEDMODE_NORMAL:
			Step_Gens_MainLoop(true, false);
			break;
		case SPEEDMODE_NOTHROTTLE:
		case SPEEDMODE_TURBO:
		case SPEEDMODE_MAXIMUM:
			Step_Gens_MainLoop(Paused!=0, false);
			break;
	}

	return 0;
}
*/



DEFINE_LUA_FUNCTION(emu_frameadvance, "")
{
/*
	if(FailVerifyAtFrameBoundary(L, "emu.frameadvance", 0,1))
		return emu_wait(L);

	int uid = luaStateToUIDMap[L];
	LuaContextInfo& info = GetCurrentInfo();

	if(!info.ranFrameAdvance)
	{
		// otherwise we'll never see the first frame of GUI drawing
		if(info.speedMode != SPEEDMODE_MAXIMUM)
			Show_Genesis_Screen();
		info.ranFrameAdvance = true;
	}

	switch(info.speedMode)
	{
		default:
		case SPEEDMODE_NORMAL:
			while(!Step_Gens_MainLoop(true, true) && !info.panic);
			break;
		case SPEEDMODE_NOTHROTTLE:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			if(!(FastForwardKeyDown && (GetActiveWindow()==HWnd || BackgroundInput)))
				emu_emulateframefastnoskipping(L);
			else
				emu_emulateframefast(L);
			break;
		case SPEEDMODE_TURBO:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			emu_emulateframefast(L);
			break;
		case SPEEDMODE_MAXIMUM:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			emu_emulateframeinvisible(L);
			break;
	}
	*/
	S9xMainLoop();
	S9xProcessEvents(FALSE);

	return 0;
}

DEFINE_LUA_FUNCTION(emu_pause, "")
{
	LuaContextInfo& info = GetCurrentInfo();

	Settings.Paused = TRUE;
	//while(!Step_Gens_MainLoop(true, false) && !info.panic);

	// allow the user to not have to manually unpause
	// after restarting a script that used emu.pause()
	if(info.panic)
		Settings.Paused = FALSE;

	return 0;
}

DEFINE_LUA_FUNCTION(emu_unpause, "")
{
	LuaContextInfo& info = GetCurrentInfo();

	Settings.Paused = FALSE;
	return 0;
}

/*DEFINE_LUA_FUNCTION(emu_redraw, "")
{
	Show_Genesis_Screen();
	worry(L,250);
	return 0;
}
*/


extern uint8 S9xGetByteFree (uint32);
extern void S9xSetByteFree (uint8, uint32);

DEFINE_LUA_FUNCTION(memory_readbyte, "address")
{
	int address = lua_tointeger(L,1);

	uint8 value = S9xGetByteFree(address);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1; // we return the number of return values
}
DEFINE_LUA_FUNCTION(memory_readbytesigned, "address")
{
	int address = lua_tointeger(L,1);
	int8 value = S9xGetByteFree(address);
	lua_settop(L,0);
	lua_pushinteger(L, *(int8*)&value);
	return 1;
}
DEFINE_LUA_FUNCTION(memory_readword, "address")
{
	int address = lua_tointeger(L,1);
	uint16 value = S9xGetByteFree(address);
	value |= S9xGetByteFree(address + 1) << 8;
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
DEFINE_LUA_FUNCTION(memory_readwordsigned, "address")
{
	int address = lua_tointeger(L,1);
	int16 value = S9xGetByteFree(address);
	value |= S9xGetByteFree(address + 1) << 8;
	lua_settop(L,0);
	lua_pushinteger(L, *(int16*)&value);
	return 1;
}
DEFINE_LUA_FUNCTION(memory_readdword, "address")
{
	int address = luaL_checkinteger(L,1);
	uint32 value = S9xGetByteFree(address);
	value |= S9xGetByteFree(address + 1) << 8;
	value |= S9xGetByteFree(address + 2) << 16;
	value |= S9xGetByteFree(address + 3) << 24;
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
DEFINE_LUA_FUNCTION(memory_readdwordsigned, "address")
{
	int address = luaL_checkinteger(L,1);
	int32 value = S9xGetByteFree(address);
	value |= S9xGetByteFree(address + 1) << 8;
	value |= S9xGetByteFree(address + 2) << 16;
	value |= S9xGetByteFree(address + 3) << 24;
	lua_settop(L,0);
	lua_pushinteger(L, *(int32*)&value);
	return 1;
}

DEFINE_LUA_FUNCTION(memory_writebyte, "address,value")
{
	int address = lua_tointeger(L,1);
	uint8 value = (uint8)(lua_tointeger(L,2) & 0xFF);
	S9xSetByteFree(value, address);
	return 0;
}
DEFINE_LUA_FUNCTION(memory_writeword, "address,value")
{
	int address = lua_tointeger(L,1);
	uint16 value = (uint16)(lua_tointeger(L,2) & 0xFFFF);
	S9xSetByteFree(value & 0xFF, address);
	S9xSetByteFree((value >> 8) & 0xFF, address + 1);
	return 0;
}
DEFINE_LUA_FUNCTION(memory_writedword, "address,value")
{
	int address = luaL_checkinteger(L,1);
	uint32 value = (uint32)(luaL_checkinteger(L,2));
	S9xSetByteFree(value & 0xFF, address);
	S9xSetByteFree((value >> 8) & 0xFF, address + 1);
	S9xSetByteFree((value >> 16) & 0xFF, address + 2);
	S9xSetByteFree((value >> 24) & 0xFF, address + 3);
	return 0;
}

DEFINE_LUA_FUNCTION(memory_readbyterange, "address,length")
{
	int address = luaL_checkinteger(L,1);
	int length = luaL_checkinteger(L,2);

	if(length < 0)
	{
		address += length;
		length = -length;
	}

	// push the array
	lua_createtable(L, abs(length), 0);

	// put all the values into the (1-based) array
	for(int a = address, n = 1; n <= length; a++, n++)
	{
//		if(IsHardwareAddressValid(a))
//		{
			uint8 value = S9xGetByteFree(a);
			lua_pushinteger(L, value);
			lua_rawseti(L, -2, n);
//		}
		// else leave the value nil
	}

	return 1;
}

/*DEFINE_LUA_FUNCTION(memory_isvalid, "address")
{
	int address = luaL_checkinteger(L,1);
	lua_settop(L,0);
	lua_pushboolean(L, IsHardwareAddressValid(address));
	return 1;
}
*/
struct registerPointerMap
{
	const char* registerName;
	unsigned int* pointer;
	int dataSize;
};

#define RPM_ENTRY(name,var) {name, (unsigned int*)&var, sizeof(var)},

registerPointerMap a65c816PointerMap [] = {
	RPM_ENTRY("db", Registers.DB)
	RPM_ENTRY("p", Registers.PL)
	RPM_ENTRY("e", Registers.PH) // 1bit flag
	RPM_ENTRY("a", Registers.A.W)
	RPM_ENTRY("d", Registers.D.W)
	RPM_ENTRY("s", Registers.S.W)
	RPM_ENTRY("x", Registers.X.W)
	RPM_ENTRY("y", Registers.Y.W)
	RPM_ENTRY("pb", Registers.PB)
	RPM_ENTRY("pc", Registers.PCw)
	RPM_ENTRY("pbpc", Registers.PBPC)
	{}
};
registerPointerMap sa1PointerMap [] = {
	RPM_ENTRY("db", SA1Registers.DB)
	RPM_ENTRY("p", SA1Registers.PL)
	RPM_ENTRY("e", SA1Registers.PH) // 1bit flag
	RPM_ENTRY("a", SA1Registers.A.W)
	RPM_ENTRY("d", SA1Registers.D.W)
	RPM_ENTRY("s", SA1Registers.S.W)
	RPM_ENTRY("x", SA1Registers.X.W)
	RPM_ENTRY("y", SA1Registers.Y.W)
	RPM_ENTRY("pb", SA1Registers.PB)
	RPM_ENTRY("pc", SA1Registers.PCw)
	RPM_ENTRY("pbpc", SA1Registers.PBPC)
	{}
};

struct cpuToRegisterMap
{
	const char* cpuName;
	registerPointerMap* rpmap;
}
cpuToRegisterMaps [] =
{
	{"65c816.", a65c816PointerMap},
	{"main.", a65c816PointerMap},
	{"sa1.", sa1PointerMap},
	{"", a65c816PointerMap},
};


DEFINE_LUA_FUNCTION(memory_getregister, "cpu_dot_registername_string")
{
	const char* qualifiedRegisterName = luaL_checkstring(L,1);
	lua_settop(L,0);
	for(int cpu = 0; cpu < sizeof(cpuToRegisterMaps)/sizeof(*cpuToRegisterMaps); cpu++)
	{
		cpuToRegisterMap ctrm = cpuToRegisterMaps[cpu];
		int cpuNameLen = strlen(ctrm.cpuName);
		if(!strnicmp(qualifiedRegisterName, ctrm.cpuName, cpuNameLen))
		{
			qualifiedRegisterName += cpuNameLen;
			for(int reg = 0; ctrm.rpmap[reg].dataSize; reg++)
			{
				registerPointerMap rpm = ctrm.rpmap[reg];
				if(!stricmp(qualifiedRegisterName, rpm.registerName))
				{
					switch(rpm.dataSize)
					{ default:
					case 1: lua_pushinteger(L, *(uint8*)rpm.pointer); break;
					case 2: lua_pushinteger(L, *(uint16*)rpm.pointer); break;
					case 4: lua_pushinteger(L, *(uint32*)rpm.pointer); break;
					}
					return 1;
				}
			}
			lua_pushnil(L);
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}
DEFINE_LUA_FUNCTION(memory_setregister, "cpu_dot_registername_string,value")
{
	const char* qualifiedRegisterName = luaL_checkstring(L,1);
	unsigned long value = (unsigned long)(luaL_checkinteger(L,2));
	lua_settop(L,0);
	for(int cpu = 0; cpu < sizeof(cpuToRegisterMaps)/sizeof(*cpuToRegisterMaps); cpu++)
	{
		cpuToRegisterMap ctrm = cpuToRegisterMaps[cpu];
		int cpuNameLen = strlen(ctrm.cpuName);
		if(!strnicmp(qualifiedRegisterName, ctrm.cpuName, cpuNameLen))
		{
			qualifiedRegisterName += cpuNameLen;
			for(int reg = 0; ctrm.rpmap[reg].dataSize; reg++)
			{
				registerPointerMap rpm = ctrm.rpmap[reg];
				if(!stricmp(qualifiedRegisterName, rpm.registerName))
				{
					switch(rpm.dataSize)
					{ default:
					case 1: *(uint8*)rpm.pointer = (uint8)(value & 0xFF); break;
					case 2: *(uint16*)rpm.pointer = (uint16)(value & 0xFFFF); break;
					case 4: *(uint32*)rpm.pointer = value; break;
					}
					return 0;
				}
			}
			return 0;
		}
	}
	return 0;
}


struct StateData
{
	uint8 *buffer;
	size_t size;
};

DEFINE_LUA_FUNCTION(state_create, "[location]")
{
	if(lua_isnumber(L,1))
	{
		// simply return the integer that got passed in
		// (that's as good a savestate object as any for a numbered savestate slot)
		lua_settop(L,1);
		return 1;
	}

	// allocate a pointer to an in-memory/anonymous savestate
	StateData** ppStateData = (StateData**)lua_newuserdata(L, sizeof(StateData*));
	*ppStateData = new StateData();
	if (*ppStateData == NULL)
	{
		luaL_error(L, "memory allocation error.");
	}

	StateData& stateData = **ppStateData;
	stateData.size = 0;
	stateData.buffer = NULL;

	luaL_getmetatable(L, "StateData*");
	lua_setmetatable(L, -2);

	return 1;
}

// savestate.save(location [, option])
// saves the current emulation state to the given location
// you can pass in either a savestate file number (an integer),
// OR you can pass in a savestate object that was returned by savestate.create()
// if option is "quiet" then any warning messages will be suppressed
// if option is "scriptdataonly" then the state will not actually be saved, but any save callbacks will still get called and their results will be saved (see savestate.registerload()/savestate.registersave())
DEFINE_LUA_FUNCTION(state_save, "location[,option]")
{
	const char* option = (lua_type(L,2) == LUA_TSTRING) ? lua_tostring(L,2) : NULL;
	if(option)
	{
		if(!stricmp(option, "quiet")) // I'm not sure if saving can generate warning messages, but we might as well support suppressing them should they turn out to exist
			g_disableStatestateWarnings = true;
		else if(!stricmp(option, "scriptdataonly"))
			g_onlyCallSavestateCallbacks = true;
	}
	struct Scope { ~Scope(){ g_disableStatestateWarnings = false; g_onlyCallSavestateCallbacks = false; } } scope; // needs to run even if the following code throws an exception... maybe I should have put this in a "finally" block instead, but this project seems to have something against using the "try" statement

	if(!g_onlyCallSavestateCallbacks && FailVerifyAtFrameBoundary(L, "savestate.save", 2,2))
		return 0;

	int type = lua_type(L,1);
	switch(type)
	{
		case LUA_TNUMBER: // numbered save file
		default:
		{
			int stateNumber = luaL_checkinteger(L,1);
			char Name [1024] = {0};
			Get_State_File_Name(Name, stateNumber);
			S9xFreezeGame(Name);
		}	return 0;
		case LUA_TUSERDATA: // in-memory save slot
		{
			StateData& stateData = **((StateData**)luaL_checkudata(L, 1, "StateData*"));
			uint32 stateSizeNeeded = S9xFreezeSize();

			// memory allocation
			uint8 *newBuffer = NULL;
			if (stateData.buffer == NULL)
			{
				newBuffer = (uint8 *)malloc(stateSizeNeeded);
				if (newBuffer == NULL)
				{
					luaL_error(L, "memory allocation error.");
				}
			}
			else if (stateData.size < stateSizeNeeded)
			{
				newBuffer = (uint8 *)realloc(stateData.buffer, stateSizeNeeded);
				if (newBuffer == NULL)
				{
					luaL_error(L, "memory allocation error.");
				}
			}
			if (newBuffer != NULL)
			{
				stateData.buffer = newBuffer;
				stateData.size = stateSizeNeeded;
			}

			S9xFreezeGameMem(stateData.buffer, stateData.size);
		}	return 0;
	}
}

// savestate.load(location [, option])
// loads the current emulation state from the given location
// you can pass in either a savestate file number (an integer),
// OR you can pass in a savestate object that was returned by savestate.create() and has already saved to with savestate.save()
// if option is "quiet" then any warning messages will be suppressed
// if option is "scriptdataonly" then the state will not actually be loaded, but load callbacks will still get called and supplied with the data saved by save callbacks (see savestate.registerload()/savestate.registersave())
DEFINE_LUA_FUNCTION(state_load, "location[,option]")
{
	const char* option = (lua_type(L,2) == LUA_TSTRING) ? lua_tostring(L,2) : NULL;
	if(option)
	{
		if(!stricmp(option, "quiet"))
			g_disableStatestateWarnings = true;
		else if(!stricmp(option, "scriptdataonly"))
			g_onlyCallSavestateCallbacks = true;
	}
	struct Scope { ~Scope(){ g_disableStatestateWarnings = false; g_onlyCallSavestateCallbacks = false; } } scope; // needs to run even if the following code throws an exception... maybe I should have put this in a "finally" block instead, but this project seems to have something against using the "try" statement

	if(!g_onlyCallSavestateCallbacks && FailVerifyAtFrameBoundary(L, "savestate.load", 2,2))
		return 0;

	g_disableStatestateWarnings = lua_toboolean(L,2) != 0;

	int type = lua_type(L,1);
	switch(type)
	{
		case LUA_TNUMBER: // numbered save file
		default:
		{
			LuaContextInfo& info = GetCurrentInfo();
			int stateNumber = luaL_checkinteger(L,1);

			char Name [1024] = {0};
			Get_State_File_Name(Name, stateNumber);
			bool8 prevRerecordCountSkip = S9xMovieGetRerecordCountSkip();
			S9xMovieSetRerecordCountSkip(info.rerecordCountingDisabled);
			S9xUnfreezeGame(Name);
			S9xMovieSetRerecordCountSkip(prevRerecordCountSkip);
		}	return 0;
		case LUA_TUSERDATA: // in-memory save slot
		{
			LuaContextInfo& info = GetCurrentInfo();
			StateData& stateData = **((StateData**)luaL_checkudata(L, 1, "StateData*"));
			if(stateData.buffer != NULL && stateData.buffer[0]){
				bool8 prevRerecordCountSkip = S9xMovieGetRerecordCountSkip();
				S9xMovieSetRerecordCountSkip(info.rerecordCountingDisabled);
				S9xUnfreezeGameMem(stateData.buffer, stateData.size);
				S9xMovieSetRerecordCountSkip(prevRerecordCountSkip);
			}
			else // the first byte of a valid savestate is never 0 (snes9x: it should start with "#!s9xsnp")
				luaL_error(L, "attempted to load an anonymous savestate before saving it");
		}	return 0;
	}
}

// savestate.loadscriptdata(location)
// returns the user data associated with the given savestate
// without actually loading the rest of that savestate or calling any callbacks.
// you can pass in either a savestate file number (an integer),
// OR you can pass in a savestate object that was returned by savestate.create()
// but note that currently only non-anonymous savestates can have associated scriptdata
//
// also note that this returns the same values
// that would be passed into a registered load function.
// the main reason this exists also is so you can register a load function that
// chooses whether or not to load the scriptdata instead of always loading it,
// and also to provide a nicer interface for loading scriptdata
// without needing to trigger savestate loading first
DEFINE_LUA_FUNCTION(state_loadscriptdata, "location")
{
	int type = lua_type(L,1);
	switch(type)
	{
		case LUA_TNUMBER: // numbered save file
		default:
		{
			int stateNumber = luaL_checkinteger(L,1);
			char Name [1024] = {0};
			Get_State_File_Name(Name, stateNumber);
			{
				LuaSaveData saveData;

				char luaSaveFilename [512];
				strncpy(luaSaveFilename, Name, 512);
				luaSaveFilename[512-(1+strlen(".luasav"))] = '\0';
				strcat(luaSaveFilename, ".luasav");
				FILE* luaSaveFile = fopen(luaSaveFilename, "rb");
				if(luaSaveFile)
				{
					saveData.ImportRecords(luaSaveFile);
					fclose(luaSaveFile);

					int uid = luaStateToUIDMap[L];
					LuaContextInfo& info = GetCurrentInfo();

					lua_settop(L, 0);
					saveData.LoadRecord(uid, info.dataLoadKey, (unsigned int)-1);
					return lua_gettop(L);
				}
			}
		}	return 0;
		case LUA_TUSERDATA: // in-memory save slot
		{	// there can be no user data associated with those, at least not yet
		}	return 0;
	}
}

// savestate.savescriptdata(location)
// same as savestate.save(location, "scriptdataonly")
// only provided for consistency with savestate.loadscriptdata(location)
DEFINE_LUA_FUNCTION(state_savescriptdata, "location")
{
	lua_settop(L, 1);
	lua_pushstring(L, "scriptdataonly");
	return state_save(L);
}


static const struct ButtonDesc
{
	unsigned short controllerNum;
	unsigned short bit;
	const char* name;
}
s_buttonDescs [] =
{
	{1, 4, "R"},
	{1, 5, "L"},
	{1, 6, "X"},
	{1, 7, "A"},
	{1, 8, "right"},
	{1, 9, "left"},
	{1, 10, "down"},
	{1, 11, "up"},
	{1, 12, "start"},
	{1, 13, "select"},
	{1, 14, "Y"},
	{1, 15, "B"},
	{2, 4, "R"},
	{2, 5, "L"},
	{2, 6, "X"},
	{2, 7, "A"},
	{2, 8, "right"},
	{2, 9, "left"},
	{2, 10, "down"},
	{2, 11, "up"},
	{2, 12, "start"},
	{2, 13, "select"},
	{2, 14, "Y"},
	{2, 15, "B"},
	{3, 4, "R"},
	{3, 5, "L"},
	{3, 6, "X"},
	{3, 7, "A"},
	{3, 8, "right"},
	{3, 9, "left"},
	{3, 10, "down"},
	{3, 11, "up"},
	{3, 12, "start"},
	{3, 13, "select"},
	{3, 14, "Y"},
	{3, 15, "B"},
	{4, 4, "R"},
	{4, 5, "L"},
	{4, 6, "X"},
	{4, 7, "A"},
	{4, 8, "right"},
	{4, 9, "left"},
	{4, 10, "down"},
	{4, 11, "up"},
	{4, 12, "start"},
	{4, 13, "select"},
	{4, 14, "Y"},
	{4, 15, "B"},
	{5, 4, "R"},
	{5, 5, "L"},
	{5, 6, "X"},
	{5, 7, "A"},
	{5, 8, "right"},
	{5, 9, "left"},
	{5, 10, "down"},
	{5, 11, "up"},
	{5, 12, "start"},
	{5, 13, "select"},
	{5, 14, "Y"},
	{5, 15, "B"},
	{6, 4, "R"},
	{6, 5, "L"},
	{6, 6, "X"},
	{6, 7, "A"},
	{6, 8, "right"},
	{6, 9, "left"},
	{6, 10, "down"},
	{6, 11, "up"},
	{6, 12, "start"},
	{6, 13, "select"},
	{6, 14, "Y"},
	{6, 15, "B"},
	{7, 4, "R"},
	{7, 5, "L"},
	{7, 6, "X"},
	{7, 7, "A"},
	{7, 8, "right"},
	{7, 9, "left"},
	{7, 10, "down"},
	{7, 11, "up"},
	{7, 12, "start"},
	{7, 13, "select"},
	{7, 14, "Y"},
	{7, 15, "B"},
	{8, 4, "R"},
	{8, 5, "L"},
	{8, 6, "X"},
	{8, 7, "A"},
	{8, 8, "right"},
	{8, 9, "left"},
	{8, 10, "down"},
	{8, 11, "up"},
	{8, 12, "start"},
	{8, 13, "select"},
	{8, 14, "Y"},
	{8, 15, "B"},
};

// string joypad.gettype(int port = 1)
//
//  Returns the type of controller at the given physical port index 1 or 2
//  (port is the same as "which" for other input functions, unless there's a multitap)
//  possible return values are "joypad", "mouse", "superscope", "justifier", "justifiers", "multitap", "none"
DEFINE_LUA_FUNCTION(joy_gettype, "[port=1]")
{
	int port = 0;
	if (lua_type(L, 1) == LUA_TNUMBER)
	{
		port = luaL_checkinteger(L, 1) - 1;
	}

	controllers controller;
	int8 ids[4];
	S9xGetController(port, &controller, &ids[0], &ids[1], &ids[2], &ids[3]);

	switch(controller)
	{
		default:
		case CTL_NONE: lua_pushliteral(L, "none"); break;
		case CTL_JOYPAD: lua_pushliteral(L, "joypad"); break;
		case CTL_MOUSE: lua_pushliteral(L, "mouse"); break;
		case CTL_SUPERSCOPE: lua_pushliteral(L, "superscope"); break;
		case CTL_JUSTIFIER: lua_pushstring(L, ids[0] ? "justifiers" : "justifier"); break;
		case CTL_MP5: lua_pushliteral(L, "multitap"); break;
	}
	return 1;
}

// joypad.settype(int port = 1, string typename)
//
//  Sets the type of controller at the given physical port index 1 or 2
//  (port is the same as "which" for other input functions, unless there's a multitap)
//  The SNES sees a new controller type get plugged in instantly.
//  note that it's an error to call this command while a movie is active.
//  note that a superscope must be plugged into port 2 for it to work, and other peripherals might have similar sorts of requirements
//  valid types are "joypad", "mouse", "superscope", "justifier", "justifiers", "multitap", "none"
DEFINE_LUA_FUNCTION(joy_settype, "[port=1,]typename")
{
	if (S9xMovieActive())
		luaL_error(L, "joypad.settype() cannot be called while a movie is active.");

	int port = 0;
	int index = 1;
	if (lua_type(L, index) == LUA_TNUMBER)
	{
		port = luaL_checkinteger(L, index) - 1;
		index++;
	}
	const char* type = luaL_checkstring(L, index);

	controllers controller;
	int8 ids[4];
	S9xGetController(port, &controller, &ids[0], &ids[1], &ids[2], &ids[3]);

	if(!strcmp(type, "joypad"))
	{
		controller = CTL_JOYPAD;
		ids[0] = port;
	}
	else if(!strcmp(type, "mouse"))
	{
		controller = CTL_MOUSE;
		ids[0] = port;
	}
	else if(!strcmp(type, "superscope"))
	{
		controller = CTL_SUPERSCOPE;
		ids[0] = 0;
	}
	else if(!strcmp(type, "justifier"))
	{
		controller = CTL_JUSTIFIER;
		ids[0] = 0;
	}
	else if(!strcmp(type, "justifiers"))
	{
		controller = CTL_JUSTIFIER;
		ids[0] = 1;
	}
	else if(!strcmp(type, "multitap"))
	{
		controller = CTL_MP5;
		if(port == 0)
		{
			ids[0] = 0;
			ids[1] = 1;
			ids[2] = 2;
			ids[3] = 3;
		}
	}
	else
		controller = CTL_NONE;

	Settings.MouseMaster = true;
	Settings.JustifierMaster = true;
	Settings.SuperScopeMaster = true;
	Settings.MultiPlayer5Master = true;

	S9xSetController(port, controller, ids[0],ids[1],ids[2],ids[3]);

	Settings.MultiPlayer5Master = false;
	Settings.SuperScopeMaster = false;
	Settings.JustifierMaster = false;
	Settings.MouseMaster = false;

	// now fix emulation settings and controller ids for multitap
	S9xGetController(0, &controller, &ids[0],&ids[1],&ids[2],&ids[3]);
	int max0id = std::max((signed char)0, std::max(ids[0], std::max(ids[1], std::max(ids[2], ids[3]))));
	if(controller == CTL_MOUSE) Settings.MouseMaster = true;
	if(controller == CTL_JUSTIFIER) Settings.JustifierMaster = true;
	if(controller == CTL_SUPERSCOPE) Settings.SuperScopeMaster = true;
	if(controller == CTL_MP5) Settings.MultiPlayer5Master = true;
	controllers controller2;
	S9xGetController(1, &controller2, &ids[0],&ids[1],&ids[2],&ids[3]);
	if(controller2 == CTL_MOUSE) Settings.MouseMaster = true;
	if(controller2 == CTL_JUSTIFIER) Settings.JustifierMaster = true;
	if(controller2 == CTL_SUPERSCOPE) Settings.SuperScopeMaster = true;
	if(controller2 == CTL_MP5) Settings.MultiPlayer5Master = true;
	if((controller2 == CTL_JOYPAD && controller == CTL_MP5) || controller2 == CTL_MP5)
	{
		ids[0] = max0id + 1;
		if(controller2 == CTL_MP5)
		{
			ids[1] = max0id + 2;
			ids[2] = max0id + 3;
			ids[3] = max0id + 4;
		}
		S9xSetController(port, controller2, ids[0],ids[1],ids[2],ids[3]);
	}

#ifdef __WIN32__
	// support only limited combination of peripherals
	extern void ChangeInputDevice(void);
	if(!strcmp(type, "mouse") && port == 0)
		GUI.ControllerOption = SNES_MOUSE;
	else if(!strcmp(type, "mouse") && port == 1)
		GUI.ControllerOption = SNES_MOUSE_SWAPPED;
	else if(!strcmp(type, "superscope"))
		GUI.ControllerOption = SNES_SUPERSCOPE;
	else if(!strcmp(type, "justifier"))
		GUI.ControllerOption = SNES_JUSTIFIER;
	else if(!strcmp(type, "justifiers"))
		GUI.ControllerOption = SNES_JUSTIFIER_2;
	else if(!strcmp(type, "multitap") && port == 0)
		GUI.ControllerOption = SNES_MULTIPLAYER8;
	else if(!strcmp(type, "multitap") && port == 1)
		GUI.ControllerOption = SNES_MULTIPLAYER5;
	else
		GUI.ControllerOption = SNES_JOYPAD;
	ChangeInputDevice();
#endif

	return 0;
}

int joy_getArgControllerNum(lua_State* L, int& index)
{
	int controllerNumber;
	int type = lua_type(L,index);
	if(type == LUA_TSTRING || type == LUA_TNUMBER)
	{
		controllerNumber = luaL_checkinteger(L,index);
		index++;
	}
	else
	{
		// argument omitted; default to controller 1
		controllerNumber = 1;
	}

	if(controllerNumber < 1 || controllerNumber > 8)
		luaL_error(L, "controller number must be within the range 1 to 8");

	return controllerNumber;
}


// PERIPHERAL_SUPPORT
#define SNESMOUSE_LEFT  0x40
#define SNESMOUSE_RIGHT 0x80
#define SUPERSCOPE_FIRE       0x80
#define SUPERSCOPE_CURSOR     0x40
#define SUPERSCOPE_TURBO      0x20
#define SUPERSCOPE_PAUSE      0x10
#define SUPERSCOPE_OFFSCREEN  0x02
#define JUSTIFIER_TRIGGER    0x80
#define JUSTIFIER_START      0x20
#define JUSTIFIER_SELECT     0x08
#define JUSTIFIER_OFFSCREEN  0x02
#define JUSTIFIER2_TRIGGER   0x40
#define JUSTIFIER2_START     0x10
#define JUSTIFIER2_SELECT    0x04
#define JUSTIFIER2_OFFSCREEN 0x01
#define MOUSE_DATA_SIZE	5
#define SCOPE_DATA_SIZE	6
#define JUSTIFIER_DATA_SIZE	11
bool MovieGetMouse(int i, uint8 out [MOUSE_DATA_SIZE]);
bool MovieGetScope(int i, uint8 out [SCOPE_DATA_SIZE]);
bool MovieGetJustifier(int i, uint8 out [JUSTIFIER_DATA_SIZE]);
void AddCommandTransformAxis(controllers type, int idx, int16 val, bool8 axis);
void AddCommandTransformButton(controllers type, int idx, bool8 on, uint8 mask);
void ClearCommandTransforms();

// joypad.set(controllerNum = 1, inputTable)
// controllerNum can be within the range 1 to 8
DEFINE_LUA_FUNCTION(joy_set, "[controller=1,]inputtable")
{
	int index = 1;
	int controllerNumber = joy_getArgControllerNum(L, index);

	// And the table of buttons.
	int tableIndex = lua_istable(L, 1) ? 1 : 2;
	luaL_checktype(L, tableIndex, LUA_TTABLE);

	if (S9xMoviePlaying()) // don't allow tampering with a playing movie's input
		return 0; // (although it might be useful sometimes...)

	if (IPPU.InMainLoop)
	{
		// defer this function until when we are processing input
		DeferFunctionCall(L, deferredJoySetIDString);
		return 0;
	}

	controllers con = CTL_JOYPAD;
	int8 ids[4];
	if(controllerNumber <= 2) // could be a peripheral in ports 1 or 2, let's check
		S9xGetController(controllerNumber - 1, &con, &ids[0], &ids[1], &ids[2], &ids[3]);

	switch(con)
	{
	default: // joypad
		{
			uint32 input = 0;
			uint32 mask = 0;

			for(int i = 0; i < sizeof(s_buttonDescs)/sizeof(*s_buttonDescs); i++)
			{
				const ButtonDesc& bd = s_buttonDescs[i];
				if(bd.controllerNum == controllerNumber)
				{
					lua_getfield(L, index, bd.name);
					if (!lua_isnil(L,-1))
					{
						bool pressed = lua_toboolean(L,-1) != 0;
						uint32 bitmask = ((uint32)1 << bd.bit);
						if(pressed)
							input |= bitmask;
						else
							input &= ~bitmask;
						mask |= bitmask;
					}
					lua_pop(L,1);
				}
			}
			MovieSetJoypad(controllerNumber - 1, input, mask);
		}
		break;
	case CTL_MOUSE:
		{
			// TODO NYI
		}
		break;
	case CTL_SUPERSCOPE:
		{
			// TODO NYI
		}
		break;
	case CTL_JUSTIFIER:
		{
			// TODO NYI
		}
		break;
	}

	return 0;
}

// joypad.get(controllerNum = 1)
// controllerNum can be within the range 1 to 8
int joy_get_internal(lua_State* L, bool reportUp, bool reportDown)
{
	int index = 1;
	int controllerNumber = joy_getArgControllerNum(L, index);

	lua_newtable(L);

	controllers controller = CTL_JOYPAD;
	int8 ids[4];
	if(controllerNumber <= 2) // could be a peripheral in ports 1 or 2, let's check
		S9xGetController(controllerNumber - 1, &controller, &ids[0], &ids[1], &ids[2], &ids[3]);

	bool pressed;
	switch(controller)
	{
	default: // joypad
		{
			uint32 input = MovieGetJoypad(controllerNumber - 1);

			for(int i = 0; i < sizeof(s_buttonDescs)/sizeof(*s_buttonDescs); i++)
			{
				const ButtonDesc& bd = s_buttonDescs[i];
				if(bd.controllerNum == controllerNumber)
				{
					pressed = (input & ((uint32)1<<bd.bit)) != 0;
					if((pressed && reportDown) || (!pressed && reportUp))
					{
						lua_pushboolean(L, pressed);
						lua_setfield(L, -2, bd.name);
					}
				}
			}
		}
		break;
	case CTL_MOUSE:
		{
			uint8 buf [MOUSE_DATA_SIZE] = {0};
			if(MovieGetMouse(controllerNumber - 1, buf))
			{
				int16 x = ((uint16*)buf)[0];
				int16 y = ((uint16*)buf)[1];
				uint8 buttons = buf[4];

				// set table with mouse status
				lua_pushinteger(L,x);     // note: the mouse does not really have x and y coordinates.
				lua_setfield(L, -2, "x"); //       so, these coordinates are "referenceless",
				lua_pushinteger(L,y);     //       they don't make sense except considering the difference
				lua_setfield(L, -2, "y"); //       between them and their previous value.
				pressed = buttons & SNESMOUSE_LEFT;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "left");
				}
				pressed = buttons & SNESMOUSE_RIGHT;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "right");
				}
			}
		}
		break;
	case CTL_SUPERSCOPE:
		{
			uint8 buf [SCOPE_DATA_SIZE] = {0};
			if(MovieGetScope(controllerNumber - 1, buf))
			{
				int16 x = ((uint16*)buf)[0];
				int16 y = ((uint16*)buf)[1];
				uint8 buttons = buf[4];

				// set table with super scope status
				lua_pushinteger(L, x);
				lua_setfield(L, -2, "x");
				lua_pushinteger(L, y);
				lua_setfield(L, -2, "y");
				pressed = buttons & SUPERSCOPE_FIRE;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "fire");
				}
				pressed = buttons & SUPERSCOPE_CURSOR;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "cursor");
				}
				pressed = buttons & SUPERSCOPE_TURBO;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "turbo");
				}
				pressed = buttons & SUPERSCOPE_PAUSE;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "pause");
				}
				pressed = buttons & SUPERSCOPE_OFFSCREEN;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "offscreen");
				}
			}
		}
		break;
	case CTL_JUSTIFIER:
		{
			uint8 buf [JUSTIFIER_DATA_SIZE] = {0};
			if(MovieGetJustifier(controllerNumber - 1, buf))
			{
				bool weHaveTwoJustifiers = (ids[0] == 1);
				int16 x1 = ((uint16*)buf)[0];
				int16 y1 = ((uint16*)buf)[2];
				uint8 buttons = buf[8];
				bool8 offscreen1 = buf[9];

				// set table with justifier status
				lua_pushinteger(L, x1);
				lua_setfield(L, -2, "x");
				lua_pushinteger(L, y1);
				lua_setfield(L, -2, "y");
				pressed = buttons & JUSTIFIER_TRIGGER;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "trigger");
				}
				pressed = buttons & JUSTIFIER_START;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "start");
				}
				pressed = buttons & JUSTIFIER_SELECT;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "select");
				}
				pressed = offscreen1;
				if((pressed && reportDown) || (!pressed && reportUp))
				{
					lua_pushboolean(L, pressed);
					lua_setfield(L, -2, "offscreen");
				}

				if(weHaveTwoJustifiers)
				{
					int16 x2 = ((uint16*)buf)[1];
					int16 y2 = ((uint16*)buf)[3];
					bool8 offscreen2 = buf[10];

					// also set table with the second justifier's status
					lua_pushinteger(L, x2);
					lua_setfield(L, -2, "x2");
					lua_pushinteger(L, y2);
					lua_setfield(L, -2, "y2");
					pressed = buttons & JUSTIFIER2_TRIGGER;
					if((pressed && reportDown) || (!pressed && reportUp))
					{
						lua_pushboolean(L, pressed);
						lua_setfield(L, -2, "trigger2");
					}
					pressed = buttons & JUSTIFIER2_START;
					if((pressed && reportDown) || (!pressed && reportUp))
					{
						lua_pushboolean(L, pressed);
						lua_setfield(L, -2, "start2");
					}
					pressed = buttons & JUSTIFIER2_SELECT;
					if((pressed && reportDown) || (!pressed && reportUp))
					{
						lua_pushboolean(L, pressed);
						lua_setfield(L, -2, "select2");
					}
					pressed = buttons & offscreen2;
					if((pressed && reportDown) || (!pressed && reportUp))
					{
						lua_pushboolean(L, pressed);
						lua_setfield(L, -2, "offscreen2");
					}
				}
			}
		}
		break;
	}

	return 1;
}
// joypad.get(int controllerNumber = 1)
// returns a table of every game button,
// true meaning currently-held and false meaning not-currently-held
// this WILL read input from a currently-playing movie
DEFINE_LUA_FUNCTION(joy_get, "[controller=1]")
{
	return joy_get_internal(L, true, true);
}
// joypad.getdown(int controllerNumber = 1)
// returns a table of every game button that is currently held
DEFINE_LUA_FUNCTION(joy_getdown, "[controller=1]")
{
	return joy_get_internal(L, false, true);
}
// joypad.getup(int controllerNumber = 1)
// returns a table of every game button that is not currently held
DEFINE_LUA_FUNCTION(joy_getup, "[controller=1]")
{
	return joy_get_internal(L, true, false);
}

// joypad.peek(controllerNum = 1)
// controllerNum can be within the range 1 to 8
/*int joy_peek_internal(lua_State* L, bool reportUp, bool reportDown)
{
	int index = 1;
	int controllerNumber = joy_getArgControllerNum(L, index);

	lua_newtable(L);

	long long input = PeekInputCondensed();

	for(int i = 0; i < sizeof(s_buttonDescs)/sizeof(*s_buttonDescs); i++)
	{
		const ButtonDesc& bd = s_buttonDescs[i];
		if(bd.controllerNum == controllerNumber)
		{
			bool pressed = (input & ((long long)1<<bd.bit)) == 0;
			if((pressed && reportDown) || (!pressed && reportUp))
			{
				lua_pushboolean(L, pressed);
				lua_setfield(L, -2, bd.name);
			}
		}
	}

	return 1;
}

// joypad.peek(int controllerNumber = 1)
// returns a table of every game button,
// true meaning currently-held and false meaning not-currently-held
// peek checks which joypad buttons are physically pressed, so it will NOT read input from a playing movie, it CAN read mid-frame input, and it will NOT pay attention to stuff like autofire or autohold or disabled L+R/U+D
DEFINE_LUA_FUNCTION(joy_peek, "[controller=1]")
{
	return joy_peek_internal(L, true, true);
}
// joypad.peekdown(int controllerNumber = 1)
// returns a table of every game button that is currently held (according to what joypad.peek() would return)
DEFINE_LUA_FUNCTION(joy_peekdown, "[controller=1]")
{
	return joy_peek_internal(L, false, true);
}
// joypad.peekup(int controllerNumber = 1)
// returns a table of every game button that is not currently held (according to what joypad.peek() would return)
DEFINE_LUA_FUNCTION(joy_peekup, "[controller=1]")
{
	return joy_peek_internal(L, true, false);
}*/


static const struct ColorMapping
{
	const char* name;
	int value;
}
s_colorMapping [] =
{
	{"white",     0xFFFFFFFF},
	{"black",     0x000000FF},
	{"clear",     0x00000000},
	{"gray",      0x7F7F7FFF},
	{"grey",      0x7F7F7FFF},
	{"red",       0xFF0000FF},
	{"orange",    0xFF7F00FF},
	{"yellow",    0xFFFF00FF},
	{"chartreuse",0x7FFF00FF},
	{"green",     0x00FF00FF},
	{"teal",      0x00FF7FFF},
	{"cyan" ,     0x00FFFFFF},
	{"blue",      0x0000FFFF},
	{"purple",    0x7F00FFFF},
	{"magenta",   0xFF00FFFF},
};

inline int getcolor_unmodified(lua_State *L, int idx, int defaultColor)
{
	int type = lua_type(L,idx);
	switch(type)
	{
		case LUA_TNUMBER:
		{
			return lua_tointeger(L,idx);
		}	break;
		case LUA_TSTRING:
		{
			const char* str = lua_tostring(L,idx);
			if(*str == '#')
			{
				int color;
				sscanf(str+1, "%X", &color);
				int len = strlen(str+1);
				int missing = std::max(0, 8-len);
				color <<= missing << 2;
				if(missing >= 2) color |= 0xFF;
				return color;
			}
			else for(int i = 0; i<sizeof(s_colorMapping)/sizeof(*s_colorMapping); i++)
			{
				if(!stricmp(str,s_colorMapping[i].name))
					return s_colorMapping[i].value;
			}
			if(!strnicmp(str, "rand", 4))
				return ((rand()*255/RAND_MAX) << 8) | ((rand()*255/RAND_MAX) << 16) | ((rand()*255/RAND_MAX) << 24) | 0xFF;
		}	break;
		case LUA_TTABLE:
		{
			int color = 0xFF;
			lua_pushnil(L); // first key
			int keyIndex = lua_gettop(L);
			int valueIndex = keyIndex + 1;
			bool first = true;
			while(lua_next(L, idx))
			{
				bool keyIsString = (lua_type(L, keyIndex) == LUA_TSTRING);
				bool keyIsNumber = (lua_type(L, keyIndex) == LUA_TNUMBER);
				int key = keyIsString ? tolower(*lua_tostring(L, keyIndex)) : (keyIsNumber ? lua_tointeger(L, keyIndex) : 0);
				int value = lua_tointeger(L, valueIndex);
				if(value < 0) value = 0;
				if(value > 255) value = 255;
				switch(key)
				{
				case 1: case 'r': color |= value << 24; break;
				case 2: case 'g': color |= value << 16; break;
				case 3: case 'b': color |= value << 8; break;
				case 4: case 'a': color = (color & ~0xFF) | value; break;
				}
				lua_pop(L, 1);
			}
			return color;
		}	break;
		case LUA_TFUNCTION:
			return 0;
	}
	return defaultColor;
}
int getcolor(lua_State *L, int idx, int defaultColor)
{
	int color = getcolor_unmodified(L, idx, defaultColor);
	LuaContextInfo& info = GetCurrentInfo();
	if(info.transparencyModifier != 255)
	{
		int alpha = (((color & 0xFF) * info.transparencyModifier) / 255);
		if(alpha > 255) alpha = 255;
		color = (color & ~0xFF) | alpha;
	}
	return color;
}

// r,g,b,a = gui.parsecolor(color)
// examples:
// local r,g,b = gui.parsecolor("green")
// local r,g,b,a = gui.parsecolor(0x7F3FFF7F)
DEFINE_LUA_FUNCTION(gui_parsecolor, "color")
{
	int color = getcolor_unmodified(L, 1, 0);
	int r = (color & 0xFF000000) >> 24;
	int g = (color & 0x00FF0000) >> 16;
	int b = (color & 0x0000FF00) >> 8;
	int a = (color & 0x000000FF);
	lua_pushinteger(L, r);
	lua_pushinteger(L, g);
	lua_pushinteger(L, b);
	lua_pushinteger(L, a);
	return 4;
}



static inline void blend32(uint32 *dstPixel, uint32 color)
{
	uint8 *dst = (uint8*) dstPixel;
	uint8 r = (color & 0xFF000000) >> 24;
	uint8 g = (color & 0x00FF0000) >> 16;
	uint8 b = (color & 0x0000FF00) >> 8;
	uint8 a = color & 0x000000FF;

	if (a == 255) {
		// direct copy
		dst[0] = b;
		dst[1] = g;
		dst[2] = r;
		dst[3] = a;
	}
	else if (a == 0) {
		// do not copy
	}
	else {
		// alpha-blending
		// http://en.wikipedia.org/wiki/Alpha_compositing
		uint8 bo = dst[0];
		uint8 go = dst[1];
		uint8 ro = dst[2];
		uint8 ao = dst[3];
		uint8 aof = (ao * (255 - a)) / 255;
		dst[3] = a + aof;
		dst[0] = (b * a + bo * aof) / dst[3];
		dst[1] = (g * a + go * aof) / dst[3];
		dst[2] = (r * a + ro * aof) / dst[3];
	}
}

static LuaGUIData curGuiData;

static void prepare_drawing()
{
	LuaContextInfo& info = GetCurrentInfo();
	curGuiData = info.guiData;
}
static void prepare_reading()
{
	LuaContextInfo& info = GetCurrentInfo();
	curGuiData = info.guiData;
	uint32* buf = luaGuiDataBuf;
	if(buf)
	{
		curGuiData.data = buf;
		curGuiData.stridePix = SNES_WIDTH;
	}
}

// note: prepare_drawing or prepare_reading must be called,
// before any of the following bunch of gui functions will work properly.


// check if a pixel is in the lua canvas
static FORCEINLINE bool gui_checkboundary(int x, int y) {
	return !(x < curGuiData.xMin || x >= curGuiData.xMax || y < curGuiData.yMin || y >= curGuiData.yMax);
}
static FORCEINLINE void gui_adjust_coord(int& x, int& y) {
	x += curGuiData.xOrigin;
	y += curGuiData.yOrigin;
}
static FORCEINLINE bool gui_checkbox(int x1, int y1, int x2, int y2) {
	if((x1 <  curGuiData.xMin && x2 <  curGuiData.xMin)
	|| (x1 >= curGuiData.xMax && x2 >= curGuiData.xMax)
	|| (y1 <  curGuiData.yMin && y2 <  curGuiData.yMin)
	|| (y1 >= curGuiData.yMax && y2 >= curGuiData.yMax))
		return false;
	return true;
}

// write a pixel (do not check boundaries for speedup)
static FORCEINLINE void gui_drawpixel_unchecked(int x, int y, uint32 color) {
	blend32((uint32*) &curGuiData.data[y*curGuiData.stridePix+x], color);
}

// write a pixel (check boundaries)
static FORCEINLINE void gui_drawpixel_checked(int x, int y, uint32 color) {
	if (gui_checkboundary(x, y))
		gui_drawpixel_unchecked(x, y, color);
}

static FORCEINLINE uint32 gui_getpixel_unchecked(int x, int y) {
	return curGuiData.data[y*curGuiData.stridePix+x];
}
static FORCEINLINE uint32 gui_adjust_coord_and_getpixel(int x, int y) {
	x += curGuiData.xOrigin;
	y += curGuiData.yOrigin;
	x = std::min(std::max(x, curGuiData.xMin), curGuiData.xMax-1);
	y = std::min(std::max(y, curGuiData.yMin), curGuiData.yMax-1);
	return gui_getpixel_unchecked(x, y);
}

// draw a line (checks boundaries)
static void gui_drawline_internal(int x1, int y1, int x2, int y2, bool lastPixel, uint32 color)
{
	// Note: New version of Bresenham's Line Algorithm
	// http://groups.google.co.jp/group/rec.games.roguelike.development/browse_thread/thread/345f4c42c3b25858/29e07a3af3a450e6?show_docid=29e07a3af3a450e6

	int swappedx = 0;
	int swappedy = 0;

	int xtemp = x1-x2;
	int ytemp = y1-y2;
	if (xtemp == 0 && ytemp == 0) {
		gui_drawpixel_checked(x1, y1, color);
		return;
	}
	if (xtemp < 0) {
		xtemp = -xtemp;
		swappedx = 1;
	}
	if (ytemp < 0) {
		ytemp = -ytemp;
		swappedy = 1;
	}

	int delta_x = xtemp << 1;
	int delta_y = ytemp << 1;

	signed char ix = x1 > x2?1:-1;
	signed char iy = y1 > y2?1:-1;

	if (lastPixel)
		gui_drawpixel_checked(x2, y2, color);

	if (delta_x >= delta_y) {
		int error = delta_y - (delta_x >> 1);

		while (x2 != x1) {
			if (error == 0 && !swappedx)
				gui_drawpixel_checked(x2+ix, y2, color);
			if (error >= 0) {
				if (error || (ix > 0)) {
					y2 += iy;
					error -= delta_x;
				}
			}
			x2 += ix;
			gui_drawpixel_checked(x2, y2, color);
			if (error == 0 && swappedx)
				gui_drawpixel_checked(x2, y2+iy, color);
			error += delta_y;
		}
	}
	else {
		int error = delta_x - (delta_y >> 1);

		while (y2 != y1) {
			if (error == 0 && !swappedy)
				gui_drawpixel_checked(x2, y2+iy, color);
			if (error >= 0) {
				if (error || (iy > 0)) {
					x2 += ix;
					error -= delta_y;
				}
			}
			y2 += iy;
			gui_drawpixel_checked(x2, y2, color);
			if (error == 0 && swappedy)
				gui_drawpixel_checked(x2+ix, y2, color);
			error += delta_x;
		}
	}
}

#define LuaFontWidth    3
#define LuaFontHeight   6
static const uint8 Small_Font_Data[] =
{
#define I +0,
#define a +1
#define b +2
#define c +4
//  !"#$%&'
      I   b   I a   c I a   c I   b c I a     I   b   I   b   I
      I   b   I a   c I a b c I a b   I     c I a   c I   b   I
      I   b   I       I a   c I   b   I   b   I   b   I       I
      I       I       I a b c I   b c I a     I a   c I       I
      I   b   I       I a   c I a b   I     c I   b c I       I
      I       I       I       I       I       I       I       I
// ()*+,-./
  b   I   b   I       I       I       I       I       I     c I
a     I     c I   b   I   b   I       I       I       I   b   I
a     I     c I a b c I a b c I       I a b c I       I   b   I
a     I     c I   b   I   b   I   b   I       I       I a     I
  b   I   b   I a   c I       I   b   I       I   b   I a     I
      I       I       I       I a     I       I       I       I
// 01234567
  b   I   b   I a b   I a b   I   b   I a b c I   b   I a b c I
a   c I a b   I     c I     c I a     I a     I a     I     c I
a   c I   b   I   b   I a b   I a   c I a b   I a b   I   b   I
a   c I   b   I a     I     c I a b c I     c I a   c I   b   I
  b   I   b   I a b c I a b   I     c I a b   I   b   I   b   I
      I       I       I       I       I       I       I       I
// 89:;<=>?
  b   I   b   I       I       I     c I       I a     I a b   I
a   c I a   c I   b   I       I   b   I a b c I   b   I     c I
  b   I   b c I       I   b   I a     I       I     c I   b   I
a   c I     c I       I       I   b   I a b c I   b   I       I
  b   I   b   I   b   I   b   I     c I       I a     I   b   I
      I       I       I a     I       I       I       I       I
// @ABCDEFG
  b   I   b   I a b   I   b c I a b   I a b c I a b c I   b c I
  b c I a   c I a   c I a     I a   c I a     I a     I a     I
a   c I a b c I a b   I a     I a   c I a b   I a b   I a   c I
  b c I a   c I a   c I a     I a   c I a     I a     I a   c I
      I a   c I a b   I   b c I a b   I a b c I a     I   b c I
      I       I       I       I       I       I       I       I
// HIJKlMNO
a   c I   b   I     c I a   c I a     I a   c I a b   I a b c I
a   c I   b   I     c I a   c I a     I a b c I a   c I a   c I
a b c I   b   I     c I a b   I a     I a   c I a   c I a   c I
a   c I   b   I a   c I a   c I a     I a   c I a   c I a   c I
a   c I   b   I   b   I a   c I a b c I a   c I a   c I a b c I
      I       I       I       I       I       I       I       I
// PQRSTUVW
a b   I a b c I a b   I   b c I a b c I a   c I a   c I a   c I
a   c I a   c I a   c I a     I   b   I a   c I a   c I a   c I
a b   I a   c I a b   I   b   I   b   I a   c I a   c I a   c I
a     I a   c I a   c I     c I   b   I a   c I   b   I a b c I
a     I a b c I a   c I a b   I   b   I a b c I   b   I a   c I
      I     c I       I       I       I       I       I       I
// XYZ[\]^_
a   c I a   c I a b c I   b c I a     I a b   I   b   I       I
a   c I a   c I     c I   b   I   b   I   b   I a   c I       I
  b   I   b   I   b   I   b   I   b   I   b   I       I       I
a   c I   b   I a     I   b   I     c I   b   I       I       I
a   c I   b   I a b c I   b c I     c I a b   I       I a b c I
      I       I       I       I       I       I       I       I
// `abcdefg
a     I       I a     I       I     c I       I   b c I       I
  b   I   b c I a     I   b c I     c I   b c I a     I   b c I
      I a   c I a b   I a     I   b c I a b c I a b   I a   c I
      I a   c I a   c I a     I a   c I a     I a     I   b c I
      I   b c I a b   I   b c I   b c I   b c I a     I     c I
      I       I       I       I       I       I       I a b   I
// hijklmno
a     I   b   I   b   I a     I   b   I       I       I       I
a     I       I       I a     I   b   I a   c I a b   I   b   I
a b   I   b   I   b   I a   c I   b   I a b c I a   c I a   c I
a   c I   b   I   b   I a b   I   b   I a   c I a   c I a   c I
a   c I   b   I   b   I a   c I     c I a   c I a   c I   b   I
      I       I a     I       I       I       I       I       I
// pqrstuvw
      I       I       I       I   b   I       I       I       I
  b   I   b   I a   c I   b c I a b c I a   c I a   c I a   c I
a   c I a   c I a b   I a     I   b   I a   c I a   c I a   c I
a b   I   b c I a     I   b c I   b   I a   c I a   c I a b c I
a     I     c I a     I a b   I     c I   b c I   b   I a   c I
a     I     c I       I       I       I       I       I       I
// xyz{|}~
      I       I       I   b c I   b   I a b   I a b   I       I
a   c I a   c I a b c I   b   I   b   I   b   I     c I   b   I
  b   I a   c I   b   I a b   I       I   b c I       I a   c I
a   c I   b   I a     I   b   I   b   I   b   I       I a b c I
a   c I   b   I a b c I   b c I   b   I a b   I       I       I
      I a     I       I       I       I       I       I       I
#undef I
#undef a
#undef b
#undef c
};

template<int dxdx, int dydy, int dxdy, int dydx>
static void PutTextInternal (const char *str, int len, short x, short y, int color, int backcolor)
{
	int Opac = color & 0xFF;
	int backOpac = backcolor & 0xFF;
	int origX = x;
	int origY = y;

	if(!Opac && !backOpac)
		return;

	while(*str && len)
	{
		if(dydy > 0 && y >= curGuiData.yMax) break;
		if(dydy < 0 && y < curGuiData.yMin) break;
		if(dxdy > 0 && x >= curGuiData.xMax) break;
		if(dxdy < 0 && x < curGuiData.xMin) break;

		int c = *str++;
		if(dxdx > 0 && x >= curGuiData.xMax
		|| dxdx < 0 && x < curGuiData.xMin
		|| dydx > 0 && y >= curGuiData.yMax
		|| dydx < 0 && y < curGuiData.yMin)
		{
			while (c != '\n') {
				c = *str;
				if (c == '\0')
					break;
				str++;
			}
		}

		if(c == '\n')
		{
			if(dydy)
			{
				x = origX;
				y += (LuaFontHeight + 2) * dydy;
			}
			else
			{
				y = origY;
				x += (LuaFontHeight + 2) * dxdy;
			}
			continue;
		}
		else if(c == '\t') // just in case
		{
			const int tabSpace = 8;
			x += (tabSpace-(((x-origX)/(LuaFontWidth+1))%tabSpace))*(LuaFontWidth+1)*dxdx;
			y += (tabSpace-(((y-origY)/(LuaFontWidth+1))%tabSpace))*(LuaFontWidth+1)*dydx;
			continue;
		}
		c -= 32;
		if((unsigned int)c >= 96)
			continue;

		if(c)
		{
			const uint8* Cur_Glyph = (const unsigned char*)&Small_Font_Data + (c%8)+((c/8)*(8*LuaFontHeight));
			for(int y2 = -1; y2 < (LuaFontHeight + 2); y2++)
			{
				for(int x2 = -1; x2 < (LuaFontWidth + 1); x2++)
				{
					bool on = y2 >= 0 && y2 < LuaFontHeight && (Cur_Glyph[y2*8] & (1 << x2));
					if(on)
					{
						gui_drawpixel_checked(x+x2*dxdx+y2*dxdy, y+y2*dydy+x2*dydx, color);
					}
					else if(backOpac)
					{
						for(int y3 = std::max(0,y2-1); y3 <= std::min(LuaFontHeight-1,y2+1); y3++)
						{
							for(int x3 = std::max(0,x2-1); x3 <= std::min(LuaFontWidth-1,x2+1); x3++)
							{
								on |= y3 >= 0 && y3 < LuaFontHeight && (Cur_Glyph[y3*8] & (1 << x3));
								if (on)
									goto draw_outline; // speedup?
							}
						}
						if(on)
						{
draw_outline:
							gui_drawpixel_checked(x+x2*dxdx+y2*dxdy, y+y2*dydy+x2*dydx, backcolor);
						}
					}
				}
			}
		}

		x += (LuaFontWidth+1)*dxdx;
		y += (LuaFontWidth+1)*dydx;
		len--;
	}
}

static int strlinelen(const char* string)
{
	const char* s = string;
	while(*s && *s != '\n')
		s++;
	if(*s)
		s++;
	return s - string;
}

static void LuaDisplayString (const char *str, int x, int y, uint32 color, uint32 outlineColor)
{
	if(!str)
		return;

#if 1
	//if(rotate == 0)
		PutTextInternal<1,1,0,0>(str, strlen(str), x, y, color, outlineColor);
	//else if(rotate == 90)
	//	PutTextInternal<0,0,1,-1>(str, strlen(str), x, y, color, outlineColor);
	//else if
#else
	const char* ptr = str;
	while(*ptr && y < curGuiData.yMax)
	{
		int len = strlinelen(ptr);
		int skip = 0;
		if(len < 1) len = 1;

		// break up the line if it's too long to display otherwise
		if(len > 63)
		{
			len = 63;
			const char* ptr2 = ptr + len-1;
			for(int j = len-1; j; j--, ptr2--)
			{
				if(*ptr2 == ' ' || *ptr2 == '\t')
				{
					len = j;
					skip = 1;
					break;
				}
			}
		}

		int xl = 0;
		int yl = curGuiData.yMin;
		int xh = (curGuiData.xMax - 1 - 1) - 4*len;
		int yh = curGuiData.yMax - 1;
		int x2 = std::min(std::max(x,xl),xh);
		int y2 = std::min(std::max(y,yl),yh);

		PutTextInternal<1,1,0,0>(ptr,len,x2,y2,color,outlineColor);

		ptr += len + skip;
		y += 8;
	}
#endif
}




DEFINE_LUA_FUNCTION(gui_text, "x,y,str[,color=\"white\"[,outline=\"black\"]]")
{
	int x = luaL_checkinteger(L,1); // have to check for errors before deferring
	int y = luaL_checkinteger(L,2);

	if(DeferGUIFuncIfNeeded(L))
		return 0; // we have to wait until later to call this function because we haven't emulated the next frame yet
		          // (the only way to avoid this deferring is to be in a gui.register or emu.registerafter callback)

	const char* str = toCString(L,3); // better than using luaL_checkstring here (more permissive)
	
	if(str && *str)
	{
		int foreColor = getcolor(L,4,0xFFFFFFFF);
		int backColor = getcolor(L,5,0x000000FF);

		prepare_drawing();
		gui_adjust_coord(x,y);

		LuaDisplayString(str, x, y, foreColor, backColor);
	}

	return 0;
}

DEFINE_LUA_FUNCTION(gui_box, "x1,y1,x2,y2[,fill[,outline]]")
{
	int x1 = luaL_checkinteger(L,1); // have to check for errors before deferring
	int y1 = luaL_checkinteger(L,2);
	int x2 = luaL_checkinteger(L,3);
	int y2 = luaL_checkinteger(L,4);

	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int fillcolor = getcolor(L,5,0xFFFFFF3F);
	int outlinecolor = getcolor(L,6,fillcolor|0xFF);

	prepare_drawing();
	gui_adjust_coord(x1,y1);
	gui_adjust_coord(x2,y2);

	if(!gui_checkbox(x1,y1,x2,y2))
		return 0;

	// require x1,y1 <= x2,y2
	if (x1 > x2)
		std::swap(x1,x2);
	if (y1 > y2)
		std::swap(y1,y2);

	// avoid trying to draw lots of offscreen pixels
	// (this is intentionally 1 out from the edge here)
	x1 = std::min(std::max(x1, curGuiData.xMin-1), curGuiData.xMax);
	x2 = std::min(std::max(x2, curGuiData.xMin-1), curGuiData.xMax);
	y1 = std::min(std::max(y1, curGuiData.yMin-1), curGuiData.yMax);
	y2 = std::min(std::max(y2, curGuiData.yMin-1), curGuiData.yMax);

	if(outlinecolor & 0xFF)
	{
		if(y1 >= curGuiData.yMin)
			for (short x = x1+1; x < x2; x++)
				gui_drawpixel_unchecked(x,y1,outlinecolor);

		if(x1 >= curGuiData.xMin && x1 < curGuiData.xMax)
		{
			if(y1 >= curGuiData.yMin)
				gui_drawpixel_unchecked(x1,y1,outlinecolor);
			for (short y = y1+1; y < y2; y++)
				gui_drawpixel_unchecked(x1,y,outlinecolor);
			if(y2 < curGuiData.yMax)
				gui_drawpixel_unchecked(x1,y2,outlinecolor);
		}

		if(y1 != y2 && y2 < curGuiData.yMax)
			for (short x = x1+1; x < x2; x++)
				gui_drawpixel_unchecked(x,y2,outlinecolor);
		if(x1 != x2 && x2 >= curGuiData.xMin && x2 < curGuiData.xMax)
		{
			if(y1 >= curGuiData.yMin)
				gui_drawpixel_unchecked(x2,y1,outlinecolor);
			for (short y = y1+1; y < y2; y++)
				gui_drawpixel_unchecked(x2,y,outlinecolor);
			if(y2 < curGuiData.yMax)
				gui_drawpixel_unchecked(x2,y2,outlinecolor);
		}
	}

	if(fillcolor & 0xFF)
	{
		for(short y = y1+1; y <= y2-1; y++)
			for(short x = x1+1; x <= x2-1; x++)
				gui_drawpixel_unchecked(x,y,fillcolor);
	}

	return 0;
}
// gui.setpixel(x,y,color)
// color can be a RGB web color like '#ff7030', or with alpha RGBA like '#ff703060'
//   or it can be an RGBA hex number like 0xFF703060
//   or it can be a preset color like 'red', 'orange', 'blue', 'white', etc.
DEFINE_LUA_FUNCTION(gui_pixel, "x,y[,color=\"white\"]")
{
	int x = luaL_checkinteger(L,1); // have to check for errors before deferring
	int y = luaL_checkinteger(L,2);

	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int color = getcolor(L,3,0xFFFFFFFF);
	if(color & 0xFF)
	{
		prepare_drawing();
		gui_adjust_coord(x,y);
		gui_drawpixel_checked(x, y, color);
	}

	return 0;
}

static FORCEINLINE void RGB555ToRGB888(uint32& r, uint32& g, uint32& b, bool rotateColorBit = true)
{
	if (rotateColorBit) {
		// Diagram:   000XXxxx -> XXxxx000 -> XXxxxXXx
		// ">> 5" would be wrong, but this is compatible with the
		// PNG image generated by screenshot.cpp, as far as I see.
		r = ((r << 3) & 0xff) | ((r >> 2) & 0xff);
		g = ((g << 3) & 0xff) | ((g >> 2) & 0xff);
		b = ((b << 3) & 0xff) | ((b >> 2) & 0xff);
	}
	else {
		r = ((r << 3) & 0xff);
		g = ((g << 3) & 0xff);
		b = ((b << 3) & 0xff);
	}
}

// r,g,b = gui.getpixel(x,y)
DEFINE_LUA_FUNCTION(gui_getpixel, "x,y")
{
	prepare_reading();

	int x = luaL_checkinteger(L,1);
	int y = luaL_checkinteger(L,2);

	// adjust coordinates
	x += curGuiData.xOrigin;
	y += curGuiData.yOrigin;
	x = std::min(std::max(x, curGuiData.xMin), IPPU.RenderedScreenWidth-1);
	y = std::min(std::max(y, curGuiData.yMin), IPPU.RenderedScreenHeight-1);

	uint32 r, g, b;
	DECOMPOSE_PIXEL(GFX.Screen[x + y * GFX.RealPPL], r, g, b);
	RGB555ToRGB888(r, g, b);

	lua_pushinteger(L, r);
	lua_pushinteger(L, g);
	lua_pushinteger(L, b);

	return 3;
}
DEFINE_LUA_FUNCTION(gui_line, "x1,y1,x2,y2[,color=\"white\"[,skipfirst=false]]")
{
	int x1 = luaL_checkinteger(L,1); // have to check for errors before deferring
	int y1 = luaL_checkinteger(L,2);
	int x2 = luaL_checkinteger(L,3);
	int y2 = luaL_checkinteger(L,4);

	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int color = getcolor(L,5,0xFFFFFFFF);
	int skipFirst = lua_toboolean(L,6);

	if(!(color & 0xFF))
		return 0;

	prepare_drawing();
	gui_adjust_coord(x1,y1);
	gui_adjust_coord(x2,y2);

	if(!gui_checkbox(x1,y1,x2,y2))
		return 0;

	gui_drawline_internal(x2, y2, x1, y1, !skipFirst, color);

	return 0;
}

// gui.opacity(number alphaValue)
// sets the transparency of subsequent draw calls
// 0.0 is completely transparent, 1.0 is completely opaque
// non-integer values are supported and meaningful, as are values greater than 1.0
// it is not necessary to use this function to get transparency (or the less-recommended gui.transparency() either),
// because you can provide an alpha value in the color argument of each draw call.
// however, it can be convenient to be able to globally modify the drawing transparency
DEFINE_LUA_FUNCTION(gui_setopacity, "alpha_0_to_1")
{
	lua_Number opacF = luaL_checknumber(L,1);
	opacF *= 255.0;
	if(opacF < 0) opacF = 0;
	int opac;
	lua_number2int(opac, opacF);
	LuaContextInfo& info = GetCurrentInfo();
	info.transparencyModifier = opac;
	return 0;
}

// gui.transparency(number transparencyValue)
// sets the transparency of subsequent draw calls
// 0.0 is completely opaque, 4.0 is completely transparent
// non-integer values are supported and meaningful, as are values less than 0.0
// this is a legacy function, and the range is from 0 to 4 solely for this reason
// it does the exact same thing as gui.opacity() but with a different argument range
DEFINE_LUA_FUNCTION(gui_settransparency, "transparency_4_to_0")
{
	lua_Number transp = luaL_checknumber(L,1);
	lua_Number opacF = 4 - transp;
	opacF *= 255.0 / 4.0;
	if(opacF < 0) opacF = 0;
	int opac;
	lua_number2int(opac, opacF);
	LuaContextInfo& info = GetCurrentInfo();
	info.transparencyModifier = opac;
	return 0;
}

// takes a screenshot and returns it in gdstr format
// example: gd.createFromGdStr(gui.gdscreenshot()):png("outputimage.png")
DEFINE_LUA_FUNCTION(gui_gdscreenshot, "")
{
	int width = IPPU.RenderedScreenWidth;
	int height = IPPU.RenderedScreenHeight;

	int imgwidth = width;
	int imgheight = height;

	int StretchScreenshots = 1; //Settings.StretchScreenshots;
	if (StretchScreenshots == 1)
	{
		if (width > SNES_WIDTH && height <= SNES_HEIGHT_EXTENDED)
			imgheight = height << 1;
	}
	else
	if (StretchScreenshots == 2)
	{
		if (width  <= SNES_WIDTH)
			imgwidth  = width  << 1;
		if (height <= SNES_HEIGHT_EXTENDED)
			imgheight = height << 1;
	}

	int size = 11 + imgwidth * imgheight * 4;
	char* str = new char[size+1];
	str[size] = 0;
	unsigned char* ptr = (unsigned char*)str;

	// GD format header for truecolor image (11 bytes)
	*ptr++ = (65534 >> 8) & 0xFF;
	*ptr++ = (65534     ) & 0xFF;
	*ptr++ = (imgwidth >> 8) & 0xFF;
	*ptr++ = (imgwidth     ) & 0xFF;
	*ptr++ = (imgheight >> 8) & 0xFF;
	*ptr++ = (imgheight     ) & 0xFF;
	*ptr++ = 1;
	*ptr++ = 255;
	*ptr++ = 255;
	*ptr++ = 255;
	*ptr++ = 255;

	uint16 *screen = GFX.Screen;

	for (int y = 0; y < height; y++, screen += GFX.RealPPL)
	{
		for (int x = 0; x < width; x++)
		{
			uint32	r, g, b;

			DECOMPOSE_PIXEL(screen[x], r, g, b);
			RGB555ToRGB888(r, g, b);

			*(ptr++) = 0;
			*(ptr++) = r;
			*(ptr++) = g;
			*(ptr++) = b;

			if (imgwidth != width)
			{
				*(ptr++) = 0;
				*(ptr++) = r;
				*(ptr++) = g;
				*(ptr++) = b;
			}
		}
		if (imgheight != height) {
			size_t lineBufSize = width * sizeof(uint32);
			memcpy(ptr, ptr - lineBufSize, lineBufSize);
			ptr += lineBufSize;
		}
	}

	lua_pushlstring(L, str, size);
	delete[] str;
	return 1;
}

// draws a gd image that's in gdstr format to the screen
// example: gui.gdoverlay(gd.createFromPng("myimage.png"):gdStr())
DEFINE_LUA_FUNCTION(gui_gdoverlay, "[dx=0,dy=0,]gdimage[,sx=0,sy=0,width,height][,alphamul]")
{
	int xStartDst = 0;
	int yStartDst = 0;
	int xStartSrc = 0;
	int yStartSrc = 0;
	int width, height;
	int numArgs = lua_gettop(L);

	int index = 1;
	if(lua_type(L,index) == LUA_TNUMBER)
	{
		xStartDst = lua_tointeger(L,index++);
		if(lua_type(L,index) == LUA_TNUMBER)
			yStartDst = lua_tointeger(L,index++);
	}

	luaL_checktype(L,index,LUA_TSTRING); // have to check for errors before deferring

	if(DeferGUIFuncIfNeeded(L))
		return 0;

	const unsigned char* ptr = (const unsigned char*)lua_tostring(L,index++);

	const bool defSrcRect = ((numArgs - index + 1) < 2);
	if (!defSrcRect) {
		xStartSrc = luaL_checkinteger(L, index++);
		yStartSrc = luaL_checkinteger(L, index++);
		width = luaL_checkinteger(L, index++);
		height = luaL_checkinteger(L, index++);
	}

	LuaContextInfo& info = GetCurrentInfo();
	int alphaMul = info.transparencyModifier;
	if(lua_isnumber(L, index))
		alphaMul = (int)(alphaMul * lua_tonumber(L, index++));
	if(alphaMul <= 0)
		return 0;

	// since there aren't that many possible opacity levels,
	// do the opacity modification calculations beforehand instead of per pixel
	int opacMap[256];
	for(int i = 0; i < 128; i++)
	{
		int opac = 255 - ((i << 1) | (i & 1)); // gdAlphaMax = 127, not 255
		opac = (opac * alphaMul) / 255;
		if(opac < 0) opac = 0;
		if(opac > 255) opac = 255;
		opacMap[i] = opac;
	}
	for(int i = 128; i < 256; i++)
		opacMap[i] = 0; // what should we do for them, actually?

	// GD format header for truecolor image (11 bytes)
	ptr++;
	bool trueColor = (*ptr++ == 254);
	int gdWidth = *ptr++ << 8;
	gdWidth |= *ptr++;
	int gdHeight = *ptr++ << 8;
	gdHeight |= *ptr++;
	int bytespp = (trueColor ? 4 : 1);
	if (defSrcRect) {
		width = gdWidth;
		height = gdHeight;
	}

	if ((!trueColor && *ptr) || (trueColor && !*ptr)) {
		luaL_error(L, "gdoverlay: inconsistent color type.");
		return 0;
	}
	ptr++;
	int colorsTotal = 0;
	if (!trueColor) {
		colorsTotal = *ptr++ << 8;
		colorsTotal |= *ptr++;
	}
	int transparent = *ptr++ << 24;
	transparent |= *ptr++ << 16;
	transparent |= *ptr++ << 8;
	transparent |= *ptr++;
	struct { int r, g, b, a; } pal[256];
	if (!trueColor) for (int i = 0; i < 256; i++) {
		pal[i].r = *ptr++;
		pal[i].g = *ptr++;
		pal[i].b = *ptr++;
		pal[i].a = opacMap[*ptr++];
	}

	prepare_drawing();
	uint8* Dst = (uint8*)curGuiData.data;
	gui_adjust_coord(xStartDst,yStartDst);

	int xMin = curGuiData.xMin;
	int yMin = curGuiData.yMin;
	int xMax = curGuiData.xMax - 1;
	int yMax = curGuiData.yMax - 1;
	int strideBytes = curGuiData.stridePix * 4;

	// limit source rect
	if (xStartSrc < 0) {
		width += xStartSrc;
		xStartDst -= xStartSrc;
		xStartSrc = 0;
	}
	if (yStartSrc < 0) {
		height += yStartSrc;
		yStartDst -= yStartSrc;
		yStartSrc = 0;
	}
	if (xStartSrc + width >= gdWidth)
		width = gdWidth - xStartSrc;
	if (yStartSrc+height >= gdHeight)
		height = gdHeight - yStartSrc;
	if (width <= 0 || height <= 0)
		return 0;
	ptr += (yStartSrc * gdWidth + xStartSrc) * bytespp;

	Dst += yStartDst * strideBytes;
	for(int y = yStartDst; y < height+yStartDst && y < yMax; y++, Dst += strideBytes)
	{
		if(y < yMin)
			ptr += gdWidth * bytespp;
		else
		{
			int xA = (xStartDst < xMin ? xMin : xStartDst);
			int xB = (xStartDst+width > xMax ? xMax : xStartDst+width);
			ptr += (xA - xStartDst) * bytespp;
			for(int x = xA; x < xB; x++)
			{
				if (trueColor) {
					int opac = opacMap[ptr[0]];
					uint32 pix = (opac|(ptr[3]<<8)|(ptr[2]<<16)|(ptr[1]<<24));
					blend32((uint32*)(Dst+x*4), pix);
					ptr += 4;
				}
				else {
					int palNo = ptr[0];
					uint32 pix = (pal[palNo].a|(pal[palNo].b<<8)|(pal[palNo].g<<16)|(pal[palNo].r<<24));
					blend32((uint32*)(Dst+x*4), pix);
					ptr++;
				}
			}
			ptr += (gdWidth - (xB - xStartDst)) * bytespp;
		}
	}

	return 0;
}

DEFINE_LUA_FUNCTION(gui_savescreenshot, "[filename]")
{
	bool8 result;
	if (lua_type(L,1) == LUA_TSTRING)
	{
		result = S9xDoScreenshot(lua_tostring(L,1), IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
	}
	else
	{
		result = S9xDoScreenshot(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
	}
	lua_pushboolean(L, result);
	return 1;
}

static FORCEINLINE uint8 CalcBlend8(uint8 dst, uint8 src, uint8 alpha)
{
	if (alpha == 0)
		return dst;
	else if (alpha == 255)
		return src;
	else
		return (uint8)((((int) src - dst) * alpha / 255 + dst) & 0xFF);
}

static FORCEINLINE void ParseColor16(uint8 *src, uint8 *r, uint8 *g, uint8 *b, uint8 *a)
{
	uint16 color = *(uint16*)src;
	uint32 rv, gv, bv;
	DECOMPOSE_PIXEL(color, rv, gv, bv);
	*b = bv << 3;
	*g = gv << 3;
	*r = rv << 3;
	if (a != NULL)
		*a = 255;
}

static FORCEINLINE void ParseColor24(uint8 *src, uint8 *r, uint8 *g, uint8 *b, uint8 *a)
{
	*b = src[0];
	*g = src[1];
	*r = src[2];
	if (a != NULL)
		*a = 255;
}

static FORCEINLINE void ParseColor32(uint8 *src, uint8 *r, uint8 *g, uint8 *b, uint8 *a)
{
	*b = src[0];
	*g = src[1];
	*r = src[2];
	if (a != NULL)
		*a = src[3];
}

static FORCEINLINE void WriteColor16(uint8 *dst, uint8 r, uint8 g, uint8 b)
{
	*(uint16*)dst = BUILD_PIXEL(r >> 3, g >> 3, b >> 3);
}

static FORCEINLINE void WriteColor24(uint8 *dst, uint8 r, uint8 g, uint8 b)
{
	dst[0] = b;
	dst[1] = g;
	dst[2] = r;
}

static FORCEINLINE void WriteColor32(uint8 *dst, uint8 r, uint8 g, uint8 b)
{
	dst[0] = b;
	dst[1] = g;
	dst[2] = r;
	dst[3] = 255; // just in case
}
 
// draw Lua GUI to specified screen buffer
void DrawLuaGuiToScreen(void *s, int width, int height, int bpp, int pitch, bool clear)
{
	if (width % SNES_WIDTH != 0) {
		//assert(width % SNES_WIDTH == 0);
		return;
	}

	if (height % SNES_HEIGHT != 0 && height % SNES_HEIGHT_EXTENDED != 0) {
		//assert(height % SNES_HEIGHT == 0 || height % SNES_HEIGHT_EXTENDED == 0);
		return;
	}

	if (bpp != 16 && bpp != 24 && bpp != 32) {
		assert(bpp == 16 || bpp == 24 || bpp == 32);
		return;
	}

	// scaled transfer is necessary for SNES hi-res & interlace
	int xscale, yscale;
	xscale = width / SNES_WIDTH;
	if (height % SNES_HEIGHT_EXTENDED == 0)
		yscale = height / SNES_HEIGHT_EXTENDED;
	else
		yscale = height / SNES_HEIGHT;

	const int luaScreenWidth = SNES_WIDTH;
	const int luaScreenHeight = SNES_HEIGHT_EXTENDED;

	for (int y = 0; y < height && y < luaScreenHeight; y++)
	{
		for (int x = 0; x < width && x < luaScreenWidth; x++)
		{
			uint8 *src_px = (uint8*)(&luaGuiDataBuf[y * luaScreenWidth + x]);
			uint8 src_r, src_g, src_b, src_a;
			ParseColor32(src_px, &src_r, &src_g, &src_b, &src_a);
			if (src_a == 0)
				continue;

			for (int yscalei = 0; yscalei < yscale; yscalei++)
			{
				for (int xscalei = 0; xscalei < xscale; xscalei++)
				{
					const int x_dst = (x * xscale) + xscalei;
					const int y_dst = (y * yscale) + yscalei;
					uint8 *dst_px = &((uint8*)s)[y_dst * pitch + x_dst * (bpp / 8)];

					if (src_a == 255)
					{
						// direct copy
						switch(bpp) {
						case 16: WriteColor16(dst_px, src_r, src_g, src_b); break;
						case 24: WriteColor24(dst_px, src_r, src_g, src_b); break;
						case 32: WriteColor32(dst_px, src_r, src_g, src_b); break;
						}
					}
					else
					{
						// alpha-blend
						uint8 dst_r, dst_g, dst_b;
						switch(bpp) {
						case 16: ParseColor16(dst_px, &dst_r, &dst_g, &dst_b, NULL); break;
						case 24: ParseColor24(dst_px, &dst_r, &dst_g, &dst_b, NULL); break;
						case 32: ParseColor32(dst_px, &dst_r, &dst_g, &dst_b, NULL); break;
						}

						switch(bpp) {
						case 16: WriteColor16(dst_px, CalcBlend8(dst_r, src_r, src_a), CalcBlend8(dst_g, src_g, src_a), CalcBlend8(dst_b, src_b, src_a)); break;
						case 24: WriteColor24(dst_px, CalcBlend8(dst_r, src_r, src_a), CalcBlend8(dst_g, src_g, src_a), CalcBlend8(dst_b, src_b, src_a)); break;
						case 32: WriteColor32(dst_px, CalcBlend8(dst_r, src_r, src_a), CalcBlend8(dst_g, src_g, src_a), CalcBlend8(dst_b, src_b, src_a)); break;
						}
					}
				}
			}
		}
	}

	if (clear)
		ClearLuaGui();
	return;
}

void ClearLuaGui(void)
{
	memset(luaGuiDataBuf, 0, SNES_WIDTH * SNES_HEIGHT_EXTENDED * sizeof(uint32));
}

static void GetCurrentScriptDir(char* buffer, int bufLen)
{
	LuaContextInfo& info = GetCurrentInfo();
	strncpy(buffer, info.lastFilename.c_str(), bufLen);
	buffer[bufLen-1] = 0;
	char* slash = std::max(strrchr(buffer, '/'), strrchr(buffer, '\\'));
	if(slash)
		slash[1] = 0;
}

DEFINE_LUA_FUNCTION(emu_openscript, "filename")
{
#ifdef __WIN32__
	char curScriptDir[1024]; GetCurrentScriptDir(curScriptDir, 1024); // make sure we can always find scripts that are in the same directory as the current script
	const char* filename = lua_isstring(L,1) ? lua_tostring(L,1) : NULL;
	extern const char* OpenLuaScript(const char* filename, const char* extraDirToCheck, bool makeSubservient);
	const char* errorMsg = OpenLuaScript(filename, curScriptDir, true);
	if(errorMsg)
		luaL_error(L, errorMsg);
#endif
    return 0;
}
/*
DEFINE_LUA_FUNCTION(emu_loadrom, "filename")
{
	struct Temp { Temp() {EnableStopAllLuaScripts(false);} ~Temp() {EnableStopAllLuaScripts(true);}} dontStopScriptsHere;
	const char* filename = lua_isstring(L,1) ? lua_tostring(L,1) : NULL;
	char curScriptDir[1024]; GetCurrentScriptDir(curScriptDir, 1024);
	filename = MakeRomPathAbsolute(filename, curScriptDir);
	int result = GensLoadRom(filename);
	if(result <= 0)
		luaL_error(L, "Failed to load ROM \"%s\": %s", filename, result ? "invalid or unsupported" : "cancelled or not found");
	CallRegisteredLuaFunctions(LUACALL_ONSTART);
    return 0;
}
*/
DEFINE_LUA_FUNCTION(emu_getframecount, "")
{
	int offset = 1;
	if (S9xMovieActive())
		lua_pushinteger(L, S9xMovieGetFrameCounter() + offset);
	else
		lua_pushinteger(L, IPPU.TotalEmulatedFrames + offset);
	return 1;
}
DEFINE_LUA_FUNCTION(emu_getlagcount, "")
{
	lua_pushinteger(L, IPPU.PadIgnoredFrames);
	return 1;
}
DEFINE_LUA_FUNCTION(emu_lagged, "")
{
	extern bool8 pad_read;
	lua_pushboolean(L, !pad_read);
	return 1;
}
DEFINE_LUA_FUNCTION(emu_emulating, "")
{
	lua_pushboolean(L, !Settings.StopEmulation);
	return 1;
}
DEFINE_LUA_FUNCTION(emu_atframeboundary, "")
{
	lua_pushboolean(L, !IPPU.InMainLoop);
	return 1;
}
DEFINE_LUA_FUNCTION(movie_getlength, "")
{
	lua_pushinteger(L, S9xMovieGetLength());
	return 1;
}
DEFINE_LUA_FUNCTION(movie_isactive, "")
{
	lua_pushboolean(L, S9xMovieActive());
	return 1;
}
DEFINE_LUA_FUNCTION(movie_rerecordcount, "")
{
	lua_pushinteger(L, S9xMovieGetRerecordCount());
	return 1;
}
DEFINE_LUA_FUNCTION(movie_setrerecordcount, "")
{
	S9xMovieSetRerecordCount(luaL_checkinteger(L, 1));
	return 0;
}
DEFINE_LUA_FUNCTION(emu_rerecordcounting, "[enabled]")
{
	LuaContextInfo& info = GetCurrentInfo();
	if(lua_gettop(L) == 0)
	{
		// if no arguments given, return the current value
		lua_pushboolean(L, !info.rerecordCountingDisabled);
		return 1;
	}
	else
	{
		// set rerecord disabling
		info.rerecordCountingDisabled = !lua_toboolean(L,1);
		return 0;
	}
}
DEFINE_LUA_FUNCTION(movie_getreadonly, "")
{
#ifdef __WIN32__
	if (S9xMovieActive())
		lua_pushboolean(L, S9xMovieReadOnly());
	else
		lua_pushboolean(L, GUI.MovieReadOnly);
#else
	lua_pushboolean(L, S9xMovieReadOnly());
#endif
	return 1;
}
DEFINE_LUA_FUNCTION(movie_setreadonly, "readonly")
{
	int readonly = lua_toboolean(L,1) ? 1 : 0;
	S9xMovieSetReadOnly(readonly);
#ifdef __WIN32__
	GUI.MovieReadOnly = readonly;
#endif
	return 0;
}
DEFINE_LUA_FUNCTION(movie_isrecording, "")
{
	lua_pushboolean(L, S9xMovieRecording());
	return 1;
}
DEFINE_LUA_FUNCTION(movie_isplaying, "")
{
	lua_pushboolean(L, S9xMoviePlaying());
	return 1;
}
DEFINE_LUA_FUNCTION(movie_getmode, "")
{
	if (!S9xMovieActive())
		lua_pushstring(L, "inactive");
	else if (S9xMoviePlaying())
		lua_pushstring(L, "playback");
	else if (S9xMovieRecording())
		lua_pushstring(L, "record");
	else if (S9xMovieFinished())
		lua_pushstring(L, "finished");
	else
		lua_pushnil(L);
	return 1;
}
DEFINE_LUA_FUNCTION(movie_getname, "")
{
	lua_pushstring(L, S9xMovieGetFilename());
	return 1;
}
// movie.play() -- plays a movie of the user's choice
// movie.play(filename) -- starts playing a particular movie
// throws an error (with a description) if for whatever reason the movie couldn't be played
DEFINE_LUA_FUNCTION(movie_play, "[filename]")
{
#ifdef __WIN32__
	bool8 readonly = GUI.MovieReadOnly;
#else
	bool8 readonly = FALSE;
#endif
	const char* filename = lua_isstring(L,1) ? lua_tostring(L,1) : NULL;
	int err = S9xMovieOpen (filename, readonly);
	if(err != SUCCESS)
	{
		char* errorMsg = "Could not open movie file.";
		switch(err)
		{
		case FILE_NOT_FOUND:
			errorMsg = "File not found.";
			break;
		case WRONG_FORMAT:
			errorMsg = "Unrecognized format.";
			break;
		case WRONG_VERSION:
			errorMsg = "Unsupported movie version.";
			break;
		}
		luaL_error(L, errorMsg);
		return false;
	}
    return 0;
}
DEFINE_LUA_FUNCTION(movie_replay, "")
{
	if(!S9xMovieActive())
		return 0;
	lua_settop(L, 0);
	movie_getname(L);
	return movie_play(L);
}
DEFINE_LUA_FUNCTION(movie_close, "")
{
	S9xMovieShutdown();
	return 0;
}

#ifdef __WIN32__
const char* s_keyToName[256] =
{
	NULL,
	"leftclick",
	"rightclick",
	NULL,
	"middleclick",
	NULL,
	NULL,
	NULL,
	"backspace",
	"tab",
	NULL,
	NULL,
	NULL,
	"enter",
	NULL,
	NULL,
	"shift", // 0x10
	"control",
	"alt",
	"pause",
	"capslock",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"escape",
	NULL,
	NULL,
	NULL,
	NULL,
	"space", // 0x20
	"pageup",
	"pagedown",
	"end",
	"home",
	"left",
	"up",
	"right",
	"down",
	NULL,
	NULL,
	NULL,
	NULL,
	"insert",
	"delete",
	NULL,
	"0","1","2","3","4","5","6","7","8","9",
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	"A","B","C","D","E","F","G","H","I","J",
	"K","L","M","N","O","P","Q","R","S","T",
	"U","V","W","X","Y","Z",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"numpad0","numpad1","numpad2","numpad3","numpad4","numpad5","numpad6","numpad7","numpad8","numpad9",
	"numpad*","numpad+",
	NULL,
	"numpad-","numpad.","numpad/",
	"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
	"F13","F14","F15","F16","F17","F18","F19","F20","F21","F22","F23","F24",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"numlock",
	"scrolllock",
	NULL, // 0x92
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, // 0xB9
	"semicolon",
	"plus",
	"comma",
	"minus",
	"period",
	"slash",
	"tilde",
	NULL, // 0xC1
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, // 0xDA
	"leftbracket",
	"backslash",
	"rightbracket",
	"quote",
};
#endif


// input.get()
// takes no input, returns a lua table of entries representing the current input state,
// independent of the joypad buttons the emulated game thinks are pressed
// for example:
//   if the user is holding the W key and the left mouse button
//   and has the mouse at the bottom-right corner of the game screen,
//   then this would return {W=true, leftclick=true, xmouse=319, ymouse=223}
int input_getcurrentinputstatus(lua_State* L, bool reportUp, bool reportDown)
{
	lua_newtable(L);

#ifdef __WIN32__
	// keyboard and mouse button status
	{
		int BackgroundInput = GUI.BackgroundInput;

		unsigned char keys [256];
		if(!BackgroundInput)
		{
			if(GetKeyboardState(keys))
			{
				for(int i = 1; i < 255; i++)
				{
					int mask = (i == VK_CAPITAL || i == VK_NUMLOCK || i == VK_SCROLL) ? 0x01 : 0x80;
					int active = keys[i] & mask;
					if((active && reportDown) || (!active && reportUp))
					{
						const char* name = s_keyToName[i];
						if(name)
						{
							lua_pushboolean(L, active);
							lua_setfield(L, -2, name);
						}
					}
				}
			}
		}
		else // use a slightly different method that will detect background input:
		{
			for(int i = 1; i < 255; i++)
			{
				const char* name = s_keyToName[i];
				if(name)
				{
					int active;
					if(i == VK_CAPITAL || i == VK_NUMLOCK || i == VK_SCROLL)
						active = GetKeyState(i) & 0x01;
					else
						active = GetAsyncKeyState(i) & 0x8000;
					if((active && reportDown) || (!active && reportUp))
					{
						lua_pushboolean(L, active);
						lua_setfield(L, -2, name);
					}
				}
			}
		}
	}
	// mouse position in game screen pixel coordinates
	{
		POINT point;
		GetCursorPos(&point);
		ScreenToClient(GUI.hWnd, &point);
		extern void ClientToSNESScreen(LPPOINT lpPoint, bool clip);
		ClientToSNESScreen(&point, false);
		lua_pushinteger(L, point.x);
		lua_setfield(L, -2, "xmouse");
		lua_pushinteger(L, point.y);
		lua_setfield(L, -2, "ymouse");
	}
#else
	// NYI (well, return an empty table)
#endif

	return 1;
}
// input.get()
// returns a table of every keyboard button,
DEFINE_LUA_FUNCTION(input_get, "")
{
	return input_getcurrentinputstatus(L, true, true);
}
// input.getdown()
// returns a table of every keyboard button that is currently held
DEFINE_LUA_FUNCTION(input_getdown, "")
{
	return input_getcurrentinputstatus(L, false, true);
}
// input.getup()
// returns a table of every keyboard button that is not currently held
DEFINE_LUA_FUNCTION(input_getup, "")
{
	return input_getcurrentinputstatus(L, true, false);
}


#include "../apu/bapu/snes/snes.hpp"
#if defined(DEBUGGER)
	extern SNES::SMPDebugger SNES::smp;
#else
	extern SNES::SMP SNES::smp;
#endif
#define APURAM  SNES::smp.apuram

DEFINE_LUA_FUNCTION(apu_readbyte, "address")
{
	int address = lua_tointeger(L,1);
	if (address < 0x0000 || address > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	uint8 value = APURAM[address];
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1; // we return the number of return values
}
DEFINE_LUA_FUNCTION(apu_readbytesigned, "address")
{
	int address = lua_tointeger(L,1);
	if (address < 0x0000 || address > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	int8 value = APURAM[address];
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
DEFINE_LUA_FUNCTION(apu_readword, "address")
{
	int address = lua_tointeger(L,1);
	if (address < 0x0000 || (address + 1) > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	uint16 value = APURAM[address];
	value |= APURAM[address + 1] << 8;
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
DEFINE_LUA_FUNCTION(apu_readwordsigned, "address")
{
	int address = lua_tointeger(L,1);
	if (address < 0x0000 || (address + 1) > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	int16 value = APURAM[address];
	value |= APURAM[address + 1] << 8;
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
DEFINE_LUA_FUNCTION(apu_readdword, "address")
{
	int address = luaL_checkinteger(L,1);
	if (address < 0x0000 || (address + 3) > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	uint32 value = APURAM[address];
	value |= APURAM[address + 1] << 8;
	value |= APURAM[address + 2] << 16;
	value |= APURAM[address + 3] << 24;
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
DEFINE_LUA_FUNCTION(apu_readdwordsigned, "address")
{
	int address = luaL_checkinteger(L,1);
	if (address < 0x0000 || (address + 3) > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	int32 value = APURAM[address];
	value |= APURAM[address + 1] << 8;
	value |= APURAM[address + 2] << 16;
	value |= APURAM[address + 3] << 24;
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}

DEFINE_LUA_FUNCTION(apu_writebyte, "address,value")
{
	int address = lua_tointeger(L,1);
	if (address < 0x0000 || address > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	uint8 value = (uint8)(lua_tointeger(L,2) & 0xFF);
	APURAM[address] = value;
	return 0;
}
DEFINE_LUA_FUNCTION(apu_writeword, "address,value")
{
	int address = lua_tointeger(L,1);
	if (address < 0x0000 || (address + 1) > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	uint16 value = (uint16)(lua_tointeger(L,2) & 0xFFFF);
	APURAM[address] = value & 0xFF;
	APURAM[address + 1] = (value >> 8) & 0xFF;
	return 0;
}
DEFINE_LUA_FUNCTION(apu_writedword, "address,value")
{
	int address = luaL_checkinteger(L,1);
	if (address < 0x0000 || (address + 3) > 0xFFFF)
		luaL_error(L, "address %xh out of range", address);
	uint32 value = (uint32)(luaL_checkinteger(L,2));
	APURAM[address] = value & 0xFF;
	APURAM[address + 1] = (value >> 8) & 0xFF;
	APURAM[address + 2] = (value >> 16) & 0xFF;
	APURAM[address + 3] = (value >> 24) & 0xFF;
	return 0;
}

DEFINE_LUA_FUNCTION(apu_readbyterange, "address,length")
{
	int address = luaL_checkinteger(L,1);
	int length = luaL_checkinteger(L,2);

	if(length < 0)
	{
		address += length;
		length = -length;
	}

	// push the array
	lua_createtable(L, abs(length), 0);

	// put all the values into the (1-based) array
	for(int a = address, n = 1; n <= length; a++, n++)
	{
		if(a >= 0x0000 && a <= 0xFFFF)
		{
			uint8 value = APURAM[a];
			lua_pushinteger(L, value);
			lua_rawseti(L, -2, n);
		}
		// else leave the value nil
	}

	return 1;
}


// resets our "worry" counter of the Lua state
int dontworry(LuaContextInfo& info)
{
	if(info.stopWorrying)
	{
		info.stopWorrying = false;
		if(info.worryCount)
			indicateBusy(info.L, false);
	}
	info.worryCount = 0;
	return 0;
}

static int gcStateData(lua_State *L)
{
	StateData** ppStateData = (StateData**)luaL_checkudata(L, 1, "StateData*");
	if ((*ppStateData)->buffer != NULL)
	{
		free((*ppStateData)->buffer);
	}
	delete (*ppStateData);
	*ppStateData = NULL;
	return 0;
}

static const struct luaL_reg emulib [] =
{
	{"frameadvance", emu_frameadvance},
//	{"speedmode", emu_speedmode},
//	{"wait", emu_wait},
	{"pause", emu_pause},
	{"unpause", emu_unpause},
//	{"emulateframe", emu_emulateframe},
	//{"emulateframefastnoskipping", emu_emulateframefastnoskipping}, // removed from library because probably nobody would notice the difference from emu_emulateframe
//	{"emulateframefast", emu_emulateframefast},
//	{"emulateframeinvisible", emu_emulateframeinvisible},
//	{"redraw", emu_redraw},
	{"framecount", emu_getframecount},
	{"lagcount", emu_getlagcount},
	{"lagged", emu_lagged},
	{"emulating", emu_emulating},
	{"atframeboundary", emu_atframeboundary},
	{"registerbefore", emu_registerbefore},
	{"registerafter", emu_registerafter},
	{"registerstart", emu_registerstart},
	{"registerexit", emu_registerexit},
	{"persistglobalvariables", emu_persistglobalvariables},
	{"message", emu_message},
	{"print", print}, // sure, why not
	{"openscript", emu_openscript},
//	{"loadrom", emu_loadrom},
	// alternative names
//	{"openrom", emu_loadrom},
	{NULL, NULL}
};
static const struct luaL_reg guilib [] =
{
	{"register", gui_register},
	{"text", gui_text},
	{"box", gui_box},
	{"line", gui_line},
	{"pixel", gui_pixel},
	{"getpixel", gui_getpixel},
	{"opacity", gui_setopacity},
	{"transparency", gui_settransparency},
	{"popup", gui_popup},
	{"parsecolor", gui_parsecolor},
	{"gdscreenshot", gui_gdscreenshot},
	{"gdoverlay", gui_gdoverlay},
	{"savescreenshot", gui_savescreenshot},
//	{"redraw", emu_redraw}, // some people might think of this as more of a GUI function
	// alternative names
	{"drawtext", gui_text},
	{"drawbox", gui_box},
	{"drawline", gui_line},
	{"drawpixel", gui_pixel},
	{"setpixel", gui_pixel},
	{"writepixel", gui_pixel},
	{"readpixel", gui_getpixel},
	{"rect", gui_box},
	{"drawrect", gui_box},
	{"drawimage", gui_gdoverlay},
	{"image", gui_gdoverlay},
	{NULL, NULL}
};
static const struct luaL_reg statelib [] =
{
	{"create", state_create},
	{"save", state_save},
	{"load", state_load},
	{"loadscriptdata", state_loadscriptdata},
	{"savescriptdata", state_savescriptdata},
	{"registersave", state_registersave},
	{"registerload", state_registerload},
	{NULL, NULL}
};
static const struct luaL_reg memorylib [] =
{
	{"readbyte", memory_readbyte},
	{"readbytesigned", memory_readbytesigned},
	{"readword", memory_readword},
	{"readwordsigned", memory_readwordsigned},
	{"readdword", memory_readdword},
	{"readdwordsigned", memory_readdwordsigned},
	{"readbyterange", memory_readbyterange},
	{"writebyte", memory_writebyte},
	{"writeword", memory_writeword},
	{"writedword", memory_writedword},
//	{"isvalid", memory_isvalid},
	{"getregister", memory_getregister},
	{"setregister", memory_setregister},
	// alternate naming scheme for word and double-word and unsigned
	{"readbyteunsigned", memory_readbyte},
	{"readwordunsigned", memory_readword},
	{"readdwordunsigned", memory_readdword},
	{"readshort", memory_readword},
	{"readshortunsigned", memory_readword},
	{"readshortsigned", memory_readwordsigned},
	{"readlong", memory_readdword},
	{"readlongunsigned", memory_readdword},
	{"readlongsigned", memory_readdwordsigned},
	{"writeshort", memory_writeword},
	{"writelong", memory_writedword},

	// memory hooks
	{"registerwrite", memory_registerwrite},
	{"registerread", memory_registerread},
	{"registerexec", memory_registerexec},
	// alternate names
	{"register", memory_registerwrite},
	{"registerrun", memory_registerexec},
	{"registerexecute", memory_registerexec},

	{NULL, NULL}
};
static const struct luaL_reg apulib [] =
{
	{"readbyte", apu_readbyte},
	{"readbytesigned", apu_readbytesigned},
	{"readword", apu_readword},
	{"readwordsigned", apu_readwordsigned},
	{"readdword", apu_readdword},
	{"readdwordsigned", apu_readdwordsigned},
	{"readbyterange", apu_readbyterange},
	{"writebyte", apu_writebyte},
	{"writeword", apu_writeword},
	{"writedword", apu_writedword},
	// alternate naming scheme for word and double-word and unsigned
	{"readbyteunsigned", apu_readbyte},
	{"readwordunsigned", apu_readword},
	{"readdwordunsigned", apu_readdword},
	{"readshort", apu_readword},
	{"readshortunsigned", apu_readword},
	{"readshortsigned", apu_readwordsigned},
	{"readlong", apu_readdword},
	{"readlongunsigned", apu_readdword},
	{"readlongsigned", apu_readdwordsigned},
	{"writeshort", apu_writeword},
	{"writelong", apu_writedword},

	{NULL, NULL}
};
static const struct luaL_reg joylib [] =
{
	{"get", joy_get},
	{"getdown", joy_getdown},
	{"getup", joy_getup},
//	{"peek", joy_peek},
//	{"peekdown", joy_peekdown},
//	{"peekup", joy_peekup},
	{"set", joy_set},
	{"gettype", joy_gettype},
	{"settype", joy_settype},
	// alternative names
	{"read", joy_get},
	{"write", joy_set},
	{"readdown", joy_getdown},
	{"readup", joy_getup},
	{NULL, NULL}
};
static const struct luaL_reg inputlib [] =
{
	{"get", input_get},
	{"getdown", input_getdown},
	{"getup", input_getup},
	{"registerhotkey", input_registerhotkey},
	{"popup", input_popup},
	// alternative names
	{"read", input_get},
	{"readdown", input_getdown},
	{"readup", input_getup},
	{NULL, NULL}
};
static const struct luaL_reg movielib [] =
{
	{"active", movie_isactive},
	{"recording", movie_isrecording},
	{"playing", movie_isplaying},
	{"mode", movie_getmode},

	{"length", movie_getlength},
	{"name", movie_getname},
	{"rerecordcount", movie_rerecordcount},
	{"setrerecordcount", movie_setrerecordcount},

	{"rerecordcounting", emu_rerecordcounting},
	{"readonly", movie_getreadonly},
	{"setreadonly", movie_setreadonly},
	{"framecount", emu_getframecount}, // for those familiar with other emulators that have movie.framecount() instead of emulatorname.framecount()

	{"play", movie_play},
	{"replay", movie_replay},
	{"stop", movie_close},

	// alternative names
	{"open", movie_play},
	{"close", movie_close},
	{"getname", movie_getname},
	{"playback", movie_play},
	{"getreadonly", movie_getreadonly},
	{NULL, NULL}
};

static const struct CFuncInfo
{
	const char* library;
	const char* name;
	const char* args;
	bool registry;
}
cFuncInfo [] = // this info is stored here to avoid having to change all of Lua's libraries to use something like DEFINE_LUA_FUNCTION
{
	{LUA_STRLIBNAME, "byte", "str[,start[,end]]"},
	{LUA_STRLIBNAME, "char", "...[bytes]"},
	{LUA_STRLIBNAME, "dump", "func"},
	{LUA_STRLIBNAME, "find", "str,pattern[,init[,plain]]"},
	{LUA_STRLIBNAME, "format", "formatstring,..."},
	{LUA_STRLIBNAME, "gfind", "!deprecated!"},
	{LUA_STRLIBNAME, "gmatch", "str,pattern"},
	{LUA_STRLIBNAME, "gsub", "str,pattern,repl[,n]"},
	{LUA_STRLIBNAME, "len", "str"},
	{LUA_STRLIBNAME, "lower", "str"},
	{LUA_STRLIBNAME, "match", "str,pattern[,init]"},
	{LUA_STRLIBNAME, "rep", "str,n"},
	{LUA_STRLIBNAME, "reverse", "str"},
	{LUA_STRLIBNAME, "sub", "str,start[,end]"},
	{LUA_STRLIBNAME, "upper", "str"},
	{NULL, "module", "name[,...]"},
	{NULL, "require", "modname"},
	{LUA_LOADLIBNAME, "loadlib", "libname,funcname"},
	{LUA_LOADLIBNAME, "seeall", "module"},
	{LUA_COLIBNAME, "create", "func"},
	{LUA_COLIBNAME, "resume", "co[,val1,...]"},
	{LUA_COLIBNAME, "running", ""},
	{LUA_COLIBNAME, "status", "co"},
	{LUA_COLIBNAME, "wrap", "func"},
	{LUA_COLIBNAME, "yield", "..."},
	{NULL, "assert", "cond[,message]"},
	{NULL, "collectgarbage", "opt[,arg]"},
	{NULL, "gcinfo", ""},
	{NULL, "dofile", "filename"},
	{NULL, "error", "message[,level]"},
	{NULL, "getfenv", "[level_or_func]"},
	{NULL, "getmetatable", "object"},
	{NULL, "ipairs", "arraytable"},
	{NULL, "load", "func[,chunkname]"},
	{NULL, "loadfile", "[filename]"},
	{NULL, "loadstring", "str[,chunkname]"},
	{NULL, "next", "table[,index]"},
	{NULL, "pairs", "table"},
	{NULL, "pcall", "func,arg1,..."},
	{NULL, "rawequal", "v1,v2"},
	{NULL, "rawget", "table,index"},
	{NULL, "rawset", "table,index,value"},
	{NULL, "select", "index,..."},
	{NULL, "setfenv", "level_or_func,envtable"},
	{NULL, "setmetatable", "table,metatable"},
	{NULL, "tonumber", "str_or_num[,base]"},
	{NULL, "type", "obj"},
	{NULL, "unpack", "list[,i=1[,j=#list]]"},
	{NULL, "xpcall", "func,errhandler"},
	{NULL, "newproxy", "hasmeta"},
	{LUA_MATHLIBNAME, "abs", "x"},
	{LUA_MATHLIBNAME, "acos", "x"},
	{LUA_MATHLIBNAME, "asin", "x"},
	{LUA_MATHLIBNAME, "atan", "x"},
	{LUA_MATHLIBNAME, "atan2", "y,x"},
	{LUA_MATHLIBNAME, "ceil", "x"},
	{LUA_MATHLIBNAME, "cos", "rads"},
	{LUA_MATHLIBNAME, "cosh", "x"},
	{LUA_MATHLIBNAME, "deg", "rads"},
	{LUA_MATHLIBNAME, "exp", "x"},
	{LUA_MATHLIBNAME, "floor", "x"},
	{LUA_MATHLIBNAME, "fmod", "x,y"},
	{LUA_MATHLIBNAME, "frexp", "x"},
	{LUA_MATHLIBNAME, "ldexp", "m,e"},
	{LUA_MATHLIBNAME, "log", "x"},
	{LUA_MATHLIBNAME, "log10", "x"},
	{LUA_MATHLIBNAME, "max", "x,..."},
	{LUA_MATHLIBNAME, "min", "x,..."},
	{LUA_MATHLIBNAME, "modf", "x"},
	{LUA_MATHLIBNAME, "pow", "x,y"},
	{LUA_MATHLIBNAME, "rad", "degs"},
	{LUA_MATHLIBNAME, "random", "[m[,n]]"},
	{LUA_MATHLIBNAME, "randomseed", "x"},
	{LUA_MATHLIBNAME, "sin", "rads"},
	{LUA_MATHLIBNAME, "sinh", "x"},
	{LUA_MATHLIBNAME, "sqrt", "x"},
	{LUA_MATHLIBNAME, "tan", "rads"},
	{LUA_MATHLIBNAME, "tanh", "x"},
	{LUA_IOLIBNAME, "close", "[file]"},
	{LUA_IOLIBNAME, "flush", ""},
	{LUA_IOLIBNAME, "input", "[file]"},
	{LUA_IOLIBNAME, "lines", "[filename]"},
	{LUA_IOLIBNAME, "open", "filename[,mode=\"r\"]"},
	{LUA_IOLIBNAME, "output", "[file]"},
	{LUA_IOLIBNAME, "popen", "prog,[model]"},
	{LUA_IOLIBNAME, "read", "..."},
	{LUA_IOLIBNAME, "tmpfile", ""},
	{LUA_IOLIBNAME, "type", "obj"},
	{LUA_IOLIBNAME, "write", "..."},
	{LUA_OSLIBNAME, "clock", ""},
	{LUA_OSLIBNAME, "date", "[format[,time]]"},
	{LUA_OSLIBNAME, "difftime", "t2,t1"},
	{LUA_OSLIBNAME, "execute", "[command]"},
	{LUA_OSLIBNAME, "exit", "[code]"},
	{LUA_OSLIBNAME, "getenv", "varname"},
	{LUA_OSLIBNAME, "remove", "filename"},
	{LUA_OSLIBNAME, "rename", "oldname,newname"},
	{LUA_OSLIBNAME, "setlocale", "locale[,category]"},
	{LUA_OSLIBNAME, "time", "[timetable]"},
	{LUA_OSLIBNAME, "tmpname", ""},
	{LUA_DBLIBNAME, "debug", ""},
	{LUA_DBLIBNAME, "getfenv", "o"},
	{LUA_DBLIBNAME, "gethook", "[thread]"},
	{LUA_DBLIBNAME, "getinfo", "[thread,]function[,what]"},
	{LUA_DBLIBNAME, "getlocal", "[thread,]level,local"},
	{LUA_DBLIBNAME, "getmetatable", "[object]"},
	{LUA_DBLIBNAME, "getregistry", ""},
	{LUA_DBLIBNAME, "getupvalue", "func,up"},
	{LUA_DBLIBNAME, "setfenv", "object,table"},
	{LUA_DBLIBNAME, "sethook", "[thread,]hook,mask[,count]"},
	{LUA_DBLIBNAME, "setlocal", "[thread,]level,local,value"},
	{LUA_DBLIBNAME, "setmetatable", "object,table"},
	{LUA_DBLIBNAME, "setupvalue", "func,up,value"},
	{LUA_DBLIBNAME, "traceback", "[thread,][message][,level]"},
	{LUA_TABLIBNAME, "concat", "table[,sep[,i[,j]]]"},
	{LUA_TABLIBNAME, "insert", "table,[pos,]value"},
	{LUA_TABLIBNAME, "maxn", "table"},
	{LUA_TABLIBNAME, "remove", "table[,pos]"},
	{LUA_TABLIBNAME, "sort", "table[,comp]"},
	{LUA_TABLIBNAME, "foreach", "table,func"},
	{LUA_TABLIBNAME, "foreachi", "table,func"},
	{LUA_TABLIBNAME, "getn", "table"},
	{LUA_TABLIBNAME, "maxn", "table"},
	{LUA_TABLIBNAME, "setn", "table,value"}, // I know some of these are obsolete but they should still have argument info if they're exposed to the user
	{LUA_FILEHANDLE, "setvbuf", "mode[,size]", true},
	{LUA_FILEHANDLE, "lines", "", true},
	{LUA_FILEHANDLE, "read", "...", true},
	{LUA_FILEHANDLE, "flush", "", true},
	{LUA_FILEHANDLE, "seek", "[whence][,offset]", true},
	{LUA_FILEHANDLE, "write", "...", true},
	{LUA_FILEHANDLE, "__tostring", "obj", true},
	{LUA_FILEHANDLE, "__gc", "", true},
	{"_LOADLIB", "__gc", "", true},
};

void registerLibs(lua_State* L)
{
	luaL_openlibs(L);

	luaL_register(L, "emu", emulib);
	luaL_register(L, "gui", guilib);
	//luaL_register(L, "stylus", styluslib);
	luaL_register(L, "savestate", statelib);
	luaL_register(L, "memory", memorylib);
	luaL_register(L, "apu", apulib);
	luaL_register(L, "joypad", joylib); // for game input
	luaL_register(L, "input", inputlib); // for user input
	luaL_register(L, "movie", movielib);
	luaL_register(L, "bit", bit_funcs); // LuaBitOp library

	lua_settop(L, 0); // clean the stack, because each call to luaL_register leaves a table on top
	
	// register a few utility functions outside of libraries (in the global namespace)
	lua_register(L, "print", print);
	lua_register(L, "tostring", tostring);
	lua_register(L, "addressof", addressof);
	lua_register(L, "copytable", copytable);
	
	// old bit operation functions
	lua_register(L, "AND", bit_band);
	lua_register(L, "OR", bit_bor);
	lua_register(L, "XOR", bit_bxor);
	lua_register(L, "SHIFT", bitshift);
	lua_register(L, "BIT", bitbit);

	luabitop_validate(L);

	// populate s_cFuncInfoMap the first time
	static bool once = true;
	if(once)
	{
		once = false;

		for(int i = 0; i < sizeof(cFuncInfo)/sizeof(*cFuncInfo); i++)
		{
			const CFuncInfo& cfi = cFuncInfo[i];
			if(cfi.registry)
			{
				lua_getregistry(L);
				lua_getfield(L, -1, cfi.library);
				lua_remove(L, -2);
				lua_getfield(L, -1, cfi.name);
				lua_remove(L, -2);
			}
			else if(cfi.library)
			{
				lua_getfield(L, LUA_GLOBALSINDEX, cfi.library);
				lua_getfield(L, -1, cfi.name);
				lua_remove(L, -2);
			}
			else
			{
				lua_getfield(L, LUA_GLOBALSINDEX, cfi.name);
			}

			lua_CFunction func = lua_tocfunction(L, -1);
			s_cFuncInfoMap[func] = cfi.args;
			lua_pop(L, 1);
		}

		// deal with some stragglers
		lua_getfield(L, LUA_GLOBALSINDEX, "package");
		lua_getfield(L, -1, "loaders");
		lua_remove(L, -2);
		if(lua_istable(L, -1))
		{
			for(int i=1;;i++)
			{
				lua_rawgeti(L, -1, i);
				lua_CFunction func = lua_tocfunction(L, -1);
				lua_pop(L,1);
				if(!func)
					break;
				s_cFuncInfoMap[func] = "name";
			}
		}
		lua_pop(L,1);
	}

	// push arrays for storing hook functions in
	for(int i = 0; i < LUAMEMHOOK_COUNT; i++)
	{
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, luaMemHookTypeStrings[i]);
	}

	// register type
	luaL_newmetatable(L, "StateData*");
	lua_pushcfunction(L, gcStateData);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
}

void ResetInfo(LuaContextInfo& info)
{
	info.L = NULL;
	info.started = false;
	info.running = false;
	info.returned = false;
	info.crashed = false;
	info.restart = false;
	info.restartLater = false;
	info.worryCount = 0;
	info.stopWorrying = false;
	info.panic = false;
	info.ranExit = false;
	info.ranFrameAdvance = false;
	info.transparencyModifier = 255;
	info.speedMode = SPEEDMODE_NORMAL;
	info.guiFuncsNeedDeferring = false;
	info.dataSaveKey = 0;
	info.dataLoadKey = 0;
	info.dataSaveLoadKeySet = false;
	info.rerecordCountingDisabled = false;
	info.numMemHooks = 0;
	info.persistVars.clear();
	info.newDefaultData.ClearRecords();
	info.guiData.data = luaGuiDataBuf;
	info.guiData.stridePix = SNES_WIDTH;
	info.guiData.xMin = 0;
	info.guiData.xMax = SNES_WIDTH;
	info.guiData.yMin = 0;
	info.guiData.yMax = SNES_HEIGHT_EXTENDED;
	info.guiData.xOrigin = 0;
	info.guiData.yOrigin = 0;
	ClearLuaGui();
}

void OpenLuaContext(int uid, void(*print)(int uid, const char* str), void(*onstart)(int uid), void(*onstop)(int uid, bool statusOK))
{
	LuaContextInfo* newInfo = new LuaContextInfo();
	ResetInfo(*newInfo);
	newInfo->print = print;
	newInfo->onstart = onstart;
	newInfo->onstop = onstop;
	luaContextInfo[uid] = newInfo;
}

void RunLuaScriptFile(int uid, const char* filenameCStr)
{
	if(luaContextInfo.find(uid) == luaContextInfo.end())
		return;
	StopLuaScript(uid);

	LuaContextInfo& info = *luaContextInfo[uid];

#ifdef USE_INFO_STACK
	infoStack.insert(infoStack.begin(), &info);
	struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope; // doing it like this makes sure that the info stack gets cleaned up even if an exception is thrown
#endif

	info.nextFilename = filenameCStr;

	// TODO: store script's current directory into LuaContextInfo
	static char dirnameCStr[MAX_PATH];
	strcpy(dirnameCStr, filenameCStr);
	TrimFilenameFromPath(dirnameCStr);
	_chdir(dirnameCStr);

	if(info.running)
	{
		// it's a little complicated, but... the call to luaL_dofile below
		// could call a C function that calls this very function again
		// additionally, if that happened then the above call to StopLuaScript
		// probably couldn't stop the script yet, so instead of continuing,
		// we'll set a flag that tells the first call of this function to loop again
		// when the script is able to stop safely
		info.restart = true;
		return;
	}

	do
	{
		std::string filename = info.nextFilename;

		lua_State* L = lua_open();
#ifndef USE_INFO_STACK
		luaStateToContextMap[L] = &info;
#endif
		luaStateToUIDMap[L] = uid;
		ResetInfo(info);
		info.L = L;
		info.guiFuncsNeedDeferring = true;
		info.lastFilename = filename;

		SetSaveKey(info, FilenameFromPath(filename.c_str()));
		info.dataSaveLoadKeySet = false;

		registerLibs(L);

		// register a function to periodically check for inactivity
		lua_sethook(L, LuaRescueHook, LUA_MASKCOUNT, HOOKCOUNT);

		// deferred evaluation table
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, deferredGUIIDString);
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, deferredJoySetIDString);

		info.started = true;
		RefreshScriptStartedStatus();
		if(info.onstart)
			info.onstart(uid);
		info.running = true;
		RefreshScriptSpeedStatus();
		info.returned = false;
		int errorcode = luaL_dofile(L,filename.c_str());
		info.running = false;
		RefreshScriptSpeedStatus();
		info.returned = true;

		if (errorcode)
		{
			info.crashed = true;
			if(info.print)
			{
				info.print(uid, lua_tostring(L,-1));
				info.print(uid, "\r\n");
			}
			else
			{
				fprintf(stderr, "%s\n", lua_tostring(L,-1));
			}
			StopLuaScript(uid);
		}
		else
		{
//			Show_Genesis_Screen();
			StopScriptIfFinished(uid, true);
		}
	} while(info.restart);
}

void StopScriptIfFinished(int uid, bool justReturned)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	if(!info.returned)
		return;

	// the script has returned, but it is not necessarily done running
	// because it may have registered a function that it expects to keep getting called
	// so check if it has any registered functions and stop the script only if it doesn't

	bool keepAlive = (info.numMemHooks != 0);
	for(int calltype = 0; calltype < LUACALL_COUNT && !keepAlive; calltype++)
	{
		lua_State* L = info.L;
		if(L)
		{
			const char* idstring = luaCallIDStrings[calltype];
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			bool isFunction = lua_isfunction(L, -1);
			lua_pop(L, 1);

			if(isFunction)
				keepAlive = true;
		}
	}

	if(keepAlive)
	{
		if(justReturned)
		{
			if(info.print)
				info.print(uid, "script returned but is still running registered functions\r\n");
			else
				fprintf(stderr, "%s\n", "script returned but is still running registered functions");
		}
	}
	else
	{
		if(info.print)
			info.print(uid, "script finished running\r\n");
		else
			fprintf(stderr, "%s\n", "script finished running");

		StopLuaScript(uid);
	}
}

void RequestAbortLuaScript(int uid, const char* message)
{
	if(luaContextInfo.find(uid) == luaContextInfo.end())
		return;
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;
	if(L)
	{
		// this probably isn't the right way to do it
		// but calling luaL_error here is positively unsafe
		// (it seemingly works fine but sometimes corrupts the emulation state in colorful ways)
		// and this works pretty well and is definitely safe, so screw it
		info.L->hookcount = 1; // run hook function as soon as possible
		info.panic = true; // and call luaL_error once we're inside the hook function
		if(message)
		{
			strncpy(info.panicMessage, message, sizeof(info.panicMessage));
			info.panicMessage[sizeof(info.panicMessage)-1] = 0;
		}
		else
		{
			// attach file/line info because this is the case where it's most necessary to see that,
			// and often it won't be possible for the later luaL_error call to retrieve it otherwise.
			// this means sometimes printing multiple file/line numbers if luaL_error does find something,
			// but that's fine since more information is probably better anyway.
			luaL_where(L,0); // should be 0 and not 1 here to get useful (on force stop) messages
			const char* whereString = lua_tostring(L,-1);
			snprintf(info.panicMessage, sizeof(info.panicMessage), "%sscript terminated", whereString);
			lua_pop(L,1);
		}
	}
}

void SetSaveKey(LuaContextInfo& info, const char* key)
{
	info.dataSaveKey = crc32(0, (const unsigned char*)key, strlen(key));

	if(!info.dataSaveLoadKeySet)
	{
		info.dataLoadKey = info.dataSaveKey;
		info.dataSaveLoadKeySet = true;
	}
}
void SetLoadKey(LuaContextInfo& info, const char* key)
{
	info.dataLoadKey = crc32(0, (const unsigned char*)key, strlen(key));

	if(!info.dataSaveLoadKeySet)
	{
		info.dataSaveKey = info.dataLoadKey;
		info.dataSaveLoadKeySet = true;
	}
}

void HandleCallbackError(lua_State* L, LuaContextInfo& info, int uid, bool stopScript)
{
	info.crashed = true;
	if(L->errfunc || L->errorJmp)
		luaL_error(L, lua_tostring(L,-1));
	else
	{
		if(info.print)
		{
			info.print(uid, lua_tostring(L,-1));
			info.print(uid, "\r\n");
		}
		else
		{
			fprintf(stderr, "%s\n", lua_tostring(L,-1));
		}
		if(stopScript)
			StopLuaScript(uid);
	}
}

void CallExitFunction(int uid)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;

	if(!L)
		return;

	dontworry(info);

	// first call the registered exit function if there is one
	if(!info.ranExit)
	{
		info.ranExit = true;

#ifdef USE_INFO_STACK
		infoStack.insert(infoStack.begin(), &info);
		struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif

		lua_settop(L, 0);
		lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
		
		int errorcode = 0;
		if (lua_isfunction(L, -1))
		{
			bool wasRunning = info.running;
			info.running = true;
			RefreshScriptSpeedStatus();

			bool wasPanic = info.panic;
			info.panic = false; // otherwise we could barely do anything in the exit function

			errorcode = lua_pcall(L, 0, 0, 0);

			info.panic |= wasPanic; // restore panic

			info.running = wasRunning;
			RefreshScriptSpeedStatus();
		}

		// save persisted variable info after the exit function runs (even if it crashed)
		{
			// gather the final value of the variables we're supposed to persist
			LuaSaveData newExitData;
			{
				int numPersistVars = info.persistVars.size();
				for(int i = 0; i < numPersistVars; i++)
				{
					const char* varName = info.persistVars[i].c_str();
					lua_getfield(L, LUA_GLOBALSINDEX, varName);
					int type = lua_type(L,-1);
					unsigned int varNameCRC = crc32(0, (const unsigned char*)varName, strlen(varName));
					newExitData.SaveRecordPartial(uid, varNameCRC, -1);
					lua_pop(L,1);
				}
			}

			char path [1024] = {0};
			char* pathTypeChrPtr = ConstructScriptSaveDataPath(path, 1024, info);

			*pathTypeChrPtr = 'd';
			if(info.newDefaultData.recordList)
			{
				FILE* defaultsFile = fopen(path, "wb");
				if(defaultsFile)
				{
					info.newDefaultData.ExportRecords(defaultsFile);
					fclose(defaultsFile);
				}
			}
			else unlink(path);

			*pathTypeChrPtr = 'e';
			if(newExitData.recordList)
			{
				FILE* persistFile = fopen(path, "wb");
				if(persistFile)
				{
					newExitData.ExportRecords(persistFile);
					fclose(persistFile);
				}
			}
			else unlink(path);
		}

		if (errorcode)
			HandleCallbackError(L,info,uid,false);

	}
}

void StopLuaScript(int uid)
{
	LuaContextInfo* infoPtr = luaContextInfo[uid];
	if(!infoPtr)
		return;

	LuaContextInfo& info = *infoPtr;

	if(info.running)
	{
		// if it's currently running then we can't stop it now without crashing
		// so the best we can do is politely request for it to go kill itself
		RequestAbortLuaScript(uid);
		return;
	}

	lua_State* L = info.L;
	if(L)
	{
		CallExitFunction(uid);

		if(info.onstop)
		{
			info.stopWorrying = true, info.worryCount++, dontworry(info); // clear "busy" status
			info.onstop(uid, !info.crashed); // must happen before closing L and after the exit function, otherwise the final GUI state of the script won't be shown properly or at all
		}

		if(info.started) // this check is necessary
		{
			lua_close(L);
#ifndef USE_INFO_STACK
			luaStateToContextMap.erase(L);
#endif
			luaStateToUIDMap.erase(L);
			info.L = NULL;
			info.started = false;
			
			info.numMemHooks = 0;
			for(int i = 0; i < LUAMEMHOOK_COUNT; i++)
				CalculateMemHookRegions((LuaMemHookType)i);
		}
		RefreshScriptStartedStatus();
	}
}

void CloseLuaContext(int uid)
{
	StopLuaScript(uid);
	delete luaContextInfo[uid];
	luaContextInfo.erase(uid);
}


// the purpose of this structure is to provide a way of
// QUICKLY determining whether a memory address range has a hook associated with it,
// with a bias toward fast rejection because the majority of addresses will not be hooked.
// (it must not use any part of Lua or perform any per-script operations,
//  otherwise it would definitely be too slow.)
// calculating the regions when a hook is added/removed may be slow,
// but this is an intentional tradeoff to obtain a high speed of checking during later execution
struct TieredRegion
{
	template<unsigned int maxGap>
	struct Region
	{
		struct Island
		{
			unsigned int start;
			unsigned int end;
			__forceinline bool Contains(unsigned int address, int size) const { return address < end && address+size > start; }
		};
		std::vector<Island> islands;

		void Calculate(const std::vector<unsigned int>& bytes)
		{
			islands.clear();

			unsigned int lastEnd = ~0;

			std::vector<unsigned int>::const_iterator iter = bytes.begin();
			std::vector<unsigned int>::const_iterator end = bytes.end();
			for(; iter != end; ++iter)
			{
				unsigned int addr = *iter;
				if(addr < lastEnd || addr > lastEnd + (long long)maxGap)
				{
					islands.push_back(Island());
					islands.back().start = addr;
				}
				islands.back().end = addr+1;
				lastEnd = addr+1;
			}
		}

		bool Contains(unsigned int address, int size) const
		{
			/*
			std::vector<Island>::const_iterator iter = islands.begin();
			std::vector<Island>::const_iterator end = islands.end();
			for(; iter != end; ++iter)
				if(iter->Contains(address, size))
					return true;
			*/
			return false;
		}
	};

	Region<0xFFFFFFFF> broad;
	Region<0x1000> mid;
	Region<0> narrow;

	void Calculate(std::vector<unsigned int>& bytes)
	{
		std::sort(bytes.begin(), bytes.end());

		broad.Calculate(bytes);
		mid.Calculate(bytes);
		narrow.Calculate(bytes);
	}

	TieredRegion()
	{
		//Calculate(std::vector<unsigned int>());
	}

	__forceinline int NotEmpty()
	{
		return broad.islands.size();
	}

	// note: it is illegal to call this if NotEmpty() returns 0
	__forceinline bool Contains(unsigned int address, int size)
	{
		return broad.islands[0].Contains(address,size) &&
		       mid.Contains(address,size) &&
			   narrow.Contains(address,size);
	}
};
TieredRegion hookedRegions [LUAMEMHOOK_COUNT];


static void CalculateMemHookRegions(LuaMemHookType hookType)
{
	std::vector<unsigned int> hookedBytes;
	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		LuaContextInfo& info = *iter->second;
		if(info.numMemHooks)
		{
			lua_State* L = info.L;
			if(L)
			{
				lua_settop(L, 0);
				lua_getfield(L, LUA_REGISTRYINDEX, luaMemHookTypeStrings[hookType]);
				lua_pushnil(L);
				while(lua_next(L, -2))
				{
					if(lua_isfunction(L, -1))
					{
						unsigned int addr = lua_tointeger(L, -2);
						hookedBytes.push_back(addr);
					}
					lua_pop(L, 1);
				}
				lua_settop(L, 0);
			}
		}
		++iter;
	}
	hookedRegions[hookType].Calculate(hookedBytes);
}





static void CallRegisteredLuaMemHook_LuaMatch(unsigned int address, int size, unsigned int value, LuaMemHookType hookType)
{
	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		LuaContextInfo& info = *iter->second;
		if(info.numMemHooks)
		{
			lua_State* L = info.L;
			if(L && !info.panic)
			{
#ifdef USE_INFO_STACK
				infoStack.insert(infoStack.begin(), &info);
				struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif
				lua_settop(L, 0);
				lua_getfield(L, LUA_REGISTRYINDEX, luaMemHookTypeStrings[hookType]);
				for(int i = address; i != address+size; i++)
				{
					lua_rawgeti(L, -1, i);
					if (lua_isfunction(L, -1))
					{
						bool wasRunning = info.running;
						info.running = true;
						RefreshScriptSpeedStatus();
						lua_pushinteger(L, address);
						lua_pushinteger(L, size);
						int errorcode = lua_pcall(L, 2, 0, 0);
						info.running = wasRunning;
						RefreshScriptSpeedStatus();
						if (errorcode)
						{
							int uid = iter->first;
							HandleCallbackError(L,info,uid,true);
						}
						break;
					}
					else
					{
						lua_pop(L,1);
					}
				}
				lua_settop(L, 0);
			}
		}
		++iter;
	}
}
void CallRegisteredLuaMemHook(unsigned int address, int size, unsigned int value, LuaMemHookType hookType)
{
	// performance critical! (called VERY frequently)
	// I suggest timing a large number of calls to this function in Release if you change anything in here,
	// before and after, because even the most innocent change can make it become 30% to 400% slower.
	// a good amount to test is: 100000000 calls with no hook set, and another 100000000 with a hook set.
	// (on my system that consistently took 200 ms total in the former case and 350 ms total in the latter case)
	if(hookedRegions[hookType].NotEmpty())
	{
		// TODO: add more mirroring
		if(address >= 0x0000 && address <= 0x1FFF)
			address |= 0x7E0000; // account for mirroring of LowRAM

		if(hookedRegions[hookType].Contains(address, size))
			CallRegisteredLuaMemHook_LuaMatch(address, size, value, hookType); // something has hooked this specific address
	}
}



void CallRegisteredLuaFunctions(LuaCallID calltype)
{
	assert((unsigned int)calltype < (unsigned int)LUACALL_COUNT);
	const char* idstring = luaCallIDStrings[calltype];

	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		lua_State* L = info.L;
		if(L && (!info.panic || calltype == LUACALL_BEFOREEXIT))
		{
#ifdef USE_INFO_STACK
			infoStack.insert(infoStack.begin(), &info);
			struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif

			// handle deferred GUI function calls and disabling deferring when unnecessary
			if(calltype == LUACALL_AFTEREMULATIONGUI || calltype == LUACALL_AFTEREMULATION)
				info.guiFuncsNeedDeferring = false;
			if(calltype == LUACALL_AFTEREMULATIONGUI) {
				ClearLuaGui();
				CallDeferredFunctions(L, deferredGUIIDString);
			}
			if(calltype == LUACALL_BEFOREEMULATION)
				CallDeferredFunctions(L, deferredJoySetIDString);

			int top = lua_gettop(L);
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			
			if (lua_isfunction(L, -1))
			{
				bool wasRunning = info.running;
				info.running = true;
				RefreshScriptSpeedStatus();
				int errorcode = lua_pcall(L, 0, 0, 0);
				info.running = wasRunning;
				RefreshScriptSpeedStatus();
				if (errorcode)
					HandleCallbackError(L,info,uid,true);
			}
			else
			{
				lua_pop(L, 1);
			}

			info.guiFuncsNeedDeferring = true;
			if(!info.crashed)
			{
				lua_settop(L, top);
				if(!info.panic)
					dontworry(info);
			}
		}

		++iter;
	}
}

void CallRegisteredLuaSaveFunctions(int savestateNumber, LuaSaveData& saveData)
{
	const char* idstring = luaCallIDStrings[LUACALL_BEFORESAVE];

	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		lua_State* L = info.L;
		if(L)
		{
#ifdef USE_INFO_STACK
			infoStack.insert(infoStack.begin(), &info);
			struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif

			lua_settop(L, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			
			if (lua_isfunction(L, -1))
			{
				bool wasRunning = info.running;
				info.running = true;
				RefreshScriptSpeedStatus();
				lua_pushinteger(L, savestateNumber);
				int errorcode = lua_pcall(L, 1, LUA_MULTRET, 0);
				info.running = wasRunning;
				RefreshScriptSpeedStatus();
				if (errorcode)
					HandleCallbackError(L,info,uid,true);
				saveData.SaveRecord(uid, info.dataSaveKey);
			}
			else
			{
				lua_pop(L, 1);
			}
		}

		++iter;
	}
}


void CallRegisteredLuaLoadFunctions(int savestateNumber, const LuaSaveData& saveData)
{
	const char* idstring = luaCallIDStrings[LUACALL_AFTERLOAD];

	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		lua_State* L = info.L;
		if(L)
		{
#ifdef USE_INFO_STACK
			infoStack.insert(infoStack.begin(), &info);
			struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif

			lua_settop(L, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			
			if (lua_isfunction(L, -1))
			{
				bool wasRunning = info.running;
				info.running = true;
				RefreshScriptSpeedStatus();

				// since the scriptdata can be very expensive to load
				// (e.g. the registered save function returned some huge tables)
				// check the number of parameters the registered load function expects
				// and don't bother loading the parameters it wouldn't receive anyway
				int numParamsExpected = (L->top - 1)->value.gc->cl.l.p->numparams;
				if(numParamsExpected) numParamsExpected--; // minus one for the savestate number we always pass in

				int prevGarbage = lua_gc(L, LUA_GCCOUNT, 0);

				lua_pushinteger(L, savestateNumber);
				saveData.LoadRecord(uid, info.dataLoadKey, numParamsExpected);
				int n = lua_gettop(L) - 1;

				int errorcode = lua_pcall(L, n, 0, 0);
				info.running = wasRunning;
				RefreshScriptSpeedStatus();
				if (errorcode)
					HandleCallbackError(L,info,uid,true);
				else
				{
					int newGarbage = lua_gc(L, LUA_GCCOUNT, 0);
					if(newGarbage - prevGarbage > 50)
					{
						// now seems to be a very good time to run the garbage collector
						// it might take a while now but that's better than taking 10 whiles 9 loads from now
						lua_gc(L, LUA_GCCOLLECT, 0);
					}
				}
			}
			else
			{
				lua_pop(L, 1);
			}
		}

		++iter;
	}
}

static const unsigned char* s_dbg_dataStart = NULL;
static int s_dbg_dataSize = 0;


// can't remember what the best way of doing this is...
#if defined(i386) || defined(__i386) || defined(__i386__) || defined(M_I86) || defined(_M_IX86) || defined(__WIN32__)
	#define IS_LITTLE_ENDIAN
#endif

// push a value's bytes onto the output stack
template<typename T>
void PushBinaryItem(T item, std::vector<unsigned char>& output)
{
	unsigned char* buf = (unsigned char*)&item;
#ifdef IS_LITTLE_ENDIAN
	for(int i = sizeof(T); i; i--)
		output.push_back(*buf++);
#else
	int vecsize = output.size();
	for(int i = sizeof(T); i; i--)
		output.insert(output.begin() + vecsize, *buf++);
#endif
}
// read a value from the byte stream and advance the stream by its size
template<typename T>
T AdvanceByteStream(const unsigned char*& data, unsigned int& remaining)
{
#ifdef IS_LITTLE_ENDIAN
	T rv = *(T*)data;
	data += sizeof(T);
#else
	T rv; unsigned char* rvptr = (unsigned char*)&rv;
	for(int i = sizeof(T)-1; i>=0; i--)
		rvptr[i] = *data++;
#endif
	remaining -= sizeof(T);
	return rv;
}
// advance the byte stream by a certain size without reading a value
void AdvanceByteStream(const unsigned char*& data, unsigned int& remaining, int amount)
{
	data += amount;
	remaining -= amount;
}

#define LUAEXT_TLONG		30 // 0x1E // 4-byte signed integer
#define LUAEXT_TUSHORT		31 // 0x1F // 2-byte unsigned integer
#define LUAEXT_TSHORT		32 // 0x20 // 2-byte signed integer
#define LUAEXT_TBYTE		33 // 0x21 // 1-byte unsigned integer
#define LUAEXT_TNILS		34 // 0x22 // multiple nils represented by a 4-byte integer (warning: becomes multiple stack entities)
#define LUAEXT_TTABLE		0x40 // 0x40 through 0x4F // tables of different sizes:
#define LUAEXT_BITS_1A		0x01 // size of array part fits in a 1-byte unsigned integer
#define LUAEXT_BITS_2A		0x02 // size of array part fits in a 2-byte unsigned integer
#define LUAEXT_BITS_4A		0x03 // size of array part fits in a 4-byte unsigned integer
#define LUAEXT_BITS_1H		0x04 // size of hash part fits in a 1-byte unsigned integer
#define LUAEXT_BITS_2H		0x08 // size of hash part fits in a 2-byte unsigned integer
#define LUAEXT_BITS_4H		0x0C // size of hash part fits in a 4-byte unsigned integer
#define BITMATCH(x,y) (((x) & (y)) == (y))

static void PushNils(std::vector<unsigned char>& output, int& nilcount)
{
	int count = nilcount;
	nilcount = 0;

	static const int minNilsWorthEncoding = 6; // because a LUAEXT_TNILS entry is 5 bytes

	if(count < minNilsWorthEncoding)
	{
		for(int i = 0; i < count; i++)
			output.push_back(LUA_TNIL);
	}
	else
	{
		output.push_back(LUAEXT_TNILS);
		PushBinaryItem<UINT32>(count, output);
	}
}


static void LuaStackToBinaryConverter(lua_State* L, int i, std::vector<unsigned char>& output)
{
	int type = lua_type(L, i);

	// the first byte of every serialized item says what Lua type it is
	output.push_back(type & 0xFF);

	switch(type)
	{
		default:
			{
				//printf("wrote unknown type %d (0x%x)\n", type, type);	
				//assert(0);

				LuaContextInfo& info = GetCurrentInfo();
				if(info.print)
				{
					char errmsg [1024];
					sprintf(errmsg, "values of type \"%s\" are not allowed to be returned from registered save functions.\r\n", luaL_typename(L,i));
					info.print(luaStateToUIDMap[L], errmsg);
				}
				else
				{
					fprintf(stderr, "values of type \"%s\" are not allowed to be returned from registered save functions.\n", luaL_typename(L,i));
				}
			}
			break;
		case LUA_TNIL:
			// no information necessary beyond the type
			break;
		case LUA_TBOOLEAN:
			// serialize as 0 or 1
			output.push_back(lua_toboolean(L,i));
			break;
		case LUA_TSTRING:
			// serialize as a 0-terminated string of characters
			{
				const char* str = lua_tostring(L,i);
				while(*str)
					output.push_back(*str++);
				output.push_back('\0');
			}
			break;
		case LUA_TNUMBER:
			{
				double num = (double)lua_tonumber(L,i);
				INT32 inum = (INT32)lua_tointeger(L,i);
				if(num != inum)
				{
					PushBinaryItem(num, output);
				}
				else
				{
					if((inum & ~0xFF) == 0)
						type = LUAEXT_TBYTE;
					else if((UINT16)(inum & 0xFFFF) == inum)
						type = LUAEXT_TUSHORT;
					else if((INT16)(inum & 0xFFFF) == inum)
						type = LUAEXT_TSHORT;
					else
						type = LUAEXT_TLONG;
					output.back() = type;
					switch(type)
					{
					case LUAEXT_TLONG:
						PushBinaryItem<INT32>(inum, output);
						break;
					case LUAEXT_TUSHORT:
						PushBinaryItem<UINT16>(inum, output);
						break;
					case LUAEXT_TSHORT:
						PushBinaryItem<INT16>(inum, output);
						break;
					case LUAEXT_TBYTE:
						output.push_back(inum);
						break;
					}
				}
			}
			break;
		case LUA_TTABLE:
			// serialize as a type that describes how many bytes are used for storing the counts,
			// followed by the number of array entries if any, then the number of hash entries if any,
			// then a Lua value per array entry, then a (key,value) pair of Lua values per hashed entry
			// note that the structure of table references are not faithfully serialized (yet)
		{
			int outputTypeIndex = output.size() - 1;
			int arraySize = 0;
			int hashSize = 0;

			if(lua_checkstack(L, 4) && std::find(s_tableAddressStack.begin(), s_tableAddressStack.end(), lua_topointer(L,i)) == s_tableAddressStack.end())
			{
				s_tableAddressStack.push_back(lua_topointer(L,i));
				struct Scope { ~Scope(){ s_tableAddressStack.pop_back(); } } scope;

				bool wasnil = false;
				int nilcount = 0;
				arraySize = lua_objlen(L, i);
				int arrayValIndex = lua_gettop(L) + 1;
				for(int j = 1; j <= arraySize; j++)
				{
			        lua_rawgeti(L, i, j);
					bool isnil = lua_isnil(L, arrayValIndex);
					if(isnil)
						nilcount++;
					else
					{
						if(wasnil)
							PushNils(output, nilcount);
						LuaStackToBinaryConverter(L, arrayValIndex, output);
					}
					lua_pop(L, 1);
					wasnil = isnil;
				}
				if(wasnil)
					PushNils(output, nilcount);

				if(arraySize)
					lua_pushinteger(L, arraySize); // before first key
				else
					lua_pushnil(L); // before first key

				int keyIndex = lua_gettop(L);
				int valueIndex = keyIndex + 1;
				while(lua_next(L, i))
				{
					assert(lua_type(L, keyIndex) && "nil key in Lua table, impossible");
					assert(lua_type(L, valueIndex) && "nil value in Lua table, impossible");
					LuaStackToBinaryConverter(L, keyIndex, output);
					LuaStackToBinaryConverter(L, valueIndex, output);
					lua_pop(L, 1);
					hashSize++;
				}
			}

			int outputType = LUAEXT_TTABLE;
			if(arraySize & 0xFFFF0000)
				outputType |= LUAEXT_BITS_4A;
			else if(arraySize & 0xFF00)
				outputType |= LUAEXT_BITS_2A;
			else if(arraySize & 0xFF)
				outputType |= LUAEXT_BITS_1A;
			if(hashSize & 0xFFFF0000)
				outputType |= LUAEXT_BITS_4H;
			else if(hashSize & 0xFF00)
				outputType |= LUAEXT_BITS_2H;
			else if(hashSize & 0xFF)
				outputType |= LUAEXT_BITS_1H;
			output[outputTypeIndex] = outputType;

			int insertIndex = outputTypeIndex;
			if(BITMATCH(outputType,LUAEXT_BITS_4A) || BITMATCH(outputType,LUAEXT_BITS_2A) || BITMATCH(outputType,LUAEXT_BITS_1A))
				output.insert(output.begin() + (++insertIndex), arraySize & 0xFF);
			if(BITMATCH(outputType,LUAEXT_BITS_4A) || BITMATCH(outputType,LUAEXT_BITS_2A))
				output.insert(output.begin() + (++insertIndex), (arraySize & 0xFF00) >> 8);
			if(BITMATCH(outputType,LUAEXT_BITS_4A))
				output.insert(output.begin() + (++insertIndex), (arraySize & 0x00FF0000) >> 16),
				output.insert(output.begin() + (++insertIndex), (arraySize & 0xFF000000) >> 24);
			if(BITMATCH(outputType,LUAEXT_BITS_4H) || BITMATCH(outputType,LUAEXT_BITS_2H) || BITMATCH(outputType,LUAEXT_BITS_1H))
				output.insert(output.begin() + (++insertIndex), hashSize & 0xFF);
			if(BITMATCH(outputType,LUAEXT_BITS_4H) || BITMATCH(outputType,LUAEXT_BITS_2H))
				output.insert(output.begin() + (++insertIndex), (hashSize & 0xFF00) >> 8);
			if(BITMATCH(outputType,LUAEXT_BITS_4H))
				output.insert(output.begin() + (++insertIndex), (hashSize & 0x00FF0000) >> 16),
				output.insert(output.begin() + (++insertIndex), (hashSize & 0xFF000000) >> 24);

		}	break;
	}
}


// complements LuaStackToBinaryConverter
void BinaryToLuaStackConverter(lua_State* L, const unsigned char*& data, unsigned int& remaining)
{
	assert(s_dbg_dataSize - (data - s_dbg_dataStart) == remaining);

	unsigned char type = AdvanceByteStream<unsigned char>(data, remaining);

	switch(type)
	{
		default:
			{
				//printf("read unknown type %d (0x%x)\n", type, type);
				//assert(0);

				LuaContextInfo& info = GetCurrentInfo();
				if(info.print)
				{
					char errmsg [1024];
					if(type <= 10 && type != LUA_TTABLE)
						sprintf(errmsg, "values of type \"%s\" are not allowed to be loaded into registered load functions. The save state's Lua save data file might be corrupted.\r\n", lua_typename(L,type));
					else
						sprintf(errmsg, "The save state's Lua save data file seems to be corrupted.\r\n");
					info.print(luaStateToUIDMap[L], errmsg);
				}
				else
				{
					if(type <= 10 && type != LUA_TTABLE)
						fprintf(stderr, "values of type \"%s\" are not allowed to be loaded into registered load functions. The save state's Lua save data file might be corrupted.\n", lua_typename(L,type));
					else
						fprintf(stderr, "The save state's Lua save data file seems to be corrupted.\n");
				}
			}
			break;
		case LUA_TNIL:
			lua_pushnil(L);
			break;
		case LUA_TBOOLEAN:
			lua_pushboolean(L, AdvanceByteStream<UINT8>(data, remaining));
			break;
		case LUA_TSTRING:
			lua_pushstring(L, (const char*)data);
			AdvanceByteStream(data, remaining, strlen((const char*)data) + 1);
			break;
		case LUA_TNUMBER:
			lua_pushnumber(L, AdvanceByteStream<double>(data, remaining));
			break;
		case LUAEXT_TLONG:
			lua_pushinteger(L, AdvanceByteStream<INT32>(data, remaining));
			break;
		case LUAEXT_TUSHORT:
			lua_pushinteger(L, AdvanceByteStream<UINT16>(data, remaining));
			break;
		case LUAEXT_TSHORT:
			lua_pushinteger(L, AdvanceByteStream<INT16>(data, remaining));
			break;
		case LUAEXT_TBYTE:
			lua_pushinteger(L, AdvanceByteStream<UINT8>(data, remaining));
			break;
		case LUAEXT_TTABLE:
		case LUAEXT_TTABLE | LUAEXT_BITS_1A:
		case LUAEXT_TTABLE | LUAEXT_BITS_2A:
		case LUAEXT_TTABLE | LUAEXT_BITS_4A:
		case LUAEXT_TTABLE | LUAEXT_BITS_1H:
		case LUAEXT_TTABLE | LUAEXT_BITS_2H:
		case LUAEXT_TTABLE | LUAEXT_BITS_4H:
		case LUAEXT_TTABLE | LUAEXT_BITS_1A | LUAEXT_BITS_1H:
		case LUAEXT_TTABLE | LUAEXT_BITS_2A | LUAEXT_BITS_1H:
		case LUAEXT_TTABLE | LUAEXT_BITS_4A | LUAEXT_BITS_1H:
		case LUAEXT_TTABLE | LUAEXT_BITS_1A | LUAEXT_BITS_2H:
		case LUAEXT_TTABLE | LUAEXT_BITS_2A | LUAEXT_BITS_2H:
		case LUAEXT_TTABLE | LUAEXT_BITS_4A | LUAEXT_BITS_2H:
		case LUAEXT_TTABLE | LUAEXT_BITS_1A | LUAEXT_BITS_4H:
		case LUAEXT_TTABLE | LUAEXT_BITS_2A | LUAEXT_BITS_4H:
		case LUAEXT_TTABLE | LUAEXT_BITS_4A | LUAEXT_BITS_4H:
			{
				unsigned int arraySize = 0;
				if(BITMATCH(type,LUAEXT_BITS_4A) || BITMATCH(type,LUAEXT_BITS_2A) || BITMATCH(type,LUAEXT_BITS_1A))
					arraySize |= AdvanceByteStream<UINT8>(data, remaining);
				if(BITMATCH(type,LUAEXT_BITS_4A) || BITMATCH(type,LUAEXT_BITS_2A))
					arraySize |= ((UINT16)AdvanceByteStream<UINT8>(data, remaining)) << 8;
				if(BITMATCH(type,LUAEXT_BITS_4A))
					arraySize |= ((UINT32)AdvanceByteStream<UINT8>(data, remaining)) << 16,
					arraySize |= ((UINT32)AdvanceByteStream<UINT8>(data, remaining)) << 24;

				unsigned int hashSize = 0;
				if(BITMATCH(type,LUAEXT_BITS_4H) || BITMATCH(type,LUAEXT_BITS_2H) || BITMATCH(type,LUAEXT_BITS_1H))
					hashSize |= AdvanceByteStream<UINT8>(data, remaining);
				if(BITMATCH(type,LUAEXT_BITS_4H) || BITMATCH(type,LUAEXT_BITS_2H))
					hashSize |= ((UINT16)AdvanceByteStream<UINT8>(data, remaining)) << 8;
				if(BITMATCH(type,LUAEXT_BITS_4H))
					hashSize |= ((UINT32)AdvanceByteStream<UINT8>(data, remaining)) << 16,
					hashSize |= ((UINT32)AdvanceByteStream<UINT8>(data, remaining)) << 24;

				lua_createtable(L, arraySize, hashSize);

				unsigned int n = 1;
				while(n <= arraySize)
				{
					if(*data == LUAEXT_TNILS)
					{
						AdvanceByteStream(data, remaining, 1);
						n += AdvanceByteStream<UINT32>(data, remaining);
					}
					else
					{
						BinaryToLuaStackConverter(L, data, remaining); // push value
						lua_rawseti(L, -2, n); // table[n] = value
						n++;
					}
				}

				for(unsigned int h = 1; h <= hashSize; h++)
				{
					BinaryToLuaStackConverter(L, data, remaining); // push key
					BinaryToLuaStackConverter(L, data, remaining); // push value
					lua_rawset(L, -3); // table[key] = value
				}
			}
			break;
	}
}

static const unsigned char luaBinaryMajorVersion = 9;
static const unsigned char luaBinaryMinorVersion = 1;

unsigned char* LuaStackToBinary(lua_State* L, unsigned int& size)
{
	int n = lua_gettop(L);
	if(n == 0)
		return NULL;

	std::vector<unsigned char> output;
	output.push_back(luaBinaryMajorVersion);
	output.push_back(luaBinaryMinorVersion);

	for(int i = 1; i <= n; i++)
		LuaStackToBinaryConverter(L, i, output);

	unsigned char* rv = new unsigned char [output.size()];
	memcpy(rv, &output.front(), output.size());
	size = output.size();
	return rv;
}

void BinaryToLuaStack(lua_State* L, const unsigned char* data, unsigned int size, unsigned int itemsToLoad)
{
	unsigned char major = *data++;
	unsigned char minor = *data++;
	size -= 2;
	if(luaBinaryMajorVersion != major || luaBinaryMinorVersion != minor)
		return;

	while(size > 0 && itemsToLoad > 0)
	{
		BinaryToLuaStackConverter(L, data, size);
		itemsToLoad--;
	}
}

// saves Lua stack into a record and pops it
void LuaSaveData::SaveRecord(int uid, unsigned int key)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;
	if(!L)
		return;

	Record* cur = new Record();
	cur->key = key;
	cur->data = LuaStackToBinary(L, cur->size);
	cur->next = NULL;

	lua_settop(L,0);

	if(cur->size <= 0)
	{
		delete cur;
		return;
	}

	Record* last = recordList;
	while(last && last->next)
		last = last->next;
	if(last)
		last->next = cur;
	else
		recordList = cur;
}

// pushes a record's data onto the Lua stack
void LuaSaveData::LoadRecord(int uid, unsigned int key, unsigned int itemsToLoad) const
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;
	if(!L)
		return;

	Record* cur = recordList;
	while(cur)
	{
		if(cur->key == key)
		{
			s_dbg_dataStart = cur->data;
			s_dbg_dataSize = cur->size;
			BinaryToLuaStack(L, cur->data, cur->size, itemsToLoad);
			return;
		}
		cur = cur->next;
	}
}

// saves part of the Lua stack (at the given index) into a record and does NOT pop anything
void LuaSaveData::SaveRecordPartial(int uid, unsigned int key, int idx)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;
	if(!L)
		return;

	if(idx < 0)
		idx += lua_gettop(L)+1;

	Record* cur = new Record();
	cur->key = key;
	cur->next = NULL;

	if(idx <= lua_gettop(L))
	{
		std::vector<unsigned char> output;
		output.push_back(luaBinaryMajorVersion);
		output.push_back(luaBinaryMinorVersion);

		LuaStackToBinaryConverter(L, idx, output);

		unsigned char* rv = new unsigned char [output.size()];
		memcpy(rv, &output.front(), output.size());
		cur->size = output.size();
		cur->data = rv;
	}

	if(cur->size <= 0)
	{
		delete cur;
		return;
	}

	Record* last = recordList;
	while(last && last->next)
		last = last->next;
	if(last)
		last->next = cur;
	else
		recordList = cur;
}

void fwriteint(unsigned int value, FILE* file)
{
	for(int i=0;i<4;i++)
	{
		int w = value & 0xFF;
		fwrite(&w, 1, 1, file);
		value >>= 8;
	}
}
void freadint(unsigned int& value, FILE* file)
{
	int rv = 0;
	for(int i=0;i<4;i++)
	{
		int r = 0;
		fread(&r, 1, 1, file);
		rv |= r << (i*8);
	}
	value = rv;
}

// writes all records to an already-open file
void LuaSaveData::ExportRecords(void* fileV) const
{
	FILE* file = (FILE*)fileV;
	if(!file)
		return;

	Record* cur = recordList;
	while(cur)
	{
		fwriteint(cur->key, file);
		fwriteint(cur->size, file);
		fwrite(cur->data, cur->size, 1, file);
		cur = cur->next;
	}
}

// reads records from an already-open file
void LuaSaveData::ImportRecords(void* fileV)
{
	FILE* file = (FILE*)fileV;
	if(!file)
		return;

	ClearRecords();

	Record rec;
	Record* cur = &rec;
	Record* last = NULL;
	while(1)
	{
		freadint(cur->key, file);
		freadint(cur->size, file);

		if(feof(file) || ferror(file))
			break;

		cur->data = new unsigned char [cur->size];
		fread(cur->data, cur->size, 1, file);

		Record* next = new Record();
		memcpy(next, cur, sizeof(Record));
		next->next = NULL;

		if(last)
			last->next = next;
		else
			recordList = next;
		last = next;
	}
}

void LuaSaveData::ClearRecords()
{
	Record* cur = recordList;
	while(cur)
	{
		Record* del = cur;
		cur = cur->next;

		delete[] del->data;
		delete del;
	}

	recordList = NULL;
}



void DontWorryLua() // everything's going to be OK
{
	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		dontworry(*iter->second);
		++iter;
	}
}

void EnableStopAllLuaScripts(bool enable)
{
	g_stopAllScriptsEnabled = enable;
}

void StopAllLuaScripts()
{
	if(!g_stopAllScriptsEnabled)
		return;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		bool wasStarted = info.started;
		StopLuaScript(uid);
		info.restartLater = wasStarted;
		++iter;
	}
}

void RestartAllLuaScripts()
{
	if(!g_stopAllScriptsEnabled)
		return;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		if(info.restartLater || info.started)
		{
			info.restartLater = false;
			RunLuaScriptFile(uid, info.lastFilename.c_str());
		}
		++iter;
	}
}

// sets anything that needs to depend on the total number of scripts running
void RefreshScriptStartedStatus()
{
	int numScriptsStarted = 0;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		LuaContextInfo& info = *iter->second;
		if(info.started)
			numScriptsStarted++;
		++iter;
	}

//	frameadvSkipLagForceDisable = (numScriptsStarted != 0); // disable while scripts are running because currently lag skipping makes lua callbacks get called twice per frame advance
	g_numScriptsStarted = numScriptsStarted;
}

// sets anything that needs to depend on speed mode or running status of scripts
void RefreshScriptSpeedStatus()
{
	g_anyScriptsHighSpeed = false;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		LuaContextInfo& info = *iter->second;
		if(info.running)
			if(info.speedMode == SPEEDMODE_TURBO || info.speedMode == SPEEDMODE_MAXIMUM)
				g_anyScriptsHighSpeed = true;
		++iter;
	}
}

#endif // HAVE_LUA
