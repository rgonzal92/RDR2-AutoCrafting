// Licensed under the MIT License.

#pragma once

/**
 * ScriptHook entry point for the AutoCraft runtime.
 *
 * Resolves required game addresses, installs either normal or diagnostic
 * native hooks, and then remains registered with ScriptHook for the session.
 */
void ScriptMain();
