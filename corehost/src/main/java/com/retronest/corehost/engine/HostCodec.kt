package com.retronest.corehost.engine

import android.os.Bundle
import com.retronest.corehost.CoreHostProtocol

/** Host side of what crosses the binder boundary (the app has its own mirror-image codec). */
object HostCodec {

    /** The start() Bundle's launch entries; null when a required entry (core / ROM / save dir / game id) is missing. */
    fun launchArgs(get: (String) -> String?): GameLaunchArgs? {
        val corePath = get(CoreHostProtocol.ARG_CORE_PATH)?.takeIf { it.isNotEmpty() } ?: return null
        val romPath = get(CoreHostProtocol.ARG_ROM_PATH)?.takeIf { it.isNotEmpty() } ?: return null
        val saveDir = get(CoreHostProtocol.ARG_SAVE_DIR)?.takeIf { it.isNotEmpty() } ?: return null
        val gameId = get(CoreHostProtocol.ARG_GAME_ID)?.takeIf { it.isNotEmpty() } ?: return null
        return GameLaunchArgs(
            corePath = corePath,
            romPath = romPath,
            systemDir = get(CoreHostProtocol.ARG_SYSTEM_DIR).orEmpty(),
            saveDir = saveDir,
            gameId = gameId,
            title = get(CoreHostProtocol.ARG_TITLE).orEmpty(),
            cheatsSupported = get(CoreHostProtocol.ARG_CHEATS_SUPPORTED) == "true",
        )
    }

    /** One ICoreHostCallback.onState call. */
    data class WireState(val kind: Int, val text1: String?, val text2: String?, val paused: Boolean)

    fun encodeState(state: EngineState): WireState = when (state) {
        EngineState.Idle -> WireState(CoreHostProtocol.STATE_IDLE, null, null, false)
        is EngineState.Loading -> WireState(CoreHostProtocol.STATE_LOADING, state.message, null, false)
        is EngineState.Running -> WireState(CoreHostProtocol.STATE_RUNNING, state.coreName, null, state.paused)
        is EngineState.Error -> WireState(CoreHostProtocol.STATE_ERROR, state.title, state.detail, false)
        EngineState.Stopped -> WireState(CoreHostProtocol.STATE_STOPPED, null, null, false)
    }

    fun encodeCapabilities(c: EngineCapabilities): Bundle = Bundle().apply {
        putBoolean(CoreHostProtocol.CAP_RESET, c.reset)
        putInt(CoreHostProtocol.CAP_SPEED_MAX, c.maxSpeed)
        putBoolean(CoreHostProtocol.CAP_REWIND, c.rewind)
        putBoolean(CoreHostProtocol.CAP_CHEATS, c.cheats)
    }
}
