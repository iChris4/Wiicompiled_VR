// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// The upper body the PC launcher draws below a Mii's head: the 3DS Mii bodies it carries
/// (mii_static_body_3ds_male_LE.rmdl and its female twin), each a shirt and a pair of trousers in
/// rio's little-endian model format. They are Nintendo's, so like the Mii parts they are
/// downloaded rather than shipped. A port of the Quest launcher's MiiBodies.kt.
final class MiiBodies: Sendable {
    /// One mesh, its own scale, rotation and translation already applied to its positions and
    /// normals as the PC applies them to each vertex. Odd meshes are the trousers.
    struct Mesh: Sendable {
        let positions: [Float]
        let normals: [Float]
        let texcoords: [Float]
        let indices: [Int]
        let pants: Bool
    }

    let male: [Mesh]
    let female: [Mesh]

    init(male: [Mesh], female: [Mesh]) {
        self.male = male
        self.female = female
    }

    /// The 3DS row of the PC's body_models.csv: the models' scale, and the head's height above their origin.
    static let modelScale: Float = 7
    static let headY: Float = 10.7766

    private static let headerSize = 0x20
    private static let meshSize = 0x38
    private static let vertexSize = 0x20

    static func load(male: URL, female: URL) throws -> MiiBodies {
        MiiBodies(male: try parse(Data(contentsOf: male)), female: try parse(Data(contentsOf: female)))
    }

    /// The meshes of a rio model, read as NativeMiiRenderer's TryReadRioModelBytes reads them.
    static func parse(_ data: Data) throws -> [Mesh] {
        try data.withUnsafeBytes { bytes -> [Mesh] in
            if bytes.count < headerSize || !bytes.prefix(8).elementsEqual("riomodel".utf8) {
                throw MiiError("Not a rio model.")
            }
            func u32(_ offset: Int) -> UInt32 {
                UInt32(bytes[offset]) | UInt32(bytes[offset + 1]) << 8 | UInt32(bytes[offset + 2]) << 16
                    | UInt32(bytes[offset + 3]) << 24
            }
            func i32(_ offset: Int) -> Int { Int(Int32(bitPattern: u32(offset))) }
            func f32(_ offset: Int) -> Float { Float(bitPattern: u32(offset)) }
            func vector(_ offset: Int) -> [Float] { [f32(offset), f32(offset + 4), f32(offset + 8)] }

            let declared = Int(u32(0x0C))
            if (1...Int(Int32.max)).contains(declared) && bytes.count != declared {
                throw MiiError("The model is \(bytes.count) bytes, not \(declared).")
            }
            let meshCount = Int(u32(0x14))
            if meshCount == 0 || meshCount > 1024 { throw MiiError("The model has \(meshCount) meshes.") }
            let list = 0x10 + i32(0x10)

            var meshes: [Mesh] = []
            for index in 0..<meshCount {
                let at = list + index * meshSize
                if at < 0 || at + meshSize > bytes.count { throw MiiError("Mesh \(index) is outside the model.") }
                let vertexCount = Int(u32(at + 0x04))
                let indexCount = Int(u32(at + 0x0C))
                if vertexCount == 0 || indexCount < 3 { continue }
                if vertexCount > 200_000 || indexCount > 2_000_000 { throw MiiError("Mesh \(index) is too large.") }
                let vertices = at + i32(at)
                let indices = at + 0x08 + i32(at + 0x08)
                if vertices < 0 || indices < 0 || vertices + vertexCount * vertexSize > bytes.count
                    || indices + indexCount * 4 > bytes.count {
                    throw MiiError("Mesh \(index)'s data is outside the model.")
                }
                // CreateBodyMeshSrt: scale, then rotation (radians), then translation.
                let srt = Mat4.scale(vector(at + 0x10)) * Mat4.rotation(vector(at + 0x1C)) * Mat4.translation(vector(at + 0x28))

                var positions = [Float](repeating: 0, count: vertexCount * 3)
                var normals = [Float](repeating: 0, count: vertexCount * 3)
                var texcoords = [Float](repeating: 0, count: vertexCount * 2)
                for i in 0..<vertexCount {
                    let vertex = vertices + i * vertexSize
                    let position = srt.transformPoint(f32(vertex), f32(vertex + 4), f32(vertex + 8))
                    positions[i * 3] = position.0
                    positions[i * 3 + 1] = position.1
                    positions[i * 3 + 2] = position.2
                    texcoords[i * 2] = f32(vertex + 0x0C)
                    texcoords[i * 2 + 1] = f32(vertex + 0x10)
                    let normal = srt.transformNormal(f32(vertex + 0x14), f32(vertex + 0x18), f32(vertex + 0x1C))
                    normals[i * 3] = normal.0
                    normals[i * 3 + 1] = normal.1
                    normals[i * 3 + 2] = normal.2
                }
                let order = try (0..<indexCount).map { i -> Int in
                    let value = Int(u32(indices + i * 4))
                    if value >= vertexCount { throw MiiError("Mesh \(index) names vertex \(value) of \(vertexCount).") }
                    return value
                }
                meshes.append(Mesh(positions: positions, normals: normals, texcoords: texcoords, indices: order,
                                   pants: index & 1 == 1))
            }
            if meshes.isEmpty { throw MiiError("The model has no meshes.") }
            return meshes
        }
    }
}
