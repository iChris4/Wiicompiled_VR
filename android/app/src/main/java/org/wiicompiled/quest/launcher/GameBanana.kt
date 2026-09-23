package org.wiicompiled.quest.launcher

import java.io.File
import java.io.IOException
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.security.MessageDigest
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import org.wiicompiled.quest.BuildConfig

/**
 * GameBanana, where WheelWizard's mod browser finds Mario Kart Wii mods (Features/GameBanana): the
 * same two public API calls, a name search one page at a time and one mod's profile page with its
 * files, and the download of one of those files. Parsing is kept apart from the network so it can
 * be tested on its own.
 *
 * Values are read with plain `opt` and type checks: Android's org.json and the JVM one the unit
 * tests run against disagree on JSON nulls (Android's optString gives "null").
 */
object GameBanana {

    private const val API = "https://gamebanana.com/apiv12"
    /** Mario Kart Wii on GameBanana (GameBananaSingletonService.MkGameId). */
    private const val GAME_ID = 5896
    /** What an empty search asks for, so the list opens on something (WheelWizard's own "featured" list). */
    private const val DEFAULT_SEARCH = "Mod"
    /** What an empty search asks for with Patches only on (ModBrowserWindow.GetEffectiveSearchTerm). */
    private const val PATCHES_SEARCH = "Patches"
    /** GameBanana refuses shorter search strings. */
    const val MIN_SEARCH_LENGTH = 2

    private const val TIMEOUT_MS = 30_000
    private const val DOWNLOAD_ATTEMPTS = 3
    private const val DOWNLOAD_BUFFER = 64 * 1024

    private val userAgent: String get() = "WiiCompiledVR-Quest/${BuildConfig.VERSION_NAME}"

    /** One preview picture, in the sizes GameBanana keeps of it. */
    data class Image(val baseUrl: String, val file: String, val file220: String?, val file530: String?) {
        /** About 220 pixels wide, as much as a list thumbnail shows. */
        val small: String get() = "$baseUrl/${file220 ?: file}"
        /** About 530 pixels wide, for the banner and the gallery. */
        val medium: String get() = "$baseUrl/${file530 ?: file}"
    }

    /** One search result: what WheelWizard's ModBrowserListItem shows of GameBananaModPreview. */
    data class Preview(
        val id: Int,
        val name: String,
        val author: String,
        val likes: Long,
        val views: Long,
        val usesPatches: Boolean,
        val image: Image?,
    )

    /** One page of results, and whether it is the last. */
    data class Page(val mods: List<Preview>, val complete: Boolean)

    /** One downloadable file of a mod (GameBananaModFiles), with what to check the download against. */
    data class ModFile(val name: String, val size: Long, val url: String, val md5: String?, val description: String)

    /** A mod's profile page (GameBananaModDetails), as the details pane shows it. */
    data class Details(
        val id: Int,
        val name: String,
        val author: String,
        val authorUrl: String?,
        val profileUrl: String?,
        val likes: Long,
        val views: Long,
        val downloads: Long,
        val text: String,
        val images: List<Image>,
        val files: List<ModFile>,
    ) {
        /** Where ModContent's Report link goes. */
        val reportUrl: String get() = "https://gamebanana.com/support/add?s=Mod.$id"
    }

    /** Thrown when a download's progress callback asks to stop; a socket timeout is not this. */
    class Cancelled : InterruptedIOException("Download cancelled")

    // Requests

    /** What GameBanana is asked for: an empty search still lists something. */
    fun effectiveSearch(term: String, patchesOnly: Boolean): String = when {
        term.isNotBlank() -> term.trim()
        patchesOnly -> PATCHES_SEARCH
        else -> DEFAULT_SEARCH
    }

    fun searchUrl(term: String, page: Int): String =
        "$API/Util/Search/Results?_sSearchString=${encode(term)}&_idGameRow=$GAME_ID&_sModelName=Mod&_nPage=$page"

    fun detailsUrl(modId: Int): String = "$API/Mod/$modId/ProfilePage"

    private fun encode(text: String): String = URLEncoder.encode(text, "UTF-8").replace("+", "%20")

    // Parsing

