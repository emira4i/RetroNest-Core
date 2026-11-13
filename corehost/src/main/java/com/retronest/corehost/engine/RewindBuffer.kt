package com.retronest.corehost.engine

/**
 * Phase 4.5 WS4: ring buffer of serialized core states for rewind (pure logic, no native calls).
 *
 * Slots are allocated lazily and reused, so recording does not allocate once the ring is full.
 * Protocol: `val dst = acquire(size)`; serialize into [dst]; then [commit] (or [abort] if the core
 * failed to serialize). While rewinding, [stepBack] pops the newest state (states newer than the
 * point the player resumes from are thereby discarded) and returns the one to restore.
 */
class RewindBuffer(val capacity: Int) {
    init {
        require(capacity > 0) { "capacity must be positive" }
    }

    private var slots = arrayOfNulls<ByteArray>(capacity)
    private var start = 0

    /** Number of states currently held. */
    var count = 0
        private set

    /** The array to serialize the next state into (the oldest slot's storage is reused when full). */
    fun acquire(size: Int): ByteArray {
        val index = if (count < capacity) (start + count) % capacity else start
        var array = slots[index]
        if (array == null || array.size != size) {
            array = ByteArray(size)
            slots[index] = array
        }
        return array
    }

    /** The state written into the last [acquire]d array becomes the newest. */
    fun commit() {
        if (count < capacity) count++ else start = (start + 1) % capacity
    }

    /** The last [acquire]d array was not filled: when the ring is full its oldest state is gone. */
    fun abort() {
        if (count == capacity) {
            start = (start + 1) % capacity
            count--
        }
    }

    /**
     * One rewind step: drops the newest state and returns the one before it. With a single state
     * left it returns that one without dropping it (stays on the oldest). Null when empty.
     */
    fun stepBack(): ByteArray? {
        if (count == 0) return null
        if (count > 1) count--
        return slots[(start + count - 1) % capacity]
    }

    /** Forgets every state and releases the memory. */
    fun clear() {
        slots = arrayOfNulls(capacity)
        start = 0
        count = 0
    }

    companion object {
        const val DEFAULT_CAP_BYTES = 64L * 1024 * 1024
        /** Slots used when the state size is unknown. */
        const val MIN_STATES = 30
        /** A state captured every this many emulated frames. */
        const val CAPTURE_INTERVAL_FRAMES = 2
        const val MAX_STATE_BYTES = 2 * 1024 * 1024
        const val MAX_SERIALIZE_NS = 2_000_000L

        /**
         * Slots = cap / state size: the byte cap is a hard limit (the :core process has no large heap);
         * every viable state (<= [MAX_STATE_BYTES]) still gets at least 32 slots under the default cap.
         */
        fun capacityFor(stateSize: Int, capBytes: Long = DEFAULT_CAP_BYTES): Int {
            if (stateSize <= 0) return MIN_STATES
            return (capBytes / stateSize).coerceIn(1L, Int.MAX_VALUE.toLong()).toInt()
        }

        /** Rewind is offered only for small states that serialize quickly (avg of measured runs). */
        fun isViable(stateSize: Int, avgSerializeNs: Long): Boolean =
            stateSize in 1..MAX_STATE_BYTES && avgSerializeNs < MAX_SERIALIZE_NS
    }
}

/**
 * Phase 4.5 WS4: decides which fast-forward multiplier the core really sustains. Pure logic.
 *
 * Feed it one sample per paced tick: the time the core needed for the tick's frames and the tick's
 * time budget (one frame at normal speed). If more than a quarter of the ticks of a ~1 s window
 * overran, it drops to the highest multiplier (4, 2, 1) the measured per-frame cost allows.
 */
class SpeedGovernor(private val windowNs: Long = 1_000_000_000L) {
    /** What the user asked for (1, 2 or 4). */
    var requested = 1
        private set

    /** What is actually run; never above [requested]. */
    var achieved = 1
        private set

    private var windowStart = -1L
    private var ticks = 0
    private var overruns = 0
    private var workSum = 0L

    fun setRequested(multiplier: Int) {
        requested = if (multiplier >= 4) 4 else if (multiplier >= 2) 2 else 1
        achieved = requested // probe again
        resetWindow()
    }

    /** True when [achieved] changed. [coreNs] = time spent in the core for this tick's [achieved] frames. */
    fun onTick(nowNs: Long, coreNs: Long, budgetNs: Long): Boolean {
        if (achieved <= 1) return false
        if (windowStart < 0) windowStart = nowNs
        ticks++
        workSum += coreNs
        if (coreNs > budgetNs) overruns++
        if (nowNs - windowStart < windowNs) return false

        var changed = false
        if (overruns * 4 > ticks) {
            val frameCost = workSum.toDouble() / (ticks.toLong() * achieved)
            val next = listOf(4, 2, 1).firstOrNull { it < achieved && it * frameCost <= budgetNs * 0.95 } ?: 1
            achieved = next
            changed = true
        }
        resetWindow()
        return changed
    }

    private fun resetWindow() {
        windowStart = -1L
        ticks = 0
        overruns = 0
        workSum = 0L
    }
}
