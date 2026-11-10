/*
 * obs-browser-hid - "interact" virtual source resolution
 *
 * The OBS frontend's "interact" dialog (frontend/dialogs/OBSBasicInteraction)
 * is a QDialog whose title is
 *
 *     QTStr("Basic.InteractionWindow").arg(source_name)
 *       e.g. "Interacting with 'My Browser Source'" (en-US)
 *
 * Any plugin running inside the OBS UI process can therefore identify which
 * source an open interact window belongs to by scanning the QApplication's
 * widgets for a visible widget that is its own window (isWindow()) with the
 * OBSBasicInteraction class and matching the title suffix against browser
 * source names. It must be isWindow(), not topLevelWidgets(): the dialog is
 * created with the main window as Qt parent.
 *
 * Qt widgets are not thread-safe and obs-websocket invokes vendor request
 * callbacks from its own threads, so the scan is marshalled onto the Qt GUI
 * thread with a BlockingQueuedConnection before returning. When the caller is
 * already on the GUI thread (the "Browser HID" dock does this synchronously),
 * a BlockingQueuedConnection would wait for the thread that is executing it
 * and deadlock, so the scan then runs directly.
 *
 * This file also maps a click in an interact window back to source-canvas
 * coordinates for the dock's "capture position" feature. The math mirrors
 * OBSBasicInteraction::GetSourceRelativeXY() and GetScaleAndCenterPos() from
 * frontend/utility/display-helpers.hpp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs-browser-hid.h"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QMetaObject>
#include <QPoint>
#include <QSize>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

static bool is_browser_source_id(obs_source_t *src)
{
	const char *id = obs_source_get_unversioned_id(src);
	return id && strcmp(id, "browser_source") == 0;
}

/** Replaces control characters so titles cannot inject lines into the OBS log. */
static QString log_safe(const QString &s)
{
	QString out;
	out.reserve(s.size());
	for (QChar ch : s) {
		if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
			out += QLatin1Char('?');
		else
			out += ch;
	}
	return out;
}

/** The interact dialog title's static part up to (and including) the %1
 *  placeholder's quoting (e.g. "Interacting with '"). */
static QString interaction_title_prefix(void)
{
	const char *loc = obs_frontend_get_locale_string("Basic.InteractionWindow");
	QString pattern = loc ? QString::fromUtf8(loc) : QStringLiteral("Interacting with '%1'");
	QString prefix = pattern;
	prefix.replace("%1", "");
	return prefix;
}

/** Extracts the source name from an interact dialog title. Falls back to
 *  matching the title suffix against all browser source names for locales
 *  whose title pattern is unknown. Must be called from the Qt GUI thread. */
static QString interact_title_source_name(QWidget *w, const QString &prefix)
{
	const QString title = w->windowTitle();

	if (title.length() > prefix.length() && title.startsWith(prefix)) {
		QString name = title.mid(prefix.length());
		/* en-US pattern ends the title with a quote around the name */
		if (name.endsWith(u'\''))
			name.chop(1);
		if (!name.isEmpty())
			return name;
	}

	/* Locale fallback: no known prefix -> look for a browser source whose
	 * name is the title's suffix. */
	struct suffix_ctx {
		QString title;
		QString match;
	} sctx{title, QString{}};
	auto cb = [](void *p, obs_source_t *src) -> bool {
		auto *c = static_cast<struct suffix_ctx *>(p);
		if (!is_browser_source_id(src))
			return true;
		const char *src_name = obs_source_get_name(src);
		if (!src_name)
			return true;
		QString name = QString::fromUtf8(src_name);
		if (!name.isEmpty() && c->title.endsWith(name)) {
			c->match = name;
			return false;
		}
		return true;
	};
	obs_enum_sources(cb, &sctx);
	return sctx.match;
}

/** Must be called from the Qt GUI thread. */
static bool scan_interact_widget(QWidget *w, const QString &prefix, QString *result)
{
	if (!w->isWindow() || !w->isVisible() || !w->inherits("OBSBasicInteraction"))
		return false;

	const QString name = interact_title_source_name(w, prefix);
	if (name.isEmpty())
		return false;

	*result = name;
	return true;
}

/** Must be called from the Qt GUI thread. */
static void scan_interact_windows(QString *result)
{
	const QString prefix = interaction_title_prefix();

	for (QWidget *w : QApplication::allWidgets()) {
		/* the interact dialog is parented to the main OBS window, so
		 * it is NOT in topLevelWidgets(); isWindow() tells apart
		 * own-window widgets regardless of parent */
		if (scan_interact_widget(w, prefix, result))
			return;
	}

	/* diagnostics: on failure, log what IS on screen (bounded +
	 * sanitized: window titles come from user-created source names) */
	constexpr int MAX_WINDOWS = 16;
	constexpr int MAX_CHARS = 2000;
	QString windows;
	int listed = 0;
	for (QWidget *w : QApplication::allWidgets()) {
		if (!w->isWindow() || !w->isVisible())
			continue;
		if (listed++ >= MAX_WINDOWS)
			break;
		windows += QStringLiteral("%1 [%2]\n")
				   .arg(QString::fromUtf8(w->metaObject()->className()),
					log_safe(w->windowTitle().left(200)));
		if (windows.size() >= MAX_CHARS)
			break;
	}
	blog(LOG_DEBUG, "[obs-browser-hid-interact] no interact window found; visible windows:%s",
	     qUtf8Printable(log_safe(windows)));
}

