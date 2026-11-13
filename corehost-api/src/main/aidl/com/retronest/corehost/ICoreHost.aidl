// Phase 4.5 core-host contract (locked). App → host. Runs in the ":core" process.
//
// The protocol is deliberately generic ("an emulator that runs a game file on a Surface"): no
// libretro types cross it. Large data (save states, SRAM, thumbnails) travel as FILES in the app's
// shared private storage (both processes have the same UID), never as binder payloads (1 MB limit).
// Methods marked oneway are fire-and-forget and safe to call at input rate (touch / gamepad).
package com.retronest.corehost;

import android.os.Bundle;
import android.view.Surface;
import com.retronest.corehost.ICoreHostCallback;

interface ICoreHost {
    /**
     * Starts the session. args: CoreHostProtocol.ARG_* (core path, game path, system / save dirs,
     * game id, title, rumble, cheats, optional ROM fd). Core options and core-specific system
     * files are the host's own business. One session per host process; a second start() is ignored.
     */
    void start(in Bundle args, ICoreHostCallback callback);

    /** Attach (non-null) / detach (null). Synchronous: returns once the old surface is no longer drawn. */
    void setSurface(in Surface surface);

    /**
     * Synchronous (NOT oneway) on purpose: Binder lets a later synchronous call (saveState, loadState, flushSram,
     * shutdown, setSurface) overtake an earlier oneway one, so a oneway pause could still be unapplied when the
     * state is saved or the session shut down. Returns once the engine has the new pause state.
     */
    void setPaused(boolean paused);
    oneway void setButtons(int mask);
    oneway void setTurboButtons(int mask);
    oneway void setAnalog(int stick, int x, int y);
    oneway void setPointer(float x, float y, boolean pressed);
    oneway void setShader(String presetId, String vertex, String fragment, boolean linearFilter);
    oneway void clearShader();
    oneway void setDisplayRotation(int degrees);
    oneway void setTurboFrames(int frames);
    oneway void setAudioBufferBursts(int bursts);
    oneway void setPressWindowFrames(int frames);
    oneway void setRumbleEnabled(boolean enabled);

    // ---- Phase 4.5 WS4 (locked): gameplay features, capability-gated.
    /** retro_reset between frames. Synchronous so a following saveState / loadState cannot overtake it. */
    void reset();
    /** Fast-forward multiplier: 1 (normal), 2, 4. The host reports what it sustains via onSpeed. */
    oneway void setSpeed(int multiplier);
    /** Start / stop recording the rewind buffer (memory is released when disabled). */
    oneway void setRewindEnabled(boolean enabled);
    /** While true, step backwards through the rewind buffer instead of running. */
    oneway void setRewinding(boolean active);
    /** Replaces all cheats; empty = none. Codes are core-specific strings. */
    oneway void setCheats(in String[] codes);
    /**
     * Per-game capabilities, keys in CoreHostProtocol: CAP_RESET, CAP_SPEED_MAX (int), CAP_REWIND,
     * CAP_CHEATS (booleans). Blocks until the game is loaded; all-false/1 when it never loads.
     */
    Bundle getCapabilities();

    /**
     * Serializes the running game into statePath and a PNG thumbnail into thumbnailPath (thumbnail
     * optional: false/absent file when no frame). Returns false if the core cannot serialize.
     * Blocks until written (runs between frames on the emulation thread).
     */
    boolean saveState(String statePath, String thumbnailPath);

    /** Restores the state file written by saveState for the same game / core. */
    boolean loadState(String statePath);

    /** Writes battery SRAM to the save dir if the game has any. Blocks until written. */
    void flushSram();

    /** Flushes SRAM, unloads, deinitializes; the host process then exits. Blocks until done. */
    void shutdown();
}
