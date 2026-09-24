package org.wiicompiled.quest.launcher

import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

/** One mesh of a made-up rio model: its vertices (x, y, z, all facing +Z), triangles and own placement. */
internal class RioMesh(
    val vertices: List<FloatArray>,
    val indices: IntArray,
    val scale: FloatArray = floatArrayOf(1f, 1f, 1f),
    val rotate: FloatArray = FloatArray(3),
    val translate: FloatArray = FloatArray(3),
)

/** The rectangle [left]..[right] by [bottom]..[top] at depth [z], facing +Z. */
internal fun quad(left: Float, right: Float, bottom: Float, top: Float, z: Float = 0f) = RioMesh(
    listOf(floatArrayOf(left, top, z), floatArrayOf(right, top, z), floatArrayOf(left, bottom, z), floatArrayOf(right, bottom, z)),
    // Counter-clockwise on screen, so back-face culling keeps them.
    intArrayOf(0, 2, 1, 1, 2, 3),
)

/**
 * A little-endian "riomodel" holding [meshes], laid out as the PC's reader expects: the header,
 * the 0x38-byte mesh records, then each mesh's 0x20-byte vertices and 32-bit indices. The real
 * bodies are Nintendo's and never in this repository.
 */
internal fun rioModel(vararg meshes: RioMesh): ByteArray {
    val records = 0x20
    var size = records + meshes.size * 0x38
    for (mesh in meshes) size += mesh.vertices.size * 0x20 + mesh.indices.size * 4
    val buffer = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
    buffer.put("riomodel".toByteArray(Charsets.US_ASCII))
    buffer.putInt(0x08, 1)
    buffer.putInt(0x0C, size)
    // The mesh list, from the field's own offset.
    buffer.putInt(0x10, records - 0x10)
    buffer.putInt(0x14, meshes.size)
    var data = records + meshes.size * 0x38
    for ((index, mesh) in meshes.withIndex()) {
        val record = records + index * 0x38
        buffer.putInt(record, data - record)
        buffer.putInt(record + 0x04, mesh.vertices.size)
        for ((i, vertex) in mesh.vertices.withIndex()) {
            val at = data + i * 0x20
            for (c in 0 until 3) buffer.putFloat(at + c * 4, vertex[c])
            buffer.putFloat(at + 0x1C, 1f)
        }
        data += mesh.vertices.size * 0x20
        buffer.putInt(record + 0x08, data - (record + 0x08))
        buffer.putInt(record + 0x0C, mesh.indices.size)
        for ((i, value) in mesh.indices.withIndex()) buffer.putInt(data + i * 4, value)
        data += mesh.indices.size * 4
        for (c in 0 until 3) {
            buffer.putFloat(record + 0x10 + c * 4, mesh.scale[c])
            buffer.putFloat(record + 0x1C + c * 4, mesh.rotate[c])
            buffer.putFloat(record + 0x28 + c * 4, mesh.translate[c])
        }
    }
    return buffer.array()
}

/** The body models are read as the PC's TryReadRioModelBytes reads them. */
class MiiBodiesTest {

    @Test
    fun readsMeshesPlacedAndTheOddOnesAsTrousers() {
        val shirt = quad(-1f, 1f, 0f, 2f).let {
            RioMesh(it.vertices, it.indices, scale = floatArrayOf(2f, 3f, 1f), translate = floatArrayOf(0f, 10f, 0f))
        }
        val turned = quad(-1f, 1f, 0f, 2f).let { RioMesh(it.vertices, it.indices, rotate = floatArrayOf(0f, (Math.PI / 2).toFloat(), 0f)) }
        val empty = RioMesh(emptyList(), IntArray(0))
        val meshes = MiiBodies.parse(rioModel(shirt, quad(-1f, 1f, -2f, 0f), empty, turned))

        // The empty mesh is left out, but still counts: the last one is odd, so trousers.
        assertEquals(listOf(false, true, true), meshes.map { it.pants })
        // Scaled, then moved: the top left corner (-1, 2) is at (-2, 16).
        assertArrayEquals(floatArrayOf(-2f, 16f, 0f), meshes[0].positions.copyOfRange(0, 3), 1e-6f)
        assertArrayEquals(intArrayOf(0, 2, 1, 1, 2, 3), meshes[0].indices)
        assertArrayEquals(FloatArray(8), meshes[0].texcoords, 0f)
        // A normal turns with its mesh but never moves: +Z turned a quarter about Y is +X.
        assertArrayEquals(floatArrayOf(0f, 0f, 1f), meshes[0].normals.copyOfRange(0, 3), 1e-6f)
        assertArrayEquals(floatArrayOf(1f, 0f, 0f), meshes[2].normals.copyOfRange(0, 3), 1e-6f)
    }

    @Test
    fun refusesWhatIsNotABodyModel() {
        val good = rioModel(quad(-1f, 1f, 0f, 2f))
        MiiBodies.parse(good)
        assertThrows(IOException::class.java) { MiiBodies.parse(good.copyOf().also { it[0] = 'X'.code.toByte() }) }
        // Not the size its header says.
        assertThrows(IOException::class.java) { MiiBodies.parse(good + 0.toByte()) }
        // A triangle naming a fifth vertex of four.
        assertThrows(IOException::class.java) { MiiBodies.parse(rioModel(RioMesh(quad(-1f, 1f, 0f, 2f).vertices, intArrayOf(0, 1, 4)))) }
        // Nothing drawable at all.
        assertThrows(IOException::class.java) { MiiBodies.parse(rioModel(RioMesh(emptyList(), IntArray(0)))) }
        // More meshes than the file holds.
        assertThrows(IOException::class.java) {
            MiiBodies.parse(good.copyOf().also { ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putInt(0x14, 40) })
        }
    }
}
