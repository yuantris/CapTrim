package io.captrim

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure-JVM checks. Native integration (JNI + .so loading) belongs to
 * androidTest on a device/emulator, not to the host unit test task.
 */
class CapTrimExceptionTest {

    @Test
    fun `error carries the native status code`() {
        val error = CapTrimException(-4, "backend unavailable")
        assertEquals(-4, error.code)
        assertTrue(error.message!!.contains("backend unavailable"))
        assertTrue(error.message!!.contains("code=-4"))
    }
}