/** Returns a bfree()-able strdup of the browser source name shown in the
 *  (first) open interact window, or NULL when there is none. */
extern "C" char *obs_browser_hid_interact_source_name(void)
{
	QCoreApplication *app = QCoreApplication::instance();

	/* Headless/non-Qt hosts have no widget tree to inspect. */
	if (!app)
		return NULL;

	QString result;

	if (QThread::currentThread() == app->thread()) {
		/* Already on the GUI thread (dock quick actions): run the
		 * scan directly. A BlockingQueuedConnection here would wait
		 * for the current thread and freeze the whole UI. */
		scan_interact_windows(&result);
	} else {
		const bool invoked = QMetaObject::invokeMethod(
			app, [&result]() { scan_interact_windows(&result); },
			Qt::BlockingQueuedConnection);

		if (!invoked)
			blog(LOG_WARNING,
			     "[obs-browser-hid-interact] could not run the widget scan on the GUI thread");
	}

	return result.isEmpty() ? NULL : bstrdup(result.toUtf8().constData());
}

/* ------------------------------------------------------------------ */
/* Click position capture                                              */
/* ------------------------------------------------------------------ */

/** Same layout math as frontend/utility/display-helpers.hpp. */
static void scale_and_center(int baseCX, int baseCY, int windowCX, int windowCY, int *x, int *y,
			     float *scale)
{
	const double windowAspect = double(windowCX) / double(windowCY);
	const double baseAspect = double(baseCX) / double(baseCY);
	int newCX, newCY;

	if (windowAspect > baseAspect) {
		*scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		*scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	*x = windowCX / 2 - newCX / 2;
	*y = windowCY / 2 - newCY / 2;
}

extern "C" bool obs_browser_hid_capture_interact_click(int global_x, int global_y, int *rel_x,
						       int *rel_y, char *source_name,
						       size_t source_name_size)
{
	QCoreApplication *app = QCoreApplication::instance();
	if (!app || QThread::currentThread() != app->thread())
		return false;

	const QString prefix = interaction_title_prefix();
	const QPoint global(global_x, global_y);

	for (QWidget *w : QApplication::allWidgets()) {
		if (!w->isWindow() || !w->isVisible() || !w->inherits("OBSBasicInteraction"))
			continue;

		/* ui->preview is the OBSQTDisplay that shows this source. */
		QWidget *preview = w->findChild<QWidget *>("preview");
		if (!preview || !preview->isVisible())
			continue;

		const QPoint local = preview->mapFromGlobal(global);
		if (!preview->rect().contains(local))
			continue;

		const QString name = interact_title_source_name(w, prefix);
		if (name.isEmpty())
			continue;

		obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
		if (!src || !is_browser_source_id(src)) {
			obs_source_release(src);
			continue;
		}
		uint32_t sourceCX = obs_source_get_width(src);
		uint32_t sourceCY = obs_source_get_height(src);
		obs_source_release(src);
		if (!sourceCX)
			sourceCX = 1;
		if (!sourceCY)
			sourceCY = 1;

		const QSize size = preview->size() * preview->devicePixelRatioF();
		if (size.width() <= 0 || size.height() <= 0)
			continue;

		/* obs-browser coordinates are pixels of the (unscaled) source
		 * canvas; OBS scales the widget coordinate by the device
		 * pixel ratio first, then un-does the aspect-fit transform. */
		const float dpr = float(w->devicePixelRatioF());
		const int mouseX = int(roundf(local.x() * dpr));
		const int mouseY = int(roundf(local.y() * dpr));

		int offX, offY;
		float scale;
		scale_and_center(int(sourceCX), int(sourceCY), size.width(), size.height(), &offX,
				 &offY, &scale);

		int x, y;
		if (offX > 0) {
			x = int(float(mouseX - offX) / scale);
			y = int(float(mouseY) / scale);
		} else {
			x = int(float(mouseX) / scale);
			y = int(float(mouseY - offY) / scale);
		}

		if (x < 0 || x > int(sourceCX) || y < 0 || y > int(sourceCY))
			continue;

		if (rel_x)
			*rel_x = x;
		if (rel_y)
			*rel_y = y;
		if (source_name && source_name_size)
			snprintf(source_name, source_name_size, "%s", name.toUtf8().constData());
		return true;
	}

	return false;
}
