#include "../../inc/main.h"
#include "script.h"

#include "../../inc/MinHook.h"

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hInstance);
		if (MH_Initialize() != MH_OK) {
			return FALSE;
		}

		scriptRegister(hInstance, ScriptMain);
		break;
	case DLL_PROCESS_DETACH:
		MH_DisableHook(MH_ALL_HOOKS);
		MH_RemoveHook(MH_ALL_HOOKS);
		MH_Uninitialize();
		
		scriptUnregister(hInstance);
		break;
	}

	return TRUE;
}
