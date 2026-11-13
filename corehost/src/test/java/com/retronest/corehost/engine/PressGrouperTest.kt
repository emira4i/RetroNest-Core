package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Test

class PressGrouperTest {
    private val A = 1
    private val B = 2
    private val C = 4

    @Test
    fun windowZeroIsPassthrough() {
        val g = PressGrouper(0)
        assertEquals(A, g.next(A))
        assertEquals(A or B, g.next(A or B))
        assertEquals(0, g.next(0))
    }

    @Test
    fun twoPressesOneFrameApartArriveTogetherWindow1() {
        val g = PressGrouper(1)
        assertEquals(0, g.next(A))
        assertEquals(A or B, g.next(A or B))
        assertEquals(A or B, g.next(A or B))
    }

    @Test
    fun threePressesWithinWindow2ArriveTogether() {
        val g = PressGrouper(2)
        assertEquals(0, g.next(A))
        assertEquals(0, g.next(A or B))
        assertEquals(A or B or C, g.next(A or B or C))
    }

    @Test
    fun windowClosesAfterNFramesWithoutSecondPress() {
        val g = PressGrouper(2)
        assertEquals(0, g.next(A))
        assertEquals(0, g.next(A))
        assertEquals(A, g.next(A))
        val g1 = PressGrouper(1)
        assertEquals(0, g1.next(A))
        assertEquals(A, g1.next(A))
    }

    @Test
    fun alreadyHeldBitIsNotDelayed() {
        val g = PressGrouper(1)
        g.next(A)
        g.next(A)
        assertEquals(A, g.next(A))
        assertEquals(A, g.next(A or B)) // B waits, A stays
        assertEquals(A or B, g.next(A or B))
    }

    @Test
    fun releaseIsImmediate() {
        val g = PressGrouper(1)
        g.next(A)
        g.next(A)
        assertEquals(0, g.next(0))
    }

    @Test
    fun releasedAppliedBitDropsWhileOthersPending() {
        val g = PressGrouper(2)
        g.next(A); g.next(A); g.next(A)
        assertEquals(0, g.next(B))
    }

    @Test
    fun tapShorterThanWindowIsDeliveredForOneFrame() {
        val g = PressGrouper(2)
        assertEquals(0, g.next(A))
        assertEquals(0, g.next(0))
        assertEquals(A, g.next(0))
        assertEquals(0, g.next(0))
    }

    @Test
    fun tapJoinsGroupAndIsReleasedAfterOneFrame() {
        val g = PressGrouper(2)
        assertEquals(0, g.next(A))
        assertEquals(0, g.next(A or B))
        assertEquals(A or B, g.next(B)) // A was tapped: delivered with the group
        assertEquals(B, g.next(B))      // and gone the next frame
    }

    @Test
    fun tappedBitDoesNotStick() {
        val g = PressGrouper(1)
        g.next(A)
        assertEquals(A, g.next(0))
        assertEquals(0, g.next(0))
    }

    @Test
    fun resetKeepsHeldBitsWithoutDelay() {
        val g = PressGrouper(2)
        g.reset(A)
        assertEquals(A, g.next(A))
    }
}
