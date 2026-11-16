package com.libertyrecomp;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class FrameRateHintTest {

    @Test
    public void capsAreRequestedAsTheyAre() {
        assertEquals(30f, FrameRateHint.requestedRate(30f), 0f);
        assertEquals(40f, FrameRateHint.requestedRate(40f), 0f);
        assertEquals(60f, FrameRateHint.requestedRate(60f), 0f);
        assertEquals(120f, FrameRateHint.requestedRate(120f), 0f);
    }

    @Test
    public void unlockedAndInvalidMeanNoPreference() {
        assertEquals(0f, FrameRateHint.requestedRate(0f), 0f);
        assertEquals(0f, FrameRateHint.requestedRate(-1f), 0f);
        assertEquals(0f, FrameRateHint.requestedRate(Float.NaN), 0f);
        assertEquals(0f, FrameRateHint.requestedRate(1000f), 0f);
    }
}
