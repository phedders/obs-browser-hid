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
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-frontend-api.h>

#include <QApplication>
#include <QMetaObject>
#include <QThread>
#include <QWidget>

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

/** Must be called from the Qt GUI thread. */
static bool scan_interact_widget(QWidget *w, const QString &prefix, QString *result)
{
	if (!w->isWindow() || !w->isVisible() || !w->inherits("OBSBasicInteraction"))
		return false;

	QString title = w->windowTitle();

	if (title.length() > prefix.length() && title.startsWith(prefix)) {
		QString name = title.mid(prefix.length());
		/* en-US pattern ends the title with a quote around the name */
		if (name.endsWith(u'\''))
			name.chop(1);
		if (!name.isEmpty()) {
			*result = name;
			return true;
		}
	}

	/* Locale fallback: no known prefix -> look for a browser source whose
	 * name is the title's suffix. */
	QString match;
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
	if (!sctx.match.isEmpty()) {
		*result = sctx.match;
		return true;
	}

	return false;
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
