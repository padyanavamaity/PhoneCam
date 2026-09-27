package org.phonecam.connect.stream

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class CameraConfigRepositoryTest {

    private fun repo(store: InMemoryConfigStore, logs: MutableList<String> = mutableListOf()) =
        CameraConfigRepository(store) { logs.add(it) }

    @Test
    fun `defaults when store empty`() {
        val cfg = repo(InMemoryConfigStore()).load()
        assertEquals(CameraStreamConfig.DEFAULT, cfg)
    }

    @Test
    fun `round trip persists all fields`() {
        val store = InMemoryConfigStore()
        val repo = repo(store)
        val cfg = CameraStreamConfig(
            facing = CameraFacing.FRONT,
            width = 1920, height = 1080, fps = 60,
            videoBitrateBps = 8_000_000, codec = VideoCodec.VP9
        )
        repo.save(cfg)
        val loaded = repo(store).load()
        assertEquals(cfg, loaded)
    }

    @Test
    fun `unknown facing falls back to BACK`() {
        val store = InMemoryConfigStore()
        store.putString(CameraConfigRepository.KEY_FACING, "SIDEWAYS")
        val logs = mutableListOf<String>()
        val cfg = repo(store, logs).load()
        assertEquals(CameraFacing.BACK, cfg.facing)
        assertTrue(logs.any { it.contains("facing", ignoreCase = true) })
    }

    @Test
    fun `unknown codec falls back to VP8`() {
        val store = InMemoryConfigStore()
        store.putString(CameraConfigRepository.KEY_CODEC, "av1")
        val logs = mutableListOf<String>()
        val cfg = repo(store, logs).load()
        assertEquals(VideoCodec.VP8, cfg.codec)
    }

    @Test
    fun `bitrate clamped to range`() {
        val store = InMemoryConfigStore()
        store.putInt(CameraConfigRepository.KEY_BITRATE, 100)                    // too low
        assertEquals(CameraStreamConfig.MIN_BITRATE_BPS, repo(store).load().videoBitrateBps)

        store.putInt(CameraConfigRepository.KEY_BITRATE, 100_000_000)            // too high
        assertEquals(CameraStreamConfig.MAX_BITRATE_BPS, repo(store).load().videoBitrateBps)

        store.putInt(CameraConfigRepository.KEY_BITRATE, 6_000_000)              // in range
        assertEquals(6_000_000, repo(store).load().videoBitrateBps)
    }

    @Test
    fun `fps clamped to range`() {
        val store = InMemoryConfigStore()
        store.putInt(CameraConfigRepository.KEY_FPS, 1)
        assertEquals(CameraStreamConfig.MIN_FPS, repo(store).load().fps)

        store.putInt(CameraConfigRepository.KEY_FPS, 240)
        assertEquals(CameraStreamConfig.MAX_FPS, repo(store).load().fps)

        store.putInt(CameraConfigRepository.KEY_FPS, 30)
        assertEquals(30, repo(store).load().fps)
    }

    @Test
    fun `resolution snapped to nearest supported`() {
        val supported = listOf(
            SizeFps(1920, 1080, 30),
            SizeFps(1280, 720, 30),
            SizeFps(640, 480, 30)
        )
        val store = InMemoryConfigStore()
        store.putInt(CameraConfigRepository.KEY_WIDTH, 1000)   // not in list
        store.putInt(CameraConfigRepository.KEY_HEIGHT, 1000)
        val cfg = repo(store).load(supported)
        // Nearest by pixel count to 1000x1000 (=1_000_000 px) is 1280x720 (=921_600 px)
        assertEquals(1280, cfg.width)
        assertEquals(720, cfg.height)
    }

    @Test
    fun `supported exact resolution passes through unchanged`() {
        val supported = listOf(SizeFps(1280, 720, 30))
        val store = InMemoryConfigStore()
        store.putInt(CameraConfigRepository.KEY_WIDTH, 1280)
        store.putInt(CameraConfigRepository.KEY_HEIGHT, 720)
        val cfg = repo(store).load(supported)
        assertEquals(1280, cfg.width)
        assertEquals(720, cfg.height)
    }

    @Test
    fun `no supported list leaves resolution as stored`() {
        val store = InMemoryConfigStore()
        store.putInt(CameraConfigRepository.KEY_WIDTH, 999)
        store.putInt(CameraConfigRepository.KEY_HEIGHT, 555)
        val cfg = repo(store).load()   // supported = null
        assertEquals(999, cfg.width)
        assertEquals(555, cfg.height)
    }

    @Test
    fun `codec fromKey round trips`() {
        for (c in VideoCodec.entries) {
            assertEquals(c, VideoCodec.fromKey(c.prefKey))
        }
        assertEquals(VideoCodec.VP8, VideoCodec.fromKey("bogus"))
        assertEquals(VideoCodec.VP8, VideoCodec.fromKey(null))
    }
}
