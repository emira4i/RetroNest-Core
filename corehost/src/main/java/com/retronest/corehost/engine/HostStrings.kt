package com.retronest.corehost.engine

import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.os.LocaleList
import androidx.annotation.StringRes
import java.util.Locale

/** User-visible host texts (core load errors etc.), resolved when they are emitted. Logs stay English. */
interface Strings {
    /** Context.getString(id, *args). */
    fun get(@StringRes id: Int, vararg args: Any): String
}

/** Production [Strings]: resolves against the (per-app) locale on every call. */
class AndroidStrings(private val contextProvider: () -> Context) : Strings {
    override fun get(id: Int, vararg args: Any): String = contextProvider().getString(id, *args)
}

/**
 * Locale for the ":core" process, without depending on the app module.
 *
 * Android 13+ applies the per-app locale (LocaleManager) to every process of the app, so nothing
 * is needed. Below that the app mirrors the chosen language into SharedPreferences
 * `retronest_locale` / key `app_language` (a BCP-47 language tag; empty = follow the system) and
 * this reads that same file. Keep PREFS / KEY in sync with the app's `AndroidAppLocale`.
 */
object HostLocale {
    private const val PREFS = "retronest_locale"
    private const val KEY = "app_language"
    private val SUPPORTED = setOf("en", "tr", "ru", "de")

    /** [base] with the stored language applied below API 33; otherwise [base] itself. */
    fun wrap(base: Context): Context {
        if (Build.VERSION.SDK_INT >= 33) return base
        val stored = base.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, "").orEmpty()
        val language = stored.trim().substringBefore('-').substringBefore('_').lowercase()
        if (language !in SUPPORTED) return base
        val config = Configuration(base.resources.configuration)
        config.setLocales(LocaleList(Locale.forLanguageTag(language)))
        return base.createConfigurationContext(config)
    }
}
