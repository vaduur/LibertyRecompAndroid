package com.libertyrecomp;

import android.os.Build;
import android.util.Log;
import android.view.Surface;

/**
 * Surface.setFrameRate for the game window (Android 11+).
 *
 * <p>{@link Surface#FRAME_RATE_COMPATIBILITY_FIXED_SOURCE} says the content
 * runs at exactly this rate, so the system prefers a refresh rate that is an
 * integer multiple of it (60 or 120 Hz for 30 FPS) instead of the adaptive
 * panel's own heuristics. The default seamless-only strategy is kept: the
 * system never blanks the screen to switch modes because of this hint.</p>
 */
final class FrameRateHint {

    static final int APPLIED = 0;
    static final int UNSUPPORTED = 1;
    static final int INVALID_SURFACE = 2;
    static final int REJECTED = 3;

    private static final String TAG = "LibertyRecomp";

    private FrameRateHint() {}

    /**
     * The rate to request for a frame limit: the limit itself, or 0 (no
     * preference) for an unlocked or nonsensical one.
     */
    static float requestedRate(float frameLimit) {
        if (!(frameLimit > 0f) || frameLimit > 240f) return 0f;
        return frameLimit;
    }

    /** Applies {@code frameLimit} to {@code surface}; returns one of the constants above. */
    static int apply(Surface surface, float frameLimit) {
        if (Build.VERSION.SDK_INT < 30) return UNSUPPORTED;
        if (surface == null || !surface.isValid()) return INVALID_SURFACE;
        float rate = requestedRate(frameLimit);
        try {
            surface.setFrameRate(rate, rate > 0f
                    ? Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
                    : Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            return APPLIED;
        } catch (IllegalArgumentException | IllegalStateException e) {
            // A surface released between isValid() and the call, or a rate
            // the platform refuses: the panel keeps its own choice.
            Log.w(TAG, "frame rate hint " + rate + " rejected", e);
            return REJECTED;
        }
    }
}
