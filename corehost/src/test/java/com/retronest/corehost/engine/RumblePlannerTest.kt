package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RumblePlannerTest {

    @Test
    fun amplitudeScalesInto1To255() {
        assertEquals(1, RumblePlanner.amplitudeFor(0))
        assertEquals(255, RumblePlanner.amplitudeFor(65535))
        val mid = RumblePlanner.amplitudeFor(32768)
        assertTrue(mid in 120..135)
    }

    @Test
    fun throttlesRetriggersAndCancelsOnZero() {
        val p = RumblePlanner()
        assertEquals(RumbleCommand.None, p.next(0, 0))
        assertTrue(p.next(65535, 100) is RumbleCommand.Vibrate)
        assertEquals(RumbleCommand.None, p.next(65535, 116)) // < 30 ms since last vibrate
        assertTrue(p.next(65535, 130) is RumbleCommand.Vibrate)
        assertEquals(RumbleCommand.Cancel, p.next(0, 140))
        assertEquals(RumbleCommand.None, p.next(0, 150))
    }

    @Test
    fun neverTwoVibratesWithinThrottleWindowEvenAfterCancel() {
        val p = RumblePlanner()
        assertTrue(p.next(1000, 0) is RumbleCommand.Vibrate)
        assertEquals(RumbleCommand.Cancel, p.next(0, 5))
        assertEquals(RumbleCommand.None, p.next(1000, 10))
        assertTrue(p.next(1000, 30) is RumbleCommand.Vibrate)
    }
}
