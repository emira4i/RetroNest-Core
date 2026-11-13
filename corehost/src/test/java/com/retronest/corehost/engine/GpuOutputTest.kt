package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GpuOutputTest {

    private class Ops : GpuOps<String> {
        val calls = mutableListOf<String>()
        var attachOk = true
        var presentOk = true
        override fun setSurface(surface: String?): Boolean {
            calls += "surface:$surface"
            return surface == null || attachOk
        }
        override fun setShader(source: ShaderSource): Boolean { calls += "shader:${source.presetId}"; return true }
        override fun clearShader() { calls += "shader:none" }
        override fun setRotation(degrees: Int) { calls += "rot:$degrees" }
        override fun present(): Boolean { calls += "present"; return presentOk }
    }

    private val ops = Ops()
    private val out = GpuOutput(ops)
    private val shader = ShaderSource("crt", "v", "f", true)

    @Test
    fun attachSuccessActivatesAndAppliesState() {
        assertTrue(out.attach("s1"))
        assertTrue(out.active)
        assertTrue(out.applyShader(shader))
        out.applyRotation(90)
        assertTrue(out.present())
        assertEquals(listOf("surface:s1", "shader:crt", "rot:90", "present"), ops.calls)
    }

    @Test
    fun clearingTheShaderReturnsToTheBuiltInOutput() {
        out.attach("s1")
        assertTrue(out.applyShader(shader))
        assertTrue(out.applyShader(null))
        assertEquals(listOf("surface:s1", "shader:crt", "shader:none"), ops.calls)
    }

    @Test
    fun attachFailureStaysSoftware() {
        ops.attachOk = false
        assertFalse(out.attach("s1"))
        assertFalse(out.active)
        assertFalse(out.present())
        assertFalse(out.applyShader(shader))
        out.applyRotation(90)
        out.detach() // nothing attached natively: no call
        assertEquals(listOf("surface:s1"), ops.calls)
    }

    @Test
    fun presentFailureDropsToSoftwareUntilNextAttach() {
        out.attach("s1")
        ops.presentOk = false
        assertFalse(out.present())
        assertFalse(out.active)
        assertEquals(listOf("surface:s1", "present", "surface:null"), ops.calls)
        assertFalse(out.present()) // no further native calls while in software
        ops.presentOk = true
        assertTrue(out.attach("s2"))
        assertTrue(out.present())
    }

    @Test
    fun detachReleasesNativeSurfaceOnce() {
        out.attach("s1")
        out.detach()
        out.detach()
        assertFalse(out.active)
        assertEquals(listOf("surface:s1", "surface:null"), ops.calls)
    }
}
