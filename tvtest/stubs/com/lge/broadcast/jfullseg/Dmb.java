/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package com.lge.broadcast.jfullseg;

import android.view.SurfaceHolder;

import java.io.FileDescriptor;

/**
 * Compile time stub for the shared library the full-seg stack exposes, which
 * ships as a prebuilt at /product/framework/com.lge.broadcast.jfullseg.jar and
 * is reached with <uses-library android:name="com.lge.broadcast.jfullseg"/>.
 *
 * Nothing here runs: this library is compile-only, and at runtime the loader
 * resolves the real class out of the prebuilt jar. The declarations are the
 * ones the stock app actually calls, taken from the jar itself, so an app built
 * against this finds the same methods on the device.
 */
public class Dmb {

    public interface AudioCallback {
        void onAudio(int what, int extra, Dmb dmb);
    }

    public interface DataCallback {
        void onData(int type, int size, byte[] data, Dmb dmb);
    }

    public interface EventCallback {
        void onEvent(int event, Dmb dmb);
    }

    public interface SignalCallback {
        void onSignal(int lock, Dmb dmb);
    }

    public interface VideoCallback {
        void onVideo(int what, int extra, Dmb dmb);
    }

    public static Dmb open() {
        throw new RuntimeException("stub");
    }

    public final void init(int opMode, int liveFlag, EventCallback eventCb) {
        throw new RuntimeException("stub");
    }

    public final void exit() {
        throw new RuntimeException("stub");
    }

    public final void release() {
        throw new RuntimeException("stub");
    }

    /** Takes the physical channel, not a frequency: 13..52 in japan. */
    public final void find(int physicalChannel, SignalCallback signalCb) {
        throw new RuntimeException("stub");
    }

    public final void select(int opMode, int freq, int serviceId, int netId, int transportId,
            int oriNetId, int channelId, int serviceType, int pmtPid, int videoPid, int audioPid,
            int emmPid, int ecmPid, int serviceId1Seg, VideoCallback videoCb,
            AudioCallback audioCb) {
        throw new RuntimeException("stub");
    }

    public final void data(int dataType, int onOff, DataCallback dataCb) {
        throw new RuntimeException("stub");
    }

    /** Free size is in kilobytes; the service stops when it runs out. */
    public final int record(int opMode, String fileName, long freeSize) {
        throw new RuntimeException("stub");
    }

    public final int record(int opMode, FileDescriptor fd, long freeSize) {
        throw new RuntimeException("stub");
    }

    public final void stopRecord() {
        throw new RuntimeException("stub");
    }

    public final void cancelRecord() {
        throw new RuntimeException("stub");
    }

    public final native int getDuration();

    public final int getCASStatus() {
        throw new RuntimeException("stub");
    }

    public final void setVideoDimension(int x, int y, int w, int h, int rotation,
            SurfaceHolder holder) {
        throw new RuntimeException("stub");
    }

    public final int setMuteStatus(int mute) {
        throw new RuntimeException("stub");
    }

    public int getFreq() {
        throw new RuntimeException("stub");
    }

    public int getServiceId() {
        throw new RuntimeException("stub");
    }

    public int getServiceId1Seg() {
        throw new RuntimeException("stub");
    }

    public int getNetId() {
        throw new RuntimeException("stub");
    }

    public int getTransportId() {
        throw new RuntimeException("stub");
    }

    public int getOriNetId() {
        throw new RuntimeException("stub");
    }

    public int getServiceType() {
        throw new RuntimeException("stub");
    }

    public int getPmtPid() {
        throw new RuntimeException("stub");
    }

    public int getVideoPid() {
        throw new RuntimeException("stub");
    }

    public int getAudioPid() {
        throw new RuntimeException("stub");
    }

    public int getEmmPid() {
        throw new RuntimeException("stub");
    }

    public int getEcmPid() {
        throw new RuntimeException("stub");
    }
}
