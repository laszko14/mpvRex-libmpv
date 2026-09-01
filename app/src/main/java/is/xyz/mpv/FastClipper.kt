package `is`.xyz.mpv

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Fast and Frame-Accurate video clipping engine powered by FFmpeg & Android Hardware MediaCodec.
 */
object FastClipper {

    enum class ClipMode(val value: Int) {
        /** Fast lossless packet stream copy (-c copy), snaps to nearest keyframe. */
        FAST_COPY(0),
        /** Frame-accurate hardware-accelerated video export (starts on the exact frame). */
        FRAME_ACCURATE(1)
    }

    /**
     * Synchronously cuts and exports a video clip from [inputPath] to [outputPath].
     *
     * @param inputPath File path or descriptor (/proc/self/fd/...) to the input video.
     * @param outputPath Output file path destination.
     * @param startSec Start position in seconds.
     * @param endSec End position in seconds.
     * @param mode [ClipMode.FAST_COPY] for instant copy or [ClipMode.FRAME_ACCURATE] for exact frame cut.
     * @return [Result.success] on completion or [Result.failure] with error details.
     */
    @JvmStatic
    @JvmOverloads
    fun cutClip(
        inputPath: String,
        outputPath: String,
        startSec: Double,
        endSec: Double,
        mode: ClipMode = ClipMode.FAST_COPY
    ): Result<Unit> {
        if (inputPath.isBlank() || outputPath.isBlank()) {
            return Result.failure(IllegalArgumentException("Input or output path cannot be blank"))
        }
        if (endSec <= startSec) {
            return Result.failure(IllegalArgumentException("End time must be greater than start time"))
        }

        return try {
            val error = MPVLib.cutClipNative(inputPath, outputPath, startSec, endSec, mode.value)
            if (error == null) {
                Result.success(Unit)
            } else {
                Result.failure(Exception(error))
            }
        } catch (e: Throwable) {
            Result.failure(e)
        }
    }

    /**
     * Asynchronously cuts and exports a video clip on the IO dispatcher.
     *
     * @param inputPath File path or descriptor (/proc/self/fd/...) to the input video.
     * @param outputPath Output file path destination.
     * @param startSec Start position in seconds.
     * @param endSec End position in seconds.
     * @param mode [ClipMode.FAST_COPY] or [ClipMode.FRAME_ACCURATE].
     * @return [Result.success] on completion or [Result.failure] with error details.
     */
    suspend fun cutClipAsync(
        inputPath: String,
        outputPath: String,
        startSec: Double,
        endSec: Double,
        mode: ClipMode = ClipMode.FAST_COPY
    ): Result<Unit> = withContext(Dispatchers.IO) {
        cutClip(inputPath, outputPath, startSec, endSec, mode)
    }
}
