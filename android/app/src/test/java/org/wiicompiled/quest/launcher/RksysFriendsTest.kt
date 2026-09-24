package org.wiicompiled.quest.launcher

import java.util.zip.CRC32
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Friend lists are read and changed in rksys.dat as WheelWizard's GameLicenseService does. */
class RksysFriendsTest {

    private val ownId = 0x01234567L
    private val friendId = 0x00ABCDEFL

    /** A whole save with one licence (slot 0) whose profile ID is [profileId]. */
    private fun save(profileId: Long = ownId): ByteArray = ByteArray(RksysFriends.SAVE_SIZE).also { save ->
        "RKSD0006".toByteArray(Charsets.US_ASCII).copyInto(save, 0)
        "RKPD".toByteArray(Charsets.US_ASCII).copyInto(save, 8)
        putU32(save, 8 + 0x5C, profileId)
    }

    private fun code(profileId: Long): String = RksysProfiles.friendCode(profileId)

    private val mii = MiiFactory.female("Friend")

    @Test
    fun addingWritesThePcsPendingRequest() {
        val save = save()
        val data = MiiData.serialize(mii)
        assertNull(RksysFriends.add(save, 0, code(friendId), data, vr = 6050))

        val slot = 8 + 0x56D0
        val registration = 8 + 0x8B50
        assertEquals(code(friendId).replace("-", "").toLong() ushr 32, u32(save, slot))
        assertEquals(friendId, u32(save, slot + 4))
        assertEquals(1, u16(save, slot + 0x10))
        assertEquals(listOf(0, 0, 6050, 5000), listOf(0x12, 0x14, 0x16, 0x18).map { u16(save, slot + it) })
        assertArrayEquals(data, save.copyOfRange(slot + 0x1A, slot + 0x1A + MiiData.SIZE))
        assertEquals(MiiDatabase.crc16(data, 0, data.size), u16(save, slot + 0x64))
        assertEquals(listOf(0, 0, 0xFF, 0xFF), (0x66..0x69).map { save[slot + it].toInt() and 0xFF })
        assertEquals(List(10) { 0xFFFFFFFFL }, (0 until 10).map { u32(save, slot + 0x70 + it * 8) })
        assertEquals(0x10, save[registration + 2].toInt())
        assertEquals(friendId, u32(save, registration + 4))
        assertEquals(CRC32().apply { update(save, 0, 0x27FFC) }.value, u32(save, 0x27FFC))

        val friend = RksysFriends.parse(save, 0).single()
        assertEquals(code(friendId), friend.friendCode)
        assertEquals(mii, friend.mii)
        assertEquals("Friend", friend.name)
        assertEquals(6050, friend.vr)
        assertEquals(5000, friend.br)
        assertTrue(friend.isPending)
    }

    @Test
    fun aFriendWhoAddedBackIsNoLongerPending() {
        val save = save()
        RksysFriends.add(save, 0, code(friendId), MiiData.serialize(mii), vr = 5000)
        // The Wii rewrites the registration once the friend adds this licence back.
        save[8 + 0x8B50 + 2] = 0x38
        assertFalse(RksysFriends.parse(save, 0).single().isPending)
        // Old one-sided entries kept 0 there, and are pending too.
        save[8 + 0x8B50 + 2] = 0
        assertTrue(RksysFriends.parse(save, 0).single().isPending)
        // Both state bits: mutual.
        putU16(save, 8 + 0x56D0 + 0x10, 3)
        assertFalse(RksysFriends.parse(save, 0).single().isPending)
    }

    @Test
    fun addingRefusesWhatThePcRefuses() {
        val data = MiiData.serialize(mii)
        val wrongChecksum = code(friendId).let { valid -> (if (valid[0] == '9') "0" else "9") + valid.substring(1) }
        assertEquals(RksysFriends.Problem.Invalid, RksysFriends.add(save(), 0, wrongChecksum, data, 5000))
        assertEquals(RksysFriends.Problem.Own, RksysFriends.add(save(), 0, code(ownId), data, 5000))
        assertEquals(RksysFriends.Problem.NoLicense, RksysFriends.add(save(profileId = 0), 0, code(friendId), data, 5000))
        assertEquals(RksysFriends.Problem.NoLicense, RksysFriends.add(save(), 1, code(friendId), data, 5000))
        assertEquals(RksysFriends.Problem.NotASave, RksysFriends.add(ByteArray(0x30000), 0, code(friendId), data, 5000))

        val save = save()
        assertNull(RksysFriends.add(save, 0, code(friendId), data, 5000))
        assertEquals(RksysFriends.Problem.Duplicate, RksysFriends.add(save, 0, code(friendId), data, 5000))
        (1 until RksysFriends.MAX).forEach { assertNull(RksysFriends.add(save, 0, code(friendId + it), data, 5000)) }
        assertEquals(RksysFriends.MAX, RksysFriends.parse(save, 0).size)
        assertEquals(RksysFriends.Problem.Full, RksysFriends.add(save, 0, code(friendId + 100), data, 5000))
    }

