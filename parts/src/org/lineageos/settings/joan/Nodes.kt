/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.settings.joan

import android.util.Log
import java.io.File
import java.io.IOException

private const val TAG = "JoanParts"

/*
 * LG's charging driver collects everything it wants userspace to see under one
 * platform device rather than spreading it over the power supply classes - see
 * drivers/power/supply/lge/unified-nodes.c in the kernel tree. The device tree
 * labels the directory sysfs_lge_power, so one genfscon entry covers the lot.
 *
 * Every node here answers a bare decimal with no trailing newline.
 */
object UnifiedNodes {
    private const val DIR = "/sys/devices/platform/lge-unified-nodes"

    /* charge_full over charge_full_design, as a percentage, capped at 100. */
    const val BATTERY_AGE = "battery_age"

    /* A bucket of the above: 1 >= 80%, 2 >= 50%, 3 below that, 0 unreadable. */
    const val BATTERY_CONDITION = "battery_condition"

    const val BATTERY_CYCLE = "battery_cycle"
    const val CHARGER_NAME = "charger_name"
    const val CHARGER_HIGHSPEED = "charger_highspeed"
    const val SUPPORT_FASTCHG = "support_fastchg"
    const val CHARGING_COMPLETED = "charging_completed"

    /*
     * LG's demo mode. protection-showcase.c holds the charge inside the range
     * the device tree gives it - 45..50% on joan - with the same current voters
     * the thermal limits use, so it holds while the handset is asleep and
     * survives userspace going away. That makes it the honest way to park a
     * battery that is going into a drawer.
     */
    const val CHARGING_SHOWCASE = "charging_showcase"

    fun read(node: String): String? = readFile(File(DIR, node))

    fun readInt(node: String): Int? = read(node)?.toIntOrNull()

    fun write(node: String, value: String): Boolean = writeFile(File(DIR, node), value)
}

private fun readFile(file: File): String? =
    try {
        file.readText().trim().ifEmpty { null }
    } catch (e: IOException) {
        /*
         * Missing, unlabelled or refused by the driver - a screen that reports
         * what the hardware says is no place to throw over any of them.
         */
        Log.d(TAG, "could not read $file", e)
        null
    }

private fun writeFile(file: File, value: String): Boolean =
    try {
        file.writeText(value)
        true
    } catch (e: IOException) {
        Log.e(TAG, "could not write $value to $file", e)
        false
    }
