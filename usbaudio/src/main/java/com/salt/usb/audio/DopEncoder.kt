// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 三水深
package com.salt.usb.audio

/**
 * DSD over PCM (DoP) Encoder.
 *
 * Encapsulates DSD data within a PCM stream so that DSD-capable DACs
 * can recognize and decode it. The DAC detects DoP by the marker byte pattern.
 *
 * DoP format (per channel, per sample period):
 *   Byte 0: Marker (0x05 for DSD, 0xFA for DSD inverted)
 *   Byte 1: DSD data byte
 *   Byte 2: DSD data byte  (for 24-bit container)
 *   Or for 32-bit:
 *   Byte 0: DSD data
 *   Byte 1: DSD data
 *   Byte 2: DSD data
 *   Byte 3: Marker (0x05)
 *
 * DSD64  -> 176.4 kHz / 24-bit or 32-bit PCM container
 * DSD128 -> 352.8 kHz / 24-bit or 32-bit PCM container
 * DSD256 -> 705.6 kHz / 24-bit or 32-bit PCM container
 */
object DopEncoder {

    /** DoP marker byte */
    private const val DOP_MARKER: Byte = 0x05

    /**
     * Convert DSD sample rate to the equivalent PCM carrier rate for DoP.
     *
     * @param dsdRate DSD sample rate (e.g., 2822400 for DSD64)
     * @param bitDepth DoP container bit depth (24 or 32)
     * @return PCM carrier sample rate, or -1 if invalid
     */
    fun getCarrierSampleRate(dsdRate: Int, bitDepth: Int = 32): Int {
        // DSD data bytes per sample per channel
        val dsdBytesPerSample = when (bitDepth) {
            24 -> 2  // marker + 2 DSD bytes
            32 -> 2  // 2 DSD bytes + marker (or varies by implementation)
            else -> return -1
        }

        // DSD64 = 2822400 Hz = 64 * 44100
        // Each PCM sample carries `dsdBytesPerSample` DSD bytes = `dsdBytesPerSample * 8` DSD bits
        val pcmRate = dsdRate / (dsdBytesPerSample * 8)
        return if (pcmRate > 0) pcmRate else -1
    }

    /**
     * Get standard DSD rate constants.
     */
    fun getDsdRate(dsdVersion: Int): Int = when (dsdVersion) {
        64  -> 2822400   // 64 * 44100
        128 -> 5644800   // 128 * 44100
        256 -> 11289600  // 256 * 44100
        512 -> 22579200  // 512 * 44100
        else -> -1
    }

    /**
     * Get the PCM carrier rate for a given DSD version.
     */
    fun getCarrierRateForDsd(dsdVersion: Int): Int = when (dsdVersion) {
        64  -> 176400   // 44100 * 4
        128 -> 352800   // 44100 * 8
        256 -> 705600   // 44100 * 16 (rare, needs high-speed USB)
        else -> -1
    }

    /**
     * Encode DSD data into DoP (DSD over PCM) format.
     *
     * @param dsdData      Raw DSD data (1 bit per sample, MSB first)
     * @param channels     Number of channels (1 or 2)
     * @param bitDepth     Container bit depth (24 or 32)
     * @return             DoP-encoded PCM data
     */
    fun encodeDoP(dsdData: ByteArray, channels: Int, bitDepth: Int = 32): ByteArray {
        require(channels == 1 || channels == 2) { "DoP supports 1 or 2 channels" }
        require(bitDepth == 24 || bitDepth == 32) { "DoP container must be 24 or 32 bit" }

        val bytesPerChannel = bitDepth / 8
        val dsdBytesPerSample = bytesPerChannel - 1  // One byte is the marker
        val totalDsdBytesPerFrame = dsdBytesPerSample * channels

        // Each output frame needs totalDsdBytesPerFrame DSD bytes
        val numFrames = dsdData.size / totalDsdBytesPerFrame
        val outputSize = numFrames * bytesPerChannel * channels
        val output = ByteArray(outputSize)

        var dsdPos = 0
        var outPos = 0

        for (frame in 0 until numFrames) {
            for (ch in 0 until channels) {
                when (bitDepth) {
                    24 -> {
                        // 24-bit DoP: [Marker] [DSD0] [DSD1] (3 bytes per channel)
                        output[outPos++] = DOP_MARKER
                        output[outPos++] = dsdData[dsdPos++]
                        output[outPos++] = dsdData[dsdPos++]
                    }
                    32 -> {
                        // 32-bit DoP: [DSD0] [DSD1] [0x00] [Marker] (4 bytes per channel)
                        output[outPos++] = dsdData[dsdPos++]
                        output[outPos++] = dsdData[dsdPos++]
                        output[outPos++] = 0x00
                        output[outPos++] = DOP_MARKER
                    }
                }
            }
        }

        return output
    }

    /**
     * Check if a given PCM sample rate could be a DoP carrier rate.
     */
    fun isCarrierRate(sampleRate: Int): Boolean = when (sampleRate) {
        176400, 352800, 705600 -> true  // Standard DoP carrier rates
        else -> false
    }

    /**
     * Detect DSD version from carrier rate.
     */
    fun detectDsdVersion(carrierRate: Int): Int = when (carrierRate) {
        176400 -> 64
        352800 -> 128
        705600 -> 256
        else -> 0
    }
}

/**
 * Audio format information for USB exclusive mode.
 */
data class UsbAudioFormat(
    val sampleRate: Int,
    val bitDepth: Int,
    val channels: Int,
    val isDsd: Boolean = false,
    val dsdVersion: Int = 0  // 64, 128, 256, 512
) {
    /**
     * Get the byte size per frame (all channels).
     */
    val bytesPerFrame: Int
        get() = (bitDepth / 8) * channels

    /**
     * Get the data rate in bytes per second.
     */
    val bytesPerSecond: Int
        get() = sampleRate * bytesPerFrame

    /**
     * Get display-friendly format string.
     */
    val displayString: String
        get() = if (isDsd) {
            "DSD${dsdVersion}"
        } else {
            "${sampleRate / 1000}kHz / ${bitDepth}bit / ${if (channels == 2) "Stereo" else "Mono"}"
        }

    companion object {
        /** Common PCM formats */
        val CD_QUALITY = UsbAudioFormat(44100, 16, 2)
        val HI_RES_48K = UsbAudioFormat(48000, 24, 2)
        val HI_RES_96K = UsbAudioFormat(96000, 24, 2)
        val HI_RES_192K = UsbAudioFormat(192000, 24, 2)
        val HI_RES_352K = UsbAudioFormat(352800, 32, 2)
        val HI_RES_384K = UsbAudioFormat(384000, 32, 2)

        /** DSD formats */
        val DSD64 = UsbAudioFormat(176400, 24, 2, true, 64)
        val DSD128 = UsbAudioFormat(352800, 24, 2, true, 128)

        /** All commonly supported formats */
        val COMMON_FORMATS = listOf(
            CD_QUALITY,
            HI_RES_48K,
            HI_RES_96K,
            HI_RES_192K,
            HI_RES_384K,
            DSD64,
            DSD128
        )
    }
}
