package org.atemvegeta.pocketwalker;

final class Bma150AntiShakeFilter {
    private static final int CAPACITY = 80;
    private static final long WINDOW_NS = 1_250_000_000L;
    private static final long EVALUATION_INTERVAL_NS = 200_000_000L;
    private static final int MIN_WINDOW_SAMPLES = 40;
    private static final float GRAVITY = 9.80665f;
    private static final float SATURATION_ACCELERATION = 2.0f * GRAVITY;
    private static final double ENTER_FREQUENCY_HZ = 3.6;
    private static final double LEAVE_FREQUENCY_HZ = 3.2;
    private static final double ENTER_SATURATION_RATIO = 0.35;
    private static final double LEAVE_SATURATION_RATIO = 0.22;
    private static final double ENTER_RMS = 8.0;
    private static final double QUIET_RMS = 1.0;
    private static final int ENTER_EVALUATIONS = 3;
    private static final int LEAVE_EVALUATIONS = 4;
    private static final double GRAVITY_FILTER_SECONDS = 1.0;

    private final long[] timestamps = new long[CAPACITY];
    private final float[] xSamples = new float[CAPACITY];
    private final float[] ySamples = new float[CAPACITY];
    private final float[] zSamples = new float[CAPACITY];
    private final int[] evaluationIndices = new int[CAPACITY];
    private int nextIndex;
    private int sampleCount;
    private long lastTimestamp;
    private long nextEvaluationTimestamp;
    private boolean includesGravity;
    private boolean gravityInitialized;
    private float gravityX;
    private float gravityY;
    private float gravityZ;
    private float outputX;
    private float outputY;
    private float outputZ;
    private boolean locked;
    private int enterCount;
    private int leaveCount;
    private double dominantFrequencyHz;
    private double saturationRatio;
    private double dynamicRms;

    void reset(boolean samplesIncludeGravity) {
        nextIndex = 0;
        sampleCount = 0;
        lastTimestamp = 0;
        nextEvaluationTimestamp = 0;
        includesGravity = samplesIncludeGravity;
        gravityInitialized = false;
        gravityX = 0;
        gravityY = 0;
        gravityZ = 0;
        outputX = 0;
        outputY = 0;
        outputZ = 0;
        locked = false;
        enterCount = 0;
        leaveCount = 0;
        dominantFrequencyHz = 0;
        saturationRatio = 0;
        dynamicRms = 0;
    }

    void update(long timestamp, float x, float y, float z) {
        updateGravity(timestamp, x, y, z);

        timestamps[nextIndex] = timestamp;
        xSamples[nextIndex] = x;
        ySamples[nextIndex] = y;
        zSamples[nextIndex] = z;
        nextIndex = (nextIndex + 1) % CAPACITY;
        if (sampleCount < CAPACITY)
            sampleCount++;

        if (nextEvaluationTimestamp == 0)
            nextEvaluationTimestamp = timestamp + WINDOW_NS;
        if (timestamp >= nextEvaluationTimestamp) {
            evaluate(timestamp);
            nextEvaluationTimestamp = timestamp + EVALUATION_INTERVAL_NS;
        }

        if (locked) {
            outputX = includesGravity ? gravityX : 0;
            outputY = includesGravity ? gravityY : 0;
            outputZ = includesGravity ? gravityZ : 0;
        } else {
            outputX = x;
            outputY = y;
            outputZ = z;
        }
    }

    private void updateGravity(long timestamp, float x, float y, float z) {
        if (!includesGravity)
            return;

        if (!gravityInitialized) {
            gravityX = x;
            gravityY = y;
            gravityZ = z;
            gravityInitialized = true;
            lastTimestamp = timestamp;
            return;
        }

        double elapsedSeconds = Math.max(0.001,
            Math.min(0.1, (timestamp - lastTimestamp) / 1_000_000_000.0));
        float alpha = (float)(elapsedSeconds / (GRAVITY_FILTER_SECONDS + elapsedSeconds));
        gravityX += alpha * (x - gravityX);
        gravityY += alpha * (y - gravityY);
        gravityZ += alpha * (z - gravityZ);
        lastTimestamp = timestamp;
    }

