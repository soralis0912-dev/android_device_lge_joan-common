/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.tvtest.joan

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.Button
import android.widget.EditText
import android.widget.ScrollView
import android.widget.TextView

import com.lge.broadcast.jfullseg.Dmb

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * Drives the full-seg stack by hand so the tuner can be checked without the
 * stock app, and writes what comes off the air to a file.
 *
 * The stack is the one the stock app uses: com.lge.broadcast.jfullseg.Dmb over
 * JNI into lgemtvserver, which owns the tuner and demux HALs. Everything here
 * is a thin wrapper over that - the point is to see the return codes and the
 * callbacks, so nothing is retried or hidden.
 */
class TunerTestActivity : Activity() {

    private val worker = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())

    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var statusView: TextView
    private lateinit var channelField: EditText
    private lateinit var serviceIdField: EditText
    private lateinit var serviceTypeField: EditText
    private lateinit var pathField: EditText

    private var dmb: Dmb? = null
    private var recordFile: File? = null
    private var recordHandle: RandomAccessFile? = null
    private var recording = false

    /** find() answers through the signal callback, which is what this waits on. */
    private val findLatch = AtomicReference<CountDownLatch?>()
    private var lastLock = 0

    private val eventCallback = Dmb.EventCallback { event, _ ->
        log("event  ${Mbt.event(event)}")
    }

    private val signalCallback = Dmb.SignalCallback { lock, _ ->
        lastLock = lock
        log("signal ${Mbt.event(lock)}")
        findLatch.get()?.countDown()
    }

    private val dataCallback = Dmb.DataCallback { type, size, data, _ ->
        val bytes = data ?: ByteArray(0)
        val head = bytes.take(16).joinToString(" ") { "%02x".format(it) }
        log("data   ${Mbt.dataType(type)} size=$size ${bytes.size}B $head")
        if (type == Mbt.DATA_PSI_INFO && bytes.size >= 16) {
            val b = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            log("       psi ch=${b.int} serviceId=${b.int} ${b.int} recordable=${b.int}")
        }
    }

    private val videoCallback = Dmb.VideoCallback { what, extra, _ ->
        log("video  what=$what extra=$extra")
    }

    private val audioCallback = Dmb.AudioCallback { what, extra, _ ->
        log("audio  what=$what extra=$extra")
    }

    private val ticker = object : Runnable {
        override fun run() {
            val file = recordFile
            if (recording && file != null) {
                statusView.text =
                    getString(R.string.status_recording, file.name, humanSize(file.length()))
                ui.postDelayed(this, 1000)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.tuner_test)

        logView = findViewById(R.id.log)
        logScroll = findViewById(R.id.log_scroll)
        statusView = findViewById(R.id.status)
        channelField = findViewById(R.id.channel)
        serviceIdField = findViewById(R.id.service_id)
        serviceTypeField = findViewById(R.id.service_type)
        pathField = findViewById(R.id.path)

        statusView.setText(R.string.status_idle)
        pathField.setText(defaultOutput().absolutePath)

        findViewById<Button>(R.id.init_scan).setOnClickListener {
            initStack(Mbt.OP_MODE_ENSQUERY)
        }
        findViewById<Button>(R.id.init_view).setOnClickListener {
            initStack(Mbt.OP_MODE_DMB)
        }
        findViewById<Button>(R.id.exit).setOnClickListener { exitStack() }
        findViewById<Button>(R.id.find).setOnClickListener { find(channel()) }
        findViewById<Button>(R.id.find_all).setOnClickListener { findAll() }
        findViewById<Button>(R.id.select).setOnClickListener { select() }
        findViewById<Button>(R.id.record).setOnClickListener { record(true) }
        findViewById<Button>(R.id.record_path).setOnClickListener { record(false) }
        findViewById<Button>(R.id.stop).setOnClickListener { stopRecord() }
        findViewById<Button>(R.id.data_on).setOnClickListener { enableData() }
        findViewById<Button>(R.id.save_log).setOnClickListener { saveLog() }
        findViewById<Button>(R.id.clear_log).setOnClickListener { logView.text = "" }

        log("ready. /product/framework/com.lge.broadcast.jfullseg.jar via uses-library")
    }

    override fun onDestroy() {
        if (recording) {
            stopRecord()
        }
        worker.execute {
            dmb?.let {
                runCatching { it.exit() }
                runCatching { it.release() }
            }
            dmb = null
        }
        worker.shutdown()
        super.onDestroy()
    }

    /*
     * The stack takes an op mode on init and keeps it: 6 (ENSQUERY) is the one
     * find() needs, 2 (DMB) the one select() and record() need. Switching means
     * exit() and init() again, which is what the stock app does too.
     */
    private fun initStack(opMode: Int) = call("init(opMode=$opMode, live=1)") {
        val instance = dmb ?: Dmb.open().also { dmb = it }
        instance.init(opMode, Mbt.LIVE_FLAG_ONAIR, eventCallback)
    }

    private fun exitStack() = call("exit") {
        dmb?.exit()
    }

    private fun find(channel: Int) = call("find(ch=$channel)") {
        findOne(channel)
    }

    private fun findAll() = call("find 13..52") {
        for (channel in 13..52) {
            if (!findOne(channel)) {
                return@call
            }
        }
    }

    /**
     * Returns false when the stack stopped answering, so a sweep gives up
     * rather than sitting through forty timeouts.
     */
    private fun findOne(channel: Int): Boolean {
        val instance = dmb ?: run {
            log("find: init first")
            return false
        }
        val latch = CountDownLatch(1)
        findLatch.set(latch)
        lastLock = 0
        val khz = Mbt.frequencyKhz(channel)
        log("find   ch=$channel (${khz / 1000.0} MHz)")
        instance.find(channel, signalCallback)
        val answered = latch.await(10, TimeUnit.SECONDS)
        findLatch.set(null)
        if (!answered) {
            log("find   ch=$channel timed out")
            return false
        }
        if (lastLock == Mbt.SIG_LOCK) {
            log("find   ch=$channel LOCKED serviceId=${instance.serviceId}" +
                    " netId=${instance.netId} tsId=${instance.transportId}" +
                    " type=${instance.serviceType} pmt=${instance.pmtPid}" +
                    " video=${instance.videoPid} audio=${instance.audioPid}")
        }
        return true
    }

    /*
     * The pids are what the stock app passes: 0 means "whatever the service
     * says", which the stack turns into 0x1fff, so leaving the fields empty
     * tunes the physical channel without a filter of our own.
     */
    private fun select() = call("select") {
        val instance = dmb ?: run {
            log("select: init first")
            return@call
        }
        val channel = channel()
        val serviceId = serviceIdField.text.toString().toIntOrNull() ?: 0
        val serviceType = serviceTypeField.text.toString().toIntOrNull() ?: 1
        log("select ch=$channel serviceId=$serviceId type=$serviceType")
        instance.select(
            Mbt.OP_MODE_DMB, channel, serviceId, 0, 0, 0, 0, serviceType,
            0, 0, 0, 0, 0, 0, videoCallback, audioCallback
        )
    }

    private fun enableData() = call("data on") {
        val instance = dmb ?: run {
            log("data: init first")
            return@call
        }
        for (type in intArrayOf(
            Mbt.DATA_SI_INFO, Mbt.DATA_PSI_INFO, Mbt.DATA_CHANNEL_SI,
            Mbt.DATA_TOT, Mbt.DATA_MULTI_SID, Mbt.DATA_TS
        )) {
            instance.data(type, 1, dataCallback)
        }
    }

    /**
     * Two ways in, because only one of them may work.
     *
     * record(FileDescriptor) is the default: we open the file, the service
     * writes through the descriptor, and sepolicy only has to let it write.
     * record(String) is what the stock app uses and has the service open the
     * path itself, which the platform forbids under /data/data - point it at
     * somewhere with a label of its own to try that one.
     */
    private fun record(useFd: Boolean) = call("record") {
        val instance = dmb ?: run {
            log("record: init first")
            return@call
        }
        if (recording) {
            log("record: already recording")
            return@call
        }
        val file = File(pathField.text.toString())
        file.parentFile?.mkdirs()
        val freeKb = freeKilobytes(file)
        val result = if (useFd) {
            val handle = RandomAccessFile(file, "rw")
            recordHandle = handle
            log("record fd ${file.absolutePath} free=${freeKb}kB")
            instance.record(Mbt.OP_MODE_DMB, handle.fd, freeKb)
        } else {
            log("record    ${file.absolutePath} free=${freeKb}kB")
            instance.record(Mbt.OP_MODE_DMB, file.absolutePath, freeKb)
        }
        log("record -> $result")
        if (result >= 0) {
            recordFile = file
            recording = true
            ui.post(ticker)
        } else {
            recordHandle?.close()
            recordHandle = null
        }
    }

    private fun stopRecord() = call("stopRecord") {
        dmb?.stopRecord()
        recording = false
        recordHandle?.let { runCatching { it.close() } }
        recordHandle = null
        val file = recordFile
        ui.post {
            statusView.text = if (file == null) {
                getString(R.string.status_idle)
            } else {
                "${file.absolutePath}  ${humanSize(file.length())}"
            }
        }
        if (file != null) {
            log("stopped ${file.absolutePath} ${file.length()} bytes")
            log(describe(file))
        }
    }

    /**
     * Enough of a look at the file to tell whether it is a transport stream at
     * all: 0x47 every 188 bytes is the sync, and anything else means the
     * container has a header or the capture never started. The real check is
     * tools/ts-inspect.py on the host.
     */
    private fun describe(file: File): String {
        if (!file.isFile || file.length() < 188L * 8) {
            return "       too short to look at"
        }
        val head = ByteArray(188 * 8)
        RandomAccessFile(file, "r").use { it.readFully(head) }
        for (stride in intArrayOf(188, 192, 204)) {
            for (offset in 0 until stride) {
                if (offset + stride * 7 >= head.size) {
                    continue
                }
                var synced = true
                for (packet in 0..7) {
                    if (head[offset + stride * packet] != 0x47.toByte()) {
                        synced = false
                        break
                    }
                }
                if (synced) {
                    return "       sync 0x47 at offset $offset every $stride bytes"
                }
            }
        }
        return "       no 0x47 sync in the first ${head.size} bytes: " +
                head.take(16).joinToString(" ") { "%02x".format(it) }
    }

    private fun saveLog() = call("save log") {
        val file = File(getExternalFilesDir(null) ?: filesDir, "tuner-test-${stamp()}.log")
        file.writeText(logView.text.toString())
        log("log saved to ${file.absolutePath}")
    }

    private fun channel() = channelField.text.toString().toIntOrNull() ?: 13

    /*
     * The service is the one writing, so the file goes where it can reach:
     * this app runs as the system uid, so its own data directory is
     * system_app_data_file and one sepolicy rule covers it. External storage
     * would be a fuse mount and a much wider hole.
     */
    private fun defaultOutput() = File(filesDir, "joan-${stamp()}.jdmb")

    /** What the stock app passes: free space minus its 40 MB margin, in kB. */
    private fun freeKilobytes(file: File): Long {
        val dir = file.parentFile ?: filesDir
        val free = dir.usableSpace
        return if (free > 40L * 1024 * 1024) (free - 40L * 1024 * 1024) / 1024 else 0
    }

    private fun humanSize(bytes: Long) = when {
        bytes >= 1L shl 20 -> "%.1f MB".format(bytes / 1048576.0)
        bytes >= 1L shl 10 -> "%.1f kB".format(bytes / 1024.0)
        else -> "$bytes B"
    }

    private fun stamp() =
        SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())

    /** Every call goes through the one worker so they cannot overlap. */
    private fun call(what: String, block: () -> Unit) {
        worker.execute {
            try {
                block()
            } catch (e: Throwable) {
                log("$what threw $e")
                Log.e(TAG, what, e)
            }
        }
    }

    private fun log(line: String) {
        Log.i(TAG, line)
        val stamped = "${SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(Date())}  $line"
        ui.post {
            logView.append(stamped + "\n")
            logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
        }
    }

    companion object {
        private const val TAG = "JoanTunerTest"
    }
}
