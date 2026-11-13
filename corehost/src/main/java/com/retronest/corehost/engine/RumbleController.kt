package com.retronest.corehost.engine

import android.content.Context
import android.os.Build
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager

/** Receives the core's rumble strength (0..65535) after every frame, on the emulation thread. */
interface RumbleOutput {
    fun update(strength: Int)
    fun stop()
}

/** Forwards rumble only while [enabled] is true (Settings → Rumble); otherwise behaves as strength 0. */
class GatedRumbleOutput(
    private val delegate: RumbleOutput,
    private val enabled: () -> Boolean,
) : RumbleOutput {
    override fun update(strength: Int) = delegate.update(if (enabled()) strength else 0)
    override fun stop() = delegate.stop()
}

/** What the vibrator should do for one poll. */
sealed interface RumbleCommand {
    data object None : RumbleCommand
    data object Cancel : RumbleCommand
    /** [amplitude] is 1..255. */
    data class Vibrate(val amplitude: Int, val durationMs: Long) : RumbleCommand
}

/** Pure throttling logic: at most one vibrate per [RETRIGGER_MS], cancel once the strength drops to 0. */
class RumblePlanner {
    private var active = false
    private var lastVibrateAt = Long.MIN_VALUE

    fun next(strength: Int, nowMs: Long): RumbleCommand {
        if (strength <= 0) {
            if (!active) return RumbleCommand.None
            active = false
            return RumbleCommand.Cancel
        }
        if (active && nowMs - lastVibrateAt < RETRIGGER_MS) return RumbleCommand.None
        if (!active && lastVibrateAt != Long.MIN_VALUE && nowMs - lastVibrateAt < RETRIGGER_MS) return RumbleCommand.None
        active = true
        lastVibrateAt = nowMs
        return RumbleCommand.Vibrate(amplitudeFor(strength), PULSE_MS)
    }

    fun reset() {
        active = false
    }

    companion object {
        const val RETRIGGER_MS = 30L
        const val PULSE_MS = 40L

        fun amplitudeFor(strength: Int): Int {
            val s = strength.coerceIn(0, 65535)
            return (1 + s * 254L / 65535L).toInt().coerceIn(1, 255)
        }
    }
}

/** Turns core rumble into device vibration. Never throws. */
class RumbleController(
    context: Context,
    private val clock: () -> Long = SystemClock::uptimeMillis,
) : RumbleOutput {

    private val vibrator: Vibrator? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Vibrator::class.java)
        }?.takeIf { it.hasVibrator() }

    private val planner = RumblePlanner()

    override fun update(strength: Int) {
        val v = vibrator ?: return
        try {
            when (val cmd = planner.next(strength, clock())) {
                RumbleCommand.None -> Unit
                RumbleCommand.Cancel -> v.cancel()
                is RumbleCommand.Vibrate -> {
                    val amplitude = if (v.hasAmplitudeControl()) cmd.amplitude else VibrationEffect.DEFAULT_AMPLITUDE
                    v.vibrate(VibrationEffect.createOneShot(cmd.durationMs, amplitude))
                }
            }
        } catch (_: RuntimeException) {
            // Vibration can be blocked by the OEM / power saving; never crash gameplay.
        }
    }

    override fun stop() {
        planner.reset()
        try {
            vibrator?.cancel()
        } catch (_: RuntimeException) {
        }
    }
}
