package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SpeedGovernorTest {

    private val budget = 16_000_000L // one 60 Hz frame
    private val second = 1_000_000_000L

    /** Feeds ticks of [coreNs] each, 60 per second, for [seconds]; returns the time reached. */
    private fun SpeedGovernor.run(coreNs: Long, seconds: Int, start: Long = 0, ticks: Int = 60 * seconds): Long {
        var now = start
        repeat(ticks) {
            now += budget
            onTick(now, coreNs, budget)
        }
        return now
    }

    @Test
    fun normalSpeedNeverChanges() {
        val g = SpeedGovernor()
        g.run(coreNs = 50_000_000, seconds = 3)
        assertEquals(1, g.achieved)
    }

    @Test
    fun keepsTheRequestedSpeedWhenTheCoreIsFastEnough() {
        val g = SpeedGovernor()
        g.setRequested(4)
        g.run(coreNs = 6_000_000, seconds = 5) // 4 frames in 6 ms
        assertEquals(4, g.achieved)
    }

    @Test
    fun dropsToTheHighestSustainableMultiplier() {
        val g = SpeedGovernor()
        g.setRequested(4)
        // 4 frames cost 24 ms (> 16 ms budget): 2 frames (12 ms) fit.
        g.run(coreNs = 24_000_000, seconds = 0, ticks = 65) // just over one window
        assertEquals(2, g.achieved)
        assertEquals(4, g.requested)
    }

    @Test
    fun fallsBackToNormalSpeedWhenEvenTwoFramesDoNotFit() {
        val g = SpeedGovernor()
        g.setRequested(4)
        g.run(coreNs = 80_000_000, seconds = 2) // 20 ms per frame
        assertEquals(1, g.achieved)
    }

    @Test
    fun reportsAChangeOnlyWhenTheWindowEndsWithOverruns() {
        val g = SpeedGovernor()
        g.setRequested(2)
        var changed = false
        var now = 0L
        repeat(59) { now += budget; changed = changed or g.onTick(now, 40_000_000, budget) }
        assertFalse(changed) // window (1 s) not over yet
        now += budget * 5
        assertTrue(g.onTick(now, 40_000_000, budget))
        assertEquals(1, g.achieved)
    }

    @Test
    fun aNewRequestProbesAgainFromTheRequestedSpeed() {
        val g = SpeedGovernor()
        g.setRequested(4)
        g.run(coreNs = 80_000_000, seconds = 2)
        assertEquals(1, g.achieved)
        g.setRequested(2)
        assertEquals(2, g.achieved)
        g.setRequested(1)
        assertEquals(1, g.achieved)
    }

    @Test
    fun occasionalSlowTicksDoNotTriggerAFallback() {
        val g = SpeedGovernor()
        g.setRequested(4)
        var now = 0L
        repeat(120) { i ->
            now += budget
            g.onTick(now, if (i % 10 == 0) 30_000_000 else 5_000_000, budget)
        }
        assertEquals(4, g.achieved)
    }
}
