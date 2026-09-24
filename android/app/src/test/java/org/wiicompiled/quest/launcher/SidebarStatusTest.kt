package org.wiicompiled.quest.launcher

import java.io.IOException
import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** Wheel Wizard's status is read as the PC reads status.json, and its own icon as Avalonia parses path data. */
class SidebarStatusTest {

    @Test
    fun theStatusIsReadAsThePcReadsIt() {
        // status.json as the Wheel Wizard team publishes it.
        val live = WheelWizardStatus.parse(
            """{"message" : "Unstable servers!", "variant": "warning",
               "we_dont_actually_parse_this_key___all_posible_variants_here" : ["none", "warning", "error"]}""",
        )
        assertEquals(WheelWizardStatus.Variant.Warning, live.variant)
        assertEquals("Unstable servers!", live.message)
        assertTrue(live.visible)

        assertFalse(WheelWizardStatus.parse("""{"message": "All good", "variant": "None"}""").visible)
        val custom = WheelWizardStatus.parse("""{"message": "Party!", "icon": "M0 0L10 10Z", "color": "#123456"}""")
        assertNull(custom.variant)
        assertTrue(custom.visible)
        assertEquals("#123456", custom.color)

        // No message, or a variant the PC does not know, is no status at all.
        for (json in listOf("""{"variant": "warning"}""", """{"message": "x", "variant": "panic"}""", "not json")) {
            try {
                WheelWizardStatus.parse(json)
                fail("Read $json")
            } catch (e: IOException) {
                // As expected.
            }
        }
    }

    @Test
    fun pathDataIsReadWithEveryCommand() {
        val segments = SvgPath.parse("M10 20h5v-5l5,5L30 30c1 1 2 2 3 3s4 4 5 5Q40 40 50 50t10 10z m1 1")
        assertEquals(SvgPath.Segment.Move(10f, 20f), segments[0])
        assertEquals(SvgPath.Segment.Line(15f, 20f), segments[1])
        assertEquals(SvgPath.Segment.Line(15f, 15f), segments[2])
        assertEquals(SvgPath.Segment.Line(20f, 20f), segments[3])
        assertEquals(SvgPath.Segment.Line(30f, 30f), segments[4])
        assertEquals(SvgPath.Segment.Cubic(31f, 31f, 32f, 32f, 33f, 33f), segments[5])
        // S mirrors the last control point about the current point.
        assertEquals(SvgPath.Segment.Cubic(34f, 34f, 37f, 37f, 38f, 38f), segments[6])
        assertEquals(SvgPath.Segment.Quad(40f, 40f, 50f, 50f), segments[7])
        assertEquals(SvgPath.Segment.Quad(60f, 60f, 60f, 60f), segments[8])
        assertEquals(SvgPath.Segment.Close, segments[9])
        // After Z the pen is back at the start of the figure.
        assertEquals(SvgPath.Segment.Move(11f, 21f), segments[10])
    }

    @Test
    fun numbersAndArcFlagsMayRunTogether() {
        // As Font Awesome writes them: ".5.5" is two numbers, "-1-2" too, and flags need no spaces.
        val segments = SvgPath.parse("M.5.5L-1-2a10 10 0 0110 10")
        assertEquals(SvgPath.Segment.Move(0.5f, 0.5f), segments[0])
        assertEquals(SvgPath.Segment.Line(-1f, -2f), segments[1])
        val end = segments.last() as SvgPath.Segment.Cubic
        assertEquals(9f, end.x, 1e-3f)
        assertEquals(8f, end.y, 1e-3f)
        assertEquals(1e3f, (SvgPath.parse("M1e3 0")[0] as SvgPath.Segment.Move).x)
    }

    @Test
    fun arcsBecomeCurvesAlongTheCircle() {
        // A half circle of radius 10 from (0, 0) to (20, 0), passing below through (10, 10).
        val curves = SvgPath.parse("M0 0A10 10 0 0 0 20 0").drop(1).map { it as SvgPath.Segment.Cubic }
        assertEquals(2, curves.size)
        assertEquals(10f, curves[0].x, 1e-3f)
        assertEquals(10f, curves[0].y, 1e-3f)
        assertEquals(20f, curves[1].x, 1e-3f)
        assertEquals(0f, curves[1].y, 1e-3f)
        // Control points of a quarter circle sit 0.5523 of the radius along the tangents.
        assertTrue(abs(curves[0].y1 - 5.523f) < 1e-2f)
        // A radius too small is grown to reach the end.
        val grown = SvgPath.parse("M0 0A1 1 0 0 1 20 0").last() as SvgPath.Segment.Cubic
        assertEquals(20f, grown.x, 1e-3f)
    }

    @Test
    fun badPathDataIsRefused() {
        for (data in listOf("10 10", "M0 0 A5 5 0 2 0 5 5", "M0 0 X1", "M0")) {
            try {
                SvgPath.parse(data)
                fail("Read $data")
            } catch (e: IllegalArgumentException) {
                // As expected.
            }
        }
    }
}