    private void evaluate(long now) {
        int count = 0;
        int oldest = (nextIndex - sampleCount + CAPACITY) % CAPACITY;
        for (int offset = 0; offset < sampleCount; offset++) {
            int index = (oldest + offset) % CAPACITY;
            if (now - timestamps[index] <= WINDOW_NS)
                evaluationIndices[count++] = index;
        }
        if (count < MIN_WINDOW_SAMPLES)
            return;

        double meanX = 0;
        double meanY = 0;
        double meanZ = 0;
        int saturated = 0;
        for (int offset = 0; offset < count; offset++) {
            int index = evaluationIndices[offset];
            meanX += xSamples[index];
            meanY += ySamples[index];
            meanZ += zSamples[index];
            if (Math.max(Math.abs(xSamples[index]),
                    Math.max(Math.abs(ySamples[index]), Math.abs(zSamples[index]))) >=
                SATURATION_ACCELERATION) {
                saturated++;
            }
        }
        meanX /= count;
        meanY /= count;
        meanZ /= count;

        double varianceX = 0;
        double varianceY = 0;
        double varianceZ = 0;
        for (int offset = 0; offset < count; offset++) {
            int index = evaluationIndices[offset];
            varianceX += square(xSamples[index] - meanX);
            varianceY += square(ySamples[index] - meanY);
            varianceZ += square(zSamples[index] - meanZ);
        }

        float[] dominantSamples;
        double dominantMean;
        double dominantVariance;
        if (varianceX >= varianceY && varianceX >= varianceZ) {
            dominantSamples = xSamples;
            dominantMean = meanX;
            dominantVariance = varianceX;
        } else if (varianceY >= varianceZ) {
            dominantSamples = ySamples;
            dominantMean = meanY;
            dominantVariance = varianceY;
        } else {
            dominantSamples = zSamples;
            dominantMean = meanZ;
            dominantVariance = varianceZ;
        }

        dynamicRms = Math.sqrt(dominantVariance / count);
        saturationRatio = saturated / (double)count;
        dominantFrequencyHz = findDominantFrequency(count, dominantSamples, dominantMean);

        boolean extreme = dominantFrequencyHz >= ENTER_FREQUENCY_HZ &&
            saturationRatio >= ENTER_SATURATION_RATIO && dynamicRms >= ENTER_RMS;
        boolean recovered = (dominantFrequencyHz <= LEAVE_FREQUENCY_HZ &&
            saturationRatio <= LEAVE_SATURATION_RATIO) || dynamicRms < QUIET_RMS;

        if (!locked) {
            enterCount = extreme ? enterCount + 1 : 0;
            if (enterCount >= ENTER_EVALUATIONS) {
                locked = true;
                enterCount = 0;
                leaveCount = 0;
            }
        } else {
            leaveCount = recovered ? leaveCount + 1 : 0;
            if (leaveCount >= LEAVE_EVALUATIONS) {
                locked = false;
                leaveCount = 0;
                enterCount = 0;
            }
        }
    }

    private double findDominantFrequency(int count, float[] samples, double mean) {
        double bestFrequency = 0;
        double bestPower = -1;
        long firstTimestamp = timestamps[evaluationIndices[0]];
        for (int frequencyStep = 10; frequencyStep <= 70; frequencyStep += 2) {
            double frequency = frequencyStep / 10.0;
            double cosine = 0;
            double sine = 0;
            for (int offset = 0; offset < count; offset++) {
                int index = evaluationIndices[offset];
                double seconds = (timestamps[index] - firstTimestamp) / 1_000_000_000.0;
                double angle = 2.0 * Math.PI * frequency * seconds;
                double centered = samples[index] - mean;
                cosine += centered * Math.cos(angle);
                sine += centered * Math.sin(angle);
            }
            double power = cosine * cosine + sine * sine;
            if (power > bestPower) {
                bestPower = power;
                bestFrequency = frequency;
            }
        }
        return bestFrequency;
    }

    private static double square(double value) {
        return value * value;
    }

    boolean isLocked() {
        return locked;
    }

    float getOutputX() {
        return outputX;
    }

    float getOutputY() {
        return outputY;
    }

    float getOutputZ() {
        return outputZ;
    }

}
