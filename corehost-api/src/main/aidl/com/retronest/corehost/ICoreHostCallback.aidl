// Phase 4.5 core-host contract (locked). Host → app events. Emulator-neutral on purpose.
package com.retronest.corehost;

oneway interface ICoreHostCallback {
    /**
     * Engine state. kind: see CoreHostProtocol.STATE_* ; text1/text2 = message (Loading),
     * coreName (Running) or title/detail (Error); paused only meaningful for Running.
     */
    void onState(int kind, String text1, String text2, boolean paused);

    /** True once frames are drawn by the GPU path. */
    void onGpuRendering(boolean active);

    /** Phase 4.5 WS4 (locked): the fast-forward multiplier the core actually sustains (1, 2 or 4). */
    void onSpeed(int achievedMultiplier);
}
