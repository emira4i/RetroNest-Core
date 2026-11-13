package com.retronest.corehost.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OutputPolicyTest {

    @Test
    fun softwareCoreKeepsFrameSinkFallback() {
        val p = OutputPolicy(hwRender = false)
        assertTrue(p.usesFrameSink(gpuActive = false))
        assertFalse(p.usesFrameSink(gpuActive = true))
        assertFalse(p.gpuFailureIsFatal)
        assertTrue(p.readsBackForThumbnail(gpuActive = true))
        assertFalse(p.readsBackForThumbnail(gpuActive = false)) // software frames are already in pixels
    }

    @Test
    fun hwCoreHasNoSoftwarePath() {
        val p = OutputPolicy(hwRender = true)
        assertFalse(p.usesFrameSink(gpuActive = false))
        assertFalse(p.usesFrameSink(gpuActive = true))
        assertTrue(p.gpuFailureIsFatal)
        // Thumbnails read the FBO back even while no Surface is attached.
        assertTrue(p.readsBackForThumbnail(gpuActive = false))
        assertTrue(p.readsBackForThumbnail(gpuActive = true))
    }

    @Test
    fun framePacingFollowsCoreFpsAndRejectsNonsense() {
        assertEquals(16_666_666L, FramePacing.frameNs(60.0))
        assertEquals(33_366_666L, FramePacing.frameNs(60.0 / 1.001 / 2.0)) // PPSSPP swap interval 2
        assertEquals(FramePacing.frameNs(FramePacing.DEFAULT_FPS), FramePacing.frameNs(0.0))
        assertEquals(FramePacing.frameNs(FramePacing.DEFAULT_FPS), FramePacing.frameNs(Double.NaN))
        assertEquals(FramePacing.frameNs(FramePacing.DEFAULT_FPS), FramePacing.frameNs(1000.0))
        assertEquals(50.0, FramePacing.sanitize(50.0), 0.0)
    }
}
