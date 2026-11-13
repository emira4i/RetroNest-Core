package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Test

class PointerMappingTest {
    @Test
    fun pointerMapsToLibretroRange() {
        assertEquals(-32767, pointerToLibretro(0f))
        assertEquals(32767, pointerToLibretro(1f))
        assertEquals(0, pointerToLibretro(0.5f))
        assertEquals(-32767, pointerToLibretro(-3f))
        assertEquals(32767, pointerToLibretro(7f))
    }
}
