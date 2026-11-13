package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class RewindBufferTest {

    private fun RewindBuffer.push(value: Int) {
        val a = acquire(2)
        a[0] = value.toByte()
        commit()
    }

    @Test
    fun capacityIsCapOverSizeWithAMinimum() {
        assertEquals(64 * 1024 * 1024 / 100_000, RewindBuffer.capacityFor(100_000))
        assertEquals(32, RewindBuffer.capacityFor(2 * 1024 * 1024)) // the byte cap is a hard limit
        assertEquals(RewindBuffer.MIN_STATES, RewindBuffer.capacityFor(0))
    }

    @Test
    fun viabilityNeedsSmallStateAndFastSerialize() {
        assertTrue(RewindBuffer.isViable(100_000, 500_000))
        assertFalse(RewindBuffer.isViable(3 * 1024 * 1024, 100_000)) // N64 / PSP / NDS sized
        assertFalse(RewindBuffer.isViable(100_000, 2_500_000))
        assertFalse(RewindBuffer.isViable(0, 10))
    }

    @Test
    fun stepBackDropsTheNewestAndReturnsThePreviousState() {
        val b = RewindBuffer(4)
        (1..3).forEach { b.push(it) }
        assertEquals(3, b.count)
        assertEquals(2, b.stepBack()!![0].toInt())
        assertEquals(2, b.count)
        assertEquals(1, b.stepBack()!![0].toInt())
        assertEquals(1, b.count)
    }

    @Test
    fun staysOnTheOldestWhenOnlyOneStateIsLeft() {
        val b = RewindBuffer(4)
        b.push(7)
        b.push(8)
        assertEquals(7, b.stepBack()!![0].toInt())
        assertEquals(7, b.stepBack()!![0].toInt())
        assertEquals(7, b.stepBack()!![0].toInt())
        assertEquals(1, b.count)
    }

    @Test
    fun emptyBufferHasNothingToRestore() {
        val b = RewindBuffer(3)
        assertNull(b.stepBack())
        b.push(1)
        b.clear()
        assertNull(b.stepBack())
        assertEquals(0, b.count)
    }

    @Test
    fun recordingAfterRewindingDiscardsTheNewerStates() {
        val b = RewindBuffer(8)
        (1..5).forEach { b.push(it) }
        b.stepBack() // restores 4, 5 is gone
        b.stepBack() // restores 3, 4 is gone
        b.push(9)
        assertEquals(4, b.count)
        assertEquals(3, b.stepBack()!![0].toInt()) // 9 dropped, 3 restored
        assertEquals(2, b.stepBack()!![0].toInt())
        assertEquals(1, b.stepBack()!![0].toInt())
    }

    @Test
    fun fullRingOverwritesTheOldest() {
        val b = RewindBuffer(3)
        (1..5).forEach { b.push(it) }
        assertEquals(3, b.count)
        assertEquals(4, b.stepBack()!![0].toInt())
        assertEquals(3, b.stepBack()!![0].toInt())
        assertEquals(3, b.stepBack()!![0].toInt()) // 3 is the oldest left
    }

    @Test
    fun abortOnAFullRingDropsTheOverwrittenOldest() {
        val b = RewindBuffer(2)
        b.push(1)
        b.push(2)
        b.acquire(2) // serialize into the oldest slot failed
        b.abort()
        assertEquals(1, b.count)
        assertEquals(2, b.stepBack()!![0].toInt())
    }

    @Test
    fun clearReleasesEverything() {
        val b = RewindBuffer(2)
        b.push(1)
        b.clear()
        assertEquals(0, b.count)
        b.push(5)
        assertEquals(5, b.stepBack()!![0].toInt())
    }
}
