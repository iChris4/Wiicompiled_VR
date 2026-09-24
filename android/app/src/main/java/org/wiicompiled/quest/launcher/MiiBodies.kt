package org.wiicompiled.quest.launcher

import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The upper body the PC launcher draws below a Mii's head: the 3DS Mii bodies it carries
 * (mii_static_body_3ds_male_LE.rmdl and its female twin), each a shirt and a pair of trousers in
 * rio's little-endian model format. They are Nintendo's, so like the Mii parts they are
 * downloaded ([MiiRenderResource]) rather than shipped.
 */
class MiiBodies(val male: List<Mesh>, val female: List<Mesh>) {

    /**
     * One mesh, its own scale, rotation and translation already applied to its positions and
     * normals as the PC applies them to each vertex. Odd meshes are the trousers.
     */
    class Mesh(val positions: FloatArray, val normals: FloatArray, val texcoords: FloatArray, val indices: IntArray, val pants: Boolean)

    companion object {
        /** The 3DS row of the PC's body_models.csv: the models' scale, and the head's height above their origin. */
        const val MODEL_SCALE = 7f
        const val HEAD_Y = 10.7766f

        private const val HEADER_SIZE = 0x20
        private const val MESH_SIZE = 0x38
        private const val VERTEX_SIZE = 0x20

        @Throws(IOException::class)
        fun load(male: File, female: File) = MiiBodies(parse(male.readBytes()), parse(female.readBytes()))

        /** The meshes of a rio model, read as NativeMiiRenderer's TryReadRioModelBytes reads them. */
        @Throws(IOException::class)
        fun parse(bytes: ByteArray): List<Mesh> {
            if (bytes.size < HEADER_SIZE || String(bytes, 0, 8, Charsets.US_ASCII) != "riomodel") {
                throw IOException("Not a rio model.")
            }
            val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            val declared = buffer.getInt(0x0C).toLong() and 0xFFFFFFFFL
            if (declared in 1..Int.MAX_VALUE && bytes.size.toLong() != declared) {
                throw IOException("The model is ${bytes.size} bytes, not $declared.")
            }
            val meshCount = buffer.getInt(0x14).toLong() and 0xFFFFFFFFL
            if (meshCount == 0L || meshCount > 1024) throw IOException("The model has $meshCount meshes.")
            val list = 0x10L + buffer.getInt(0x10)

            val meshes = ArrayList<Mesh>()
            for (index in 0 until meshCount.toInt()) {
                val at = list + index.toLong() * MESH_SIZE
                if (at < 0 || at + MESH_SIZE > bytes.size) throw IOException("Mesh $index is outside the model.")
                val mesh = at.toInt()
                val vertexCount = buffer.getInt(mesh + 0x04).toLong() and 0xFFFFFFFFL
                val indexCount = buffer.getInt(mesh + 0x0C).toLong() and 0xFFFFFFFFL
                if (vertexCount == 0L || indexCount < 3) continue
                if (vertexCount > 200_000 || indexCount > 2_000_000) throw IOException("Mesh $index is too large.")
                val vertices = at + buffer.getInt(mesh)
                val indices = at + 0x08 + buffer.getInt(mesh + 0x08)
                if (vertices < 0 || indices < 0 || vertices + vertexCount * VERTEX_SIZE > bytes.size || indices + indexCount * 4 > bytes.size) {
                    throw IOException("Mesh $index's data is outside the model.")
                }
                fun vector(offset: Int) = floatArrayOf(buffer.getFloat(offset), buffer.getFloat(offset + 4), buffer.getFloat(offset + 8))
                // CreateBodyMeshSrt: scale, then rotation (radians), then translation.
                val srt = Mat4.scale(vector(mesh + 0x10)) * Mat4.rotation(vector(mesh + 0x1C)) * Mat4.translation(vector(mesh + 0x28))

                val count = vertexCount.toInt()
                val positions = FloatArray(count * 3)
                val normals = FloatArray(count * 3)
                val texcoords = FloatArray(count * 2)
                for (i in 0 until count) {
                    val vertex = vertices.toInt() + i * VERTEX_SIZE
                    srt.transformPoint(vector(vertex), 0, positions, i * 3)
                    texcoords[i * 2] = buffer.getFloat(vertex + 0x0C)
                    texcoords[i * 2 + 1] = buffer.getFloat(vertex + 0x10)
                    srt.transformNormal(vector(vertex + 0x14), 0, normals, i * 3)
                }
                val order = IntArray(indexCount.toInt()) { i ->
                    val value = buffer.getInt(indices.toInt() + i * 4).toLong() and 0xFFFFFFFFL
                    if (value >= vertexCount) throw IOException("Mesh $index names vertex $value of $vertexCount.")
                    value.toInt()
                }
                meshes += Mesh(positions, normals, texcoords, order, pants = (index and 1) == 1)
            }
            if (meshes.isEmpty()) throw IOException("The model has no meshes.")
            return meshes
        }
    }
}
