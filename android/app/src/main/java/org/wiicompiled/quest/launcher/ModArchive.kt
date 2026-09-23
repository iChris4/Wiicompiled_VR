package org.wiicompiled.quest.launcher

import java.io.IOException
import java.io.InterruptedIOException

/**
 * The .7z and .rar mod archives Android cannot open itself, unpacked by android/nod-jni
 * (src/archive.rs: sevenz-rust2 and rars, both checking each file's CRC or hash). Only regular
 * files are written, all below the destination. Blocks, so callers run it off the main thread.
 */
object ModArchive {

    init {
        System.loadLibrary("nod_jni")
    }

    fun interface Listener {
        /** Bytes written so far out of [total]; return false to cancel. */
        fun update(done: Long, total: Long): Boolean
    }

    /**
     * Unpacks [archive], a 7z or RAR file, into the existing [outDir] and returns how many files
     * it wrote. Throws [InterruptedIOException] when [listener] cancels.
     */
    @Throws(IOException::class)
    external fun extract(archive: String, outDir: String, listener: Listener): Long
}
