/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.settings.joan

import android.os.Bundle
import android.widget.Toast
import androidx.preference.Preference
import androidx.preference.SwitchPreferenceCompat
import com.android.settingslib.widget.SettingsBasePreferenceFragment

class BatteryHealthFragment :
    SettingsBasePreferenceFragment(), Preference.OnPreferenceChangeListener {

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        addPreferencesFromResource(R.xml.battery_health)

        findPreference<SwitchPreferenceCompat>(KEY_SHOWCASE)!!.apply {
            /*
             * The node is created by the charging driver, so a handset whose
             * kernel predates it - or whose sepolicy has not caught up - should
             * simply not offer the switch rather than offer one that does
             * nothing.
             */
            if (UnifiedNodes.readInt(UnifiedNodes.CHARGING_SHOWCASE) == null) {
                isVisible = false
            } else {
                onPreferenceChangeListener = this@BatteryHealthFragment
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        val age = UnifiedNodes.readInt(UnifiedNodes.BATTERY_AGE)
        setSummary(KEY_AGE, age?.let { getString(R.string.battery_percent, it) })

        setSummary(
            KEY_CONDITION,
            when (UnifiedNodes.readInt(UnifiedNodes.BATTERY_CONDITION)) {
                1 -> getString(R.string.battery_condition_good)
                2 -> getString(R.string.battery_condition_fair)
                3 -> getString(R.string.battery_condition_poor)
                else -> null
            },
        )

        setSummary(KEY_CYCLE, UnifiedNodes.readInt(UnifiedNodes.BATTERY_CYCLE)?.toString())

        /*
         * charger_name is whatever the driver matched, and is the one field
         * here that says which of the two chargers on this board is in use.
         */
        setSummary(KEY_CHARGER, UnifiedNodes.read(UnifiedNodes.CHARGER_NAME))

        setSummary(
            KEY_FAST_CHARGE,
            when {
                UnifiedNodes.readInt(UnifiedNodes.SUPPORT_FASTCHG) != 1 ->
                    getString(R.string.fast_charge_unsupported)
                UnifiedNodes.readInt(UnifiedNodes.CHARGER_HIGHSPEED) == 1 ->
                    getString(R.string.fast_charge_active)
                else -> getString(R.string.fast_charge_inactive)
            },
        )

        findPreference<SwitchPreferenceCompat>(KEY_SHOWCASE)?.isChecked =
            UnifiedNodes.readInt(UnifiedNodes.CHARGING_SHOWCASE) == 1
    }

    private fun setSummary(key: String, value: String?) {
        findPreference<Preference>(key)?.apply {
            summary = value ?: getString(R.string.value_unavailable)
            isEnabled = value != null
        }
    }

    override fun onPreferenceChange(preference: Preference, newValue: Any?): Boolean {
        if (preference.key != KEY_SHOWCASE) {
            return false
        }

        /*
         * ueventd hands the node to system, and the device tree labels it. Both
         * are easy to ship without the other, and a settings screen is no place
         * to die over it, so say so and leave the switch where it was.
         */
        val enabled = newValue as Boolean
        if (!UnifiedNodes.write(UnifiedNodes.CHARGING_SHOWCASE, if (enabled) "1" else "0")) {
            Toast.makeText(context, R.string.storage_mode_failed, Toast.LENGTH_LONG).show()
            return false
        }

        return true
    }

    companion object {
        private const val KEY_AGE = "battery_age"
        private const val KEY_CONDITION = "battery_condition"
        private const val KEY_CYCLE = "battery_cycle"
        private const val KEY_CHARGER = "charger_name"
        private const val KEY_FAST_CHARGE = "fast_charge"
        private const val KEY_SHOWCASE = "storage_mode"
    }
}
