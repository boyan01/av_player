package tech.soit.flutter.player

import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.annotation.Keep
import io.flutter.view.TextureRegistry
import java.util.concurrent.CountDownLatch

object MediaPlayerBridge {

    private const val TAG = "MediaPlayerBridge"

    private var textureRegistry: TextureRegistry? = null

    fun onAttached(textureRegistry: TextureRegistry) {
        if (this.textureRegistry != null) {
            throw RuntimeException("texture registry already registered.")
        }
        this.textureRegistry = textureRegistry
        setupJNI()
    }


    @Keep
    fun register(): FlutterTexture? {
        val countDownLatch = CountDownLatch(1)

        var texture: TextureRegistry.SurfaceTextureEntry? = null
        Handler(Looper.getMainLooper()).post {
            texture = textureRegistry?.createSurfaceTexture()
            countDownLatch.countDown()
        }
        countDownLatch.await()
        Log.d(TAG, "register: $texture")
        texture ?: return null
        return FlutterTexture(texture!!)
    }


    @Keep
    private external fun setupJNI();


}