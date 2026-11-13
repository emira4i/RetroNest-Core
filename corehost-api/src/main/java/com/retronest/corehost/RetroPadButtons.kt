package com.retronest.corehost

/**
 * RetroPad button bitmask. Bit N = RETRO_DEVICE_ID_JOYPAD_N in libretro.h, so a mask can be
 * passed straight to `LibretroBridge.nativeSetInputMask`.
 */
object RetroPadButtons {
    const val B = 1 shl 0
    const val Y = 1 shl 1
    const val SELECT = 1 shl 2
    const val START = 1 shl 3
    const val UP = 1 shl 4
    const val DOWN = 1 shl 5
    const val LEFT = 1 shl 6
    const val RIGHT = 1 shl 7
    const val A = 1 shl 8
    const val X = 1 shl 9
    const val L = 1 shl 10
    const val R = 1 shl 11
    const val L2 = 1 shl 12
    const val R2 = 1 shl 13
    const val L3 = 1 shl 14
    const val R3 = 1 shl 15

    const val NONE = 0
    const val DPAD = UP or DOWN or LEFT or RIGHT
}