    /**
     * A search page as the browser lists it: only mods, and none with content ratings, which
     * WheelWizard's browser leaves out as well.
     */
    fun parsePage(json: String): Page = parsing {
        val root = JSONObject(json)
        val records = root.array("_aRecords") ?: throw IOException("GameBanana sent no search results.")
        val mods = records.objects()
            .filter { it.text("_sModelName") == "Mod" && !it.flag("_bHasContentRatings") }
            .mapNotNull { record ->
                Preview(
                    id = record.number("_idRow").toInt().takeIf { it > 0 } ?: return@mapNotNull null,
                    name = record.text("_sName") ?: return@mapNotNull null,
                    author = record.obj("_aSubmitter")?.text("_sName").orEmpty(),
                    likes = record.number("_nLikeCount"),
                    views = record.number("_nViewCount"),
                    usesPatches = usesPatches(tagTitles(record.opt("_aTags"))),
                    image = images(record).firstOrNull(),
                )
            }
        // Without the flag there is no way to ask for more, so the list stops rather than repeating.
        Page(mods, complete = root.obj("_aMetadata")?.opt("_bIsComplete") as? Boolean ?: true)
    }

    /**
     * A mod's profile page. Its files are the current ones, or the archived ones when it has none.
     * A private, removed or withheld mod, which searches can still list, only has a stub of a
     * page; it is refused with the reason, where WheelWizard fails to read it at all.
     */
    fun parseDetails(json: String): Details = parsing {
        val root = JSONObject(json)
        when {
            root.flag("_bIsTrashed") -> throw IOException("GameBanana has removed this mod.")
            root.flag("_bIsWithheld") -> throw IOException("GameBanana is withholding this mod for now.")
            root.flag("_bIsPrivate") -> throw IOException("This mod is private on GameBanana, so its page and files cannot be shown.")
        }
        val submitter = root.obj("_aSubmitter")
        Details(
            id = root.number("_idRow").toInt(),
            name = root.text("_sName") ?: throw IOException("GameBanana sent a mod without a name."),
            author = submitter?.text("_sName").orEmpty(),
            authorUrl = submitter?.text("_sProfileUrl"),
            profileUrl = root.text("_sProfileUrl"),
            likes = root.number("_nLikeCount"),
            views = root.number("_nViewCount"),
            downloads = root.number("_nDownloadCount"),
            text = root.text("_sText").orEmpty(),
            images = images(root),
            files = files(root.array("_aFiles")).ifEmpty { files(root.array("_aArchivedFiles")) },
        )
    }

    /**
     * GameBananaModPreview.UsesPatches: a tag titled Patch or Patches. Search results give tags
     * as "Title: Value" strings ("Patches: True"), profile pages as objects.
     */
    fun usesPatches(tagTitles: List<String>): Boolean = tagTitles.any { tag ->
        val title = tag.trim().substringBefore(':').trim()
        title.equals("patch", ignoreCase = true) || title.equals("patches", ignoreCase = true)
    }

    /** The reason GameBanana gives when it refuses a request, such as a search string that is too short. */
    fun errorMessage(json: String): String? = try {
        val root = JSONObject(json)
        val fields = root.obj("_aErrorData")
        fields?.keys()?.asSequence()?.mapNotNull { fields.obj(it)?.text("_sErrorMessage") }?.firstOrNull()
            ?: root.text("_sErrorMessage")
            ?: root.text("_sErrorCode")
    } catch (e: JSONException) {
        null
    }

    private fun tagTitles(tags: Any?): List<String> = (tags as? JSONArray)?.let { array ->
        (0 until array.length()).mapNotNull { index ->
            when (val tag = array.opt(index)) {
                is String -> tag
                is JSONObject -> tag.text("_sTitle")
                else -> null
            }
        }
    }.orEmpty()

    private fun images(record: JSONObject): List<Image> =
        record.obj("_aPreviewMedia")?.array("_aImages")?.objects().orEmpty().mapNotNull { image ->
            val base = image.text("_sBaseUrl") ?: return@mapNotNull null
            val file = image.text("_sFile") ?: return@mapNotNull null
            Image(base, file, image.text("_sFile220"), image.text("_sFile530"))
        }

    private fun files(array: JSONArray?): List<ModFile> = array?.objects().orEmpty().mapNotNull { file ->
        ModFile(
            name = file.text("_sFile") ?: return@mapNotNull null,
            size = file.number("_nFilesize"),
            url = file.text("_sDownloadUrl") ?: return@mapNotNull null,
            md5 = file.text("_sMd5Checksum")?.takeIf { it.length == 32 },
            description = file.text("_sDescription").orEmpty(),
        )
    }

    private fun <T> parsing(block: () -> T): T = try {
        block()
    } catch (e: JSONException) {
        throw IOException("GameBanana sent something this launcher cannot read.", e)
    }

