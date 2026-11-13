package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TurboPatternTest {

    @Test
    fun defaultIsThreeFramesOnThreeOff() {
        val pattern = (0L..11L).map { TurboPattern.effectiveMask(buttons = 0x1, turbo = 0x100, frame = it) }
        assertEquals(listOf(0x101, 0x101, 0x101, 0x1, 0x1, 0x1, 0x101, 0x101, 0x101, 0x1, 0x1, 0x1), pattern)
    }

    @Test
    fun cycleStartsInThePressedPhaseForEveryRate() {
        for (n in listOf(2, 3, 5)) {
            assertTrue(TurboPattern.isOn(0, n))
            assertTrue(TurboPattern.isOn((n - 1).toLong(), n))
            assertFalse(TurboPattern.isOn(n.toLong(), n))
            assertFalse(TurboPattern.isOn((2 * n - 1).toLong(), n))
            assertTrue(TurboPattern.isOn((2 * n).toLong(), n))
        }
    }

    @Test
    fun comboBitsAreAlwaysPressedTogether() {
        val combo = 0x300 // e.g. B + C
        for (n in listOf(2, 3, 5)) {
            for (f in 0L..30L) {
                val m = TurboPattern.effectiveMask(0, combo, f, n)
                assertTrue("frame $f rate $n", m == combo || m == 0)
            }
            // A tap restarts at frame 0: all bits are on for the first n frames.
            for (f in 0 until n) assertEquals(combo, TurboPattern.effectiveMask(0, combo, f.toLong(), n))
        }
    }

    @Test
    fun invalidRateFallsBackToDefault() {
        assertEquals(3, TurboPattern.normalize(4))
        assertEquals(TurboPattern.isOn(3, 3), TurboPattern.isOn(3, 99))
    }

    @Test
    fun noTurboLeavesButtonsAlone() {
        for (f in 0L..5L) assertEquals(0x30, TurboPattern.effectiveMask(0x30, 0, f))
    }

    @Test
    fun heldButtonStaysPressedWhileTurboIsOff() {
        assertEquals(0x100, TurboPattern.effectiveMask(0x100, 0x100, 3))
    }
}
