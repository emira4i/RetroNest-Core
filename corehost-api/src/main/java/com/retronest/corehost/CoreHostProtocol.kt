package com.retronest.corehost

/*
 * Phase 4.5 core-host contract (locked). Keys and constants shared by the app side
 * (RemoteEmulationEngine) and the host side (CoreHostService, ":core" process).
 *
 * Core-agnostic by rule: the app says WHICH core runs WHICH game, never how the core is tuned.
 * Libretro option keys / values and per-core system files (e.g. PPSSPP's fonts) belong to the
 * host alone (CoreOptionDefaults / PpssppAssetInstaller in :corehost). A future user-facing
 * setting that has to change a core option crosses the boundary as a generic, core-independent
 * key defined here (e.g. a boolean "SETTING_HIGH_RESOLUTION" in a start() Bundle), which the host
 * translates for whichever core it runs and ignores where it has no meaning.
 */
object CoreHostProtocol {
    const val ARG_CORE_PATH = "corePath"
    const val ARG_ROM_PATH = "romPath"
    const val ARG_SYSTEM_DIR = "systemDir"
    const val ARG_SAVE_DIR = "saveDir"
    const val ARG_GAME_ID = "gameId"
    const val ARG_TITLE = "title"
    /** Boolean: rumble initially enabled. */
    const val ARG_RUMBLE = "rumble"
    /**
     * ParcelFileDescriptor (optional): the ROM's open file descriptor when [ARG_ROM_PATH] is a
     * symlink to the app's `/proc/self/fd/N` (large content:// PS1 / NDS images that are not copied).
     * That link means nothing in the host process, so the host links its own copy of the fd under
     * the same file name instead.
     */
    const val ARG_ROM_FD = "romFd"

    /** Boolean ("true"/"false" string entry): the core implements retro_cheat_set. Phase 4.5 WS4 (locked). */
    const val ARG_CHEATS_SUPPORTED = "cheatsSupported"

    // Phase 4.5 WS4 (locked): getCapabilities() Bundle keys.
    /** Boolean: reset() works. */
    const val CAP_RESET = "capReset"
    /** Int: highest fast-forward multiplier offered (1 = none). */
    const val CAP_SPEED_MAX = "capSpeedMax"
    /** Boolean: rewind is available for this game. */
    const val CAP_REWIND = "capRewind"
    /** Boolean: cheats can be applied. */
    const val CAP_CHEATS = "capCheats"

    const val STATE_IDLE = 0
    const val STATE_LOADING = 1
    const val STATE_RUNNING = 2
    const val STATE_ERROR = 3
    const val STATE_STOPPED = 4

    /** Process name suffix of the host (android:process=":core"); app-only init must skip it. */
    const val PROCESS_SUFFIX = ":core"

    /** Suffix of the alternate host process: sessions alternate between the two so a new game never lands in a process that is still shutting down. */
    const val PROCESS_SUFFIX_ALT = ":core2"

    /** Class name of the host service (declared in the :corehost manifest); the app binds it by name. */
    const val HOST_SERVICE_CLASS = "com.retronest.corehost.engine.CoreHostService"

    /** Same service in the alternate process (":core2"). */
    const val HOST_SERVICE_CLASS_ALT = "com.retronest.corehost.engine.CoreHostServiceAlt"
}