    @Test
    fun removingEmptiesTheSlotForTheNextFriend() {
        val save = save()
        val data = MiiData.serialize(mii)
        RksysFriends.add(save, 0, code(friendId), data, 5000)
        RksysFriends.add(save, 0, code(friendId + 1), data, 5000)
        assertTrue(RksysFriends.remove(save, 0, friendId))
        assertFalse(RksysFriends.remove(save, 0, friendId))
        assertTrue((8 + 0x56D0 until 8 + 0x56D0 + 0x1C0).all { save[it] == 0.toByte() })
        assertTrue((8 + 0x8B50 until 8 + 0x8B50 + 0x0C).all { save[it] == 0.toByte() })
        assertEquals(CRC32().apply { update(save, 0, 0x27FFC) }.value, u32(save, 0x27FFC))
        assertEquals(listOf(1), RksysFriends.parse(save, 0).map { it.slot })
        // The first empty slot takes the next friend.
        RksysFriends.add(save, 0, code(friendId + 2), data, 5000)
        assertEquals(listOf(0 to friendId + 2, 1 to friendId + 1), RksysFriends.parse(save, 0).map { it.slot to it.profileId })
    }

    @Test
    fun friendCodesAreCheckedAsThePcChecksThem() {
        val valid = code(friendId)
        assertEquals(friendId, RksysFriends.profileId(valid))
        assertEquals(valid, RksysFriends.normalize(" ${valid.replace("-", " ")} ").friendCode)
        assertEquals(RksysFriends.Problem.Empty, RksysFriends.normalize("  ").problem)
        assertEquals(RksysFriends.Problem.NotTwelveDigits, RksysFriends.normalize("1234-5678").problem)
        assertEquals(RksysFriends.Problem.Invalid, RksysFriends.normalize("0000-0000-0000").problem)
        assertEquals(0L, RksysFriends.profileId("9999-9999-9999"))
    }

    @Test
    fun friendsAreSortedAsThePageChooses() {
        fun friend(slot: Int, name: String, vr: Int, wins: Int, losses: Int) =
            RksysFriends.Friend(slot, slot.toLong(), "", MiiFactory.male(name), vr, 5000, wins, losses, 0xFF, 0xFF, isPending = false)
        val friends = listOf(friend(0, "bob", 100, 5, 0), friend(1, "Anna", 300, 1, 9), friend(2, "carl", 200, 5, 1))
        val names = { order: FriendList.Order, online: (RksysFriends.Friend) -> Boolean ->
            FriendList.sorted(friends, order, online).map { it.name }
        }
        assertEquals(listOf("carl", "bob", "Anna"), names(FriendList.Order.Online) { it.slot == 2 })
        assertEquals(listOf("Anna", "carl", "bob"), names(FriendList.Order.Vr) { false })
        assertEquals(listOf("Anna", "bob", "carl"), names(FriendList.Order.Name) { false })
        // Equal wins keep the save's order.
        assertEquals(listOf("bob", "carl", "Anna"), names(FriendList.Order.Wins) { false })
        assertEquals(listOf("Anna", "carl", "bob"), names(FriendList.Order.Races) { false })
    }

    @Test
    fun aNulInsideAMiiNameIsNotShown() {
        val save = save()
        val named = MiiFactory.male("matt").apply { name = "matt\u0000ias" }
        RksysFriends.add(save, 0, code(friendId), MiiData.serialize(named), 5000)
        assertEquals("mattias", RksysFriends.parse(save, 0).single().name)
    }

    private fun u16(data: ByteArray, offset: Int): Int = ((data[offset].toInt() and 0xFF) shl 8) or (data[offset + 1].toInt() and 0xFF)

    private fun u32(data: ByteArray, offset: Int): Long =
        ((data[offset].toLong() and 0xFF) shl 24) or ((data[offset + 1].toLong() and 0xFF) shl 16) or
            ((data[offset + 2].toLong() and 0xFF) shl 8) or (data[offset + 3].toLong() and 0xFF)

    private fun putU16(data: ByteArray, offset: Int, value: Int) {
        data[offset] = (value ushr 8).toByte()
        data[offset + 1] = value.toByte()
    }

    private fun putU32(data: ByteArray, offset: Int, value: Long) {
        data[offset] = (value ushr 24).toByte()
        data[offset + 1] = (value ushr 16).toByte()
        data[offset + 2] = (value ushr 8).toByte()
        data[offset + 3] = value.toByte()
    }
}
