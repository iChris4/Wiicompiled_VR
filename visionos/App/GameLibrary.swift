// SPDX-License-Identifier: GPL-3.0-or-later

import Darwin
import Foundation

/// The game runtime, an embedded framework (runtime/cmake/PublicProducts.cmake) loaded with
/// dlopen when the player presses Play, and its C bridge
/// (runtime/include/platform/visionos/visionos_host.h) looked up by name.
///
/// It is loaded late on purpose, as the Quest's launcher loads libmain.so: dozens of the
/// runtime's globals read Config.toml in their static initialisers, which run the moment the
/// framework is loaded, so loading it after the launcher's settings page wrote the file is what
/// makes those settings count.
final class GameLibrary: @unchecked Sendable {
    /// Set once, on the main actor, before the immersive space opens; read from any thread after.
    nonisolated(unsafe) static var shared: GameLibrary?

    private typealias SetDirectories = @convention(c) (UnsafePointer<CChar>?, UnsafePointer<CChar>?) -> Void
    private typealias SetPointer = @convention(c) (UnsafeMutableRawPointer?) -> Void
    private typealias SpatialEvent = @convention(c) (UInt64, Int32, Int32, Bool, Float, Float, Float, Float, Float, Float, Bool, Float, Float, Float) -> Void
    private typealias BoolFunction = @convention(c) () -> Bool
    private typealias IntFunction = @convention(c) () -> Int32
    private typealias VoidFunction = @convention(c) () -> Void
    private typealias StringFunction = @convention(c) () -> UnsafePointer<CChar>?

    private let handle: UnsafeMutableRawPointer
    private let setDirectoriesFunction: SetDirectories
    private let setLayerRendererFunction: SetPointer
    private let spatialEventFunction: SpatialEvent
    private let layerInvalidatedFunction: BoolFunction
    private let startGameFunction: BoolFunction
    private let gameRunningFunction: BoolFunction
    private let exitCodeFunction: IntFunction
    private let requestQuitFunction: VoidFunction
    private let lastErrorFunction: StringFunction

    enum LoadError: LocalizedError {
        case missing(String)
        case dlopen(String)
        case symbol(String)
        case otherGameLoaded

        var errorDescription: String? {
            switch self {
            case .missing(let path): return "The game is missing from the app (\(path))."
            case .dlopen(let message): return "The game could not be loaded: \(message)"
            case .symbol(let name): return "The game is incomplete (no \(name))."
            case .otherGameLoaded: return "The other game is already loaded. Relaunch the app to switch games."
            }
        }
    }

    /// The framework this library came from (GameChoice.frameworkName).
    let frameworkName: String

    /// Whether the app embeds the game's framework (visionos/CMakeLists.txt embeds one per
    /// translated product).
    static func isEmbedded(_ frameworkName: String) -> Bool {
        guard let binary = binaryURL(frameworkName) else { return false }
        return FileManager.default.fileExists(atPath: binary.path)
    }

    private static func binaryURL(_ frameworkName: String) -> URL? {
        Bundle.main.privateFrameworksURL?.appendingPathComponent("\(frameworkName).framework/\(frameworkName)")
    }

    /// Loads one game. Both frameworks define the same runtime, so a process holds one: once
    /// loaded, the other game needs a relaunch (as a second run of the same one does).
    static func load(_ frameworkName: String) throws -> GameLibrary {
        if let shared {
            guard shared.frameworkName == frameworkName else { throw LoadError.otherGameLoaded }
            return shared
        }
        guard let binary = binaryURL(frameworkName) else {
            throw LoadError.missing("Frameworks")
        }
        guard FileManager.default.fileExists(atPath: binary.path) else {
            throw LoadError.missing(binary.lastPathComponent)
        }
        guard let handle = dlopen(binary.path, RTLD_NOW | RTLD_GLOBAL) else {
            throw LoadError.dlopen(String(cString: dlerror()))
        }
        let library = try GameLibrary(handle: handle, frameworkName: frameworkName)
        shared = library
        return library
    }

    private init(handle: UnsafeMutableRawPointer, frameworkName: String) throws {
        self.handle = handle
        self.frameworkName = frameworkName
        func symbol<T>(_ name: String, as type: T.Type) throws -> T {
            guard let address = dlsym(handle, name) else { throw LoadError.symbol(name) }
            return unsafeBitCast(address, to: type)
        }
        setDirectoriesFunction = try symbol("mkw_visionos_set_directories", as: SetDirectories.self)
        setLayerRendererFunction = try symbol("mkw_visionos_set_layer_renderer", as: SetPointer.self)
        spatialEventFunction = try symbol("mkw_visionos_spatial_event", as: SpatialEvent.self)
        layerInvalidatedFunction = try symbol("mkw_visionos_layer_invalidated", as: BoolFunction.self)
        startGameFunction = try symbol("mkw_visionos_start_game", as: BoolFunction.self)
        gameRunningFunction = try symbol("mkw_visionos_game_running", as: BoolFunction.self)
        exitCodeFunction = try symbol("mkw_visionos_exit_code", as: IntFunction.self)
        requestQuitFunction = try symbol("mkw_visionos_request_quit", as: VoidFunction.self)
        lastErrorFunction = try symbol("mkw_visionos_last_error", as: StringFunction.self)
    }

    func setDirectories(data: String, resources: String) {
        setDirectoriesFunction(data, resources)
    }

    func setLayerRenderer(_ renderer: UnsafeMutableRawPointer) { setLayerRendererFunction(renderer) }

    func spatialEvent(id: UInt64, phase: Int32, chirality: Int32, ray: (origin: SIMD3<Float>, direction: SIMD3<Float>)?,
                      pose: SIMD3<Float>?) {
        let origin = ray?.origin ?? .zero
        let direction = ray?.direction ?? .zero
        let position = pose ?? .zero
        spatialEventFunction(id, phase, chirality, ray != nil, origin.x, origin.y, origin.z,
                             direction.x, direction.y, direction.z, pose != nil, position.x, position.y, position.z)
    }

    var layerInvalidated: Bool { layerInvalidatedFunction() }
    func startGame() -> Bool { startGameFunction() }
    var gameRunning: Bool { gameRunningFunction() }
    var exitCode: Int32 { exitCodeFunction() }
    func requestQuit() { requestQuitFunction() }
    var lastError: String { lastErrorFunction().map { String(cString: $0) } ?? "" }
}