    private fun JSONObject.text(key: String): String? = (opt(key) as? String)?.takeIf { it.isNotBlank() }
    private fun JSONObject.number(key: String): Long = when (val value = opt(key)) {
        is Number -> value.toLong()
        is String -> value.toLongOrNull() ?: 0
        else -> 0
    }
    private fun JSONObject.flag(key: String): Boolean = opt(key) as? Boolean ?: false
    private fun JSONObject.obj(key: String): JSONObject? = opt(key) as? JSONObject
    private fun JSONObject.array(key: String): JSONArray? = opt(key) as? JSONArray
    private fun JSONArray.objects(): List<JSONObject> = (0 until length()).mapNotNull { opt(it) as? JSONObject }

    // Network; everything below blocks, so callers run it off the main thread.

    /** One page of Mario Kart Wii mods for [term], which [effectiveSearch] made. */
    fun search(term: String, page: Int): Page = parsePage(get(searchUrl(term, page)))

    fun details(modId: Int): Details = parseDetails(get(detailsUrl(modId)))

    private fun connect(url: String): HttpURLConnection = (URL(url).openConnection() as HttpURLConnection).apply {
        connectTimeout = TIMEOUT_MS
        readTimeout = TIMEOUT_MS
        setRequestProperty("User-Agent", userAgent)
    }

    private fun get(url: String): String {
        val connection = connect(url)
        try {
            val code = try {
                connection.responseCode
            } catch (e: IOException) {
                throw IOException("GameBanana could not be reached. Check the headset's internet connection.", e)
            }
            val ok = code in 200..299
            val body = (if (ok) connection.inputStream else connection.errorStream)?.use { it.readBytes().toString(Charsets.UTF_8) }.orEmpty()
            if (!ok) throw IOException(errorMessage(body)?.let { "GameBanana: $it" } ?: "GameBanana answered $code.")
            return body
        } finally {
            connection.disconnect()
        }
    }

    /**
     * Downloads [file] to [target] and checks it against the MD5 GameBanana lists for it (or the
     * size, when there is no MD5), so a download cut short or damaged on the way never reaches the
     * Mods folder. [progress] gets the bytes so far and the total, and returns false to cancel,
     * which throws [Cancelled]. A failed attempt is tried again twice, as WheelWizard retries its
     * downloads, and handed to [failedAttempt] first.
     */
    fun download(
        file: ModFile,
        target: File,
        failedAttempt: (Int, IOException) -> Unit = { _, _ -> },
        progress: (Long, Long) -> Boolean,
    ) {
        var failure: IOException? = null
        for (attempt in 1..DOWNLOAD_ATTEMPTS) {
            try {
                downloadOnce(file, target, progress)
                return
            } catch (e: Cancelled) {
                target.delete()
                throw e
            } catch (e: IOException) {
                target.delete()
                failure = e
                failedAttempt(attempt, e)
                if (attempt < DOWNLOAD_ATTEMPTS) {
                    Thread.sleep(1_000L * attempt)
                    if (!progress(0, file.size)) throw Cancelled()
                }
            }
        }
        throw failure ?: IOException("The download of ${file.name} failed.")
    }

    private fun downloadOnce(file: ModFile, target: File, progress: (Long, Long) -> Boolean) {
        val connection = connect(file.url)
        // Android would ask for gzip, and the length it then reports is not the file's size.
        connection.setRequestProperty("Accept-Encoding", "identity")
        try {
            val code = try {
                connection.responseCode
            } catch (e: IOException) {
                throw IOException("GameBanana could not be reached to download ${file.name}. Check the headset's internet connection.", e)
            }
            if (code != HttpURLConnection.HTTP_OK) throw IOException("GameBanana answered $code to the download of ${file.name}.")
            val total = connection.contentLengthLong.takeIf { it > 0 } ?: file.size
            val digest = MessageDigest.getInstance("MD5")
            var done = 0L
            if (!progress(done, total)) throw Cancelled()
            connection.inputStream.use { input ->
                target.outputStream().use { output ->
                    val buffer = ByteArray(DOWNLOAD_BUFFER)
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        output.write(buffer, 0, count)
                        digest.update(buffer, 0, count)
                        done += count
                        if (!progress(done, total)) throw Cancelled()
                    }
                }
            }
            val md5 = digest.digest().joinToString("") { "%02x".format(it) }
            when {
                file.md5 != null && !md5.equals(file.md5, ignoreCase = true) ->
                    throw IOException("The download of ${file.name} does not match the checksum GameBanana lists for it.")
                file.md5 == null && file.size > 0 && done != file.size ->
                    throw IOException("The download of ${file.name} stopped at $done of ${file.size} bytes.")
            }
        } finally {
            connection.disconnect()
        }
    }
}
