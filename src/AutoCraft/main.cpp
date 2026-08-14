#include "../../inc/main.h"
#include "script.h"

#include "../../inc/MinHook.h"

/**
 * Initializes and tears down MinHook around the ASI's ScriptHook registration.
 *
 * @param hInstance Module handle supplied by Windows.
 * @param reason DLL lifecycle notification supplied by Windows.
 * @return FALSE only when MinHook cannot initialize during process attach.
 */
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
