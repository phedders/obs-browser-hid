/*
 * obs-browser-hid - "Browser HID" dock and quick-action settings dialog
 *
 * The dock targets a browser source (literal name, or the virtual names
 * "current" / "interact") and runs a user-configured list of keys/clicks
 * directly against it - no websocket round-trip. The same input functions
 * back the obs-websocket vendor, so behaviour is identical.
 *
 * Each action also gets an OBS frontend hotkey (bind it in Settings ->
 * Hotkeys); bindings are stored in the active profile and loaded here on
 * registration.
 *
 * Settings live in obs-browser-hid/settings.json under the OBS
 * plugin_config directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs-browser-hid.h"

#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace {

/* ------------------------------------------------------------------ */
/* Settings model                                                      */
/* ------------------------------------------------------------------ */

enum ActionType { ActionKey = 0, ActionMouse = 1 };

struct QuickAction {
	QString uuid;
	QString name;
	int type = ActionKey;

	/* shared */
	QString modifiers;
	int mode = OBS_BROWSER_HID_BOTH;

	/* key */
	QString key = "Return";
	QString text_override;

	/* mouse */
	int x = 0;
	int y = 0;
	QString button = "left";
	int click_count = 1;
};

constexpr const char *kConfigFile = "settings.json";
constexpr const char *kDockId = "obs-browser-hid";

QString new_uuid()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/** Repairs imported/hand-edited values so nothing can produce an unusable
 *  action (limits mirror the plugin-side input hardening). Returns true when
 *  something had to be changed (used to persist generated UUIDs). */
bool same_action(const QuickAction &a, const QuickAction &b)
{
	return a.uuid == b.uuid && a.name == b.name && a.type == b.type &&
	       a.modifiers == b.modifiers && a.mode == b.mode && a.key == b.key &&
	       a.text_override == b.text_override && a.x == b.x && a.y == b.y &&
	       a.button == b.button && a.click_count == b.click_count;
}

bool sanitize_action(QuickAction &a)
{
	const QuickAction before = a;

	a.name = a.name.left(64);
	if (a.name.isEmpty())
		a.name = QStringLiteral("Untitled action");
	if (a.type != ActionKey && a.type != ActionMouse)
		a.type = ActionKey;
	if (a.mode < OBS_BROWSER_HID_BOTH || a.mode > OBS_BROWSER_HID_RELEASE)
		a.mode = OBS_BROWSER_HID_BOTH;
	a.x = std::clamp(a.x, -1000000, 1000000);
	a.y = std::clamp(a.y, -1000000, 1000000);
	a.click_count = std::clamp(a.click_count, 1, 16);
	if (a.button != "left" && a.button != "middle" && a.button != "right")
		a.button = "left";
	a.key = a.key.left(128);
	if (a.key.isEmpty())
		a.key = "Return";
	a.modifiers = a.modifiers.left(255);
	a.text_override = a.text_override.left(4096);

	const QUuid parsed = QUuid::fromString(a.uuid);
	a.uuid = parsed.isNull() ? new_uuid() : parsed.toString(QUuid::WithoutBraces);

	return !same_action(a, before);
}

QString config_path()
{
	char *path = obs_module_config_path(kConfigFile);
	QString result = path ? QString::fromUtf8(path) : QString();
	bfree(path);
	return result;
}

void ensure_config_dir()
{
	char *dir = obs_module_config_path(nullptr);
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
}

void write_actions(obs_data_t *root, const std::vector<QuickAction> &actions)
{
	obs_data_array_t *arr = obs_data_array_create();

	for (const QuickAction &a : actions) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "uuid", a.uuid.toUtf8().constData());
		obs_data_set_string(item, "name", a.name.toUtf8().constData());
		obs_data_set_int(item, "type", a.type);
		obs_data_set_string(item, "modifiers", a.modifiers.toUtf8().constData());
		obs_data_set_int(item, "mode", a.mode);
		obs_data_set_string(item, "key", a.key.toUtf8().constData());
		obs_data_set_string(item, "text", a.text_override.toUtf8().constData());
		obs_data_set_int(item, "x", a.x);
		obs_data_set_int(item, "y", a.y);
		obs_data_set_string(item, "button", a.button.toUtf8().constData());
		obs_data_set_int(item, "clickCount", a.click_count);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}

	obs_data_set_array(root, "actions", arr);
	obs_data_array_release(arr);
}

bool read_actions(obs_data_t *root, std::vector<QuickAction> &out, QString *error,
		  bool *changed = nullptr)
{
	out.clear();
	bool dirty = false;

	if (!root) {
		if (error)
			*error = QStringLiteral("Empty or invalid JSON file.");
		return false;
	}

	obs_data_array_t *arr = obs_data_get_array(root, "actions");
	if (!arr) {
		if (error)
			*error = QStringLiteral("The file has no \"actions\" array.");
		return false;
	}

	QSet<QString> seen;
	const size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		QuickAction a;
		a.uuid = QString::fromUtf8(obs_data_get_string(item, "uuid"));
		a.name = QString::fromUtf8(obs_data_get_string(item, "name"));
		a.type = (int)obs_data_get_int(item, "type");
		a.modifiers = QString::fromUtf8(obs_data_get_string(item, "modifiers"));
		a.mode = (int)obs_data_get_int(item, "mode");
		a.key = QString::fromUtf8(obs_data_get_string(item, "key"));
		a.text_override = QString::fromUtf8(obs_data_get_string(item, "text"));
		a.x = (int)obs_data_get_int(item, "x");
		a.y = (int)obs_data_get_int(item, "y");
		a.button = QString::fromUtf8(obs_data_get_string(item, "button"));
		a.click_count = (int)obs_data_get_int(item, "clickCount");
		obs_data_release(item);

		dirty |= sanitize_action(a);
		while (seen.contains(a.uuid)) {
			a.uuid = new_uuid();
			dirty = true;
		}
		seen.insert(a.uuid);
		out.push_back(a);
	}

	obs_data_array_release(arr);
	if (changed)
		*changed = dirty;
	return true;
}

void load_settings(std::vector<QuickAction> &actions, QString &target,
		   bool *changed = nullptr)
{
	actions.clear();
	target = "current";
	if (changed)
		*changed = false;

	const QString path = config_path();
	if (path.isEmpty())
		return;

	obs_data_t *root = obs_data_create_from_json_file_safe(
		path.toUtf8().constData(), "bak");
	if (!root)
		return;

	bool dirty = false;
	QString error;
	read_actions(root, actions, &error, &dirty);

	const char *t = obs_data_get_string(root, "target");
	if (t && *t)
		target = QString::fromUtf8(t);

	obs_data_release(root);
	if (changed)
		*changed = dirty;
}

bool save_settings(const std::vector<QuickAction> &actions, const QString &target)
{
	obs_data_t *root = obs_data_create();
	write_actions(root, actions);
	obs_data_set_string(root, "target", target.toUtf8().constData());

	ensure_config_dir();
	const QString path = config_path();
	const bool ok = !path.isEmpty() &&
			obs_data_save_json_pretty_safe(root, path.toUtf8().constData(), "tmp", "bak");
	obs_data_release(root);

	if (!ok)
		blog(LOG_WARNING, "[obs-browser-hid] could not save %s", qUtf8Printable(path));
	return ok;
}

/* ------------------------------------------------------------------ */
/* Small shared widgets                                                */
/* ------------------------------------------------------------------ */

class ModifierBox : public QGroupBox {
public:
	explicit ModifierBox(QWidget *parent = nullptr) : QGroupBox("Modifiers", parent)
	{
		auto *layout = new QHBoxLayout(this);
		shift_ = new QCheckBox("Shift", this);
		ctrl_ = new QCheckBox("Ctrl", this);
		alt_ = new QCheckBox("Alt", this);
		super_ = new QCheckBox("Super", this);
		layout->addWidget(shift_);
		layout->addWidget(ctrl_);
		layout->addWidget(alt_);
		layout->addWidget(super_);
		layout->addStretch(1);
	}

	QString value() const
	{
		QStringList parts;
		if (ctrl_->isChecked())
			parts << "ctrl";
		if (shift_->isChecked())
			parts << "shift";
		if (alt_->isChecked())
			parts << "alt";
		if (super_->isChecked())
			parts << "super";
		return parts.join("+");
	}

	void set(const QString &spec)
	{
		const QStringList parts = spec.toLower().split('+', Qt::SkipEmptyParts);
		ctrl_->setChecked(parts.contains("ctrl") || parts.contains("control"));
		shift_->setChecked(parts.contains("shift"));
		alt_->setChecked(parts.contains("alt"));
		super_->setChecked(parts.contains("super") || parts.contains("meta") ||
				   parts.contains("win"));
	}

	void onChanged(QObject *ctx, const std::function<void()> &fn)
	{
		for (QCheckBox *box : {shift_, ctrl_, alt_, super_})
			QObject::connect(box, &QCheckBox::toggled, ctx, [fn](bool) { fn(); });
	}

private:
	QCheckBox *shift_ = nullptr;
	QCheckBox *ctrl_ = nullptr;
	QCheckBox *alt_ = nullptr;
	QCheckBox *super_ = nullptr;
};

QComboBox *make_mode_combo(int mode)
{
	auto *combo = new QComboBox;
	combo->addItem("Press and release", (int)OBS_BROWSER_HID_BOTH);
	combo->addItem("Press only", (int)OBS_BROWSER_HID_PRESS);
	combo->addItem("Release only", (int)OBS_BROWSER_HID_RELEASE);
	const int idx = combo->findData(mode);
	combo->setCurrentIndex(idx < 0 ? 0 : idx);
	return combo;
}

void set_mode_combo(QComboBox *combo, int mode)
{
	const int idx = combo->findData(mode);
	combo->setCurrentIndex(idx < 0 ? 0 : idx);
}

QComboBox *make_button_combo()
{
	auto *combo = new QComboBox;
	combo->addItem("Left", "left");
	combo->addItem("Middle", "middle");
	combo->addItem("Right", "right");
	return combo;
}

/** QListWidget that reports drops so the action vector can follow the
 *  visual order (the model has no reliable moveRows signal here). */
class ActionListWidget : public QListWidget {
public:
	explicit ActionListWidget(QWidget *parent = nullptr) : QListWidget(parent) {}
	std::function<void()> afterDrop;

protected:
	void dropEvent(QDropEvent *event) override
	{
		QListWidget::dropEvent(event);
		if (afterDrop)
			afterDrop();
	}
};

/* ------------------------------------------------------------------ */
/* Quick-action settings dialog                                        */
/* ------------------------------------------------------------------ */

class SettingsDialog : public QDialog {
public:
	enum { ResultCapture = 2 };

	explicit SettingsDialog(std::vector<QuickAction> actions, QWidget *parent,
				int select_row = 0)
		: QDialog(parent), actions_(std::move(actions))
	{
		setWindowTitle("Browser HID quick actions");
		setModal(true);
		resize(780, 500);
		build_ui();
		refill_list(actions_.empty() ? -1 : select_row);
	}

	const std::vector<QuickAction> &actions() const { return actions_; }
	int current_row() const { return list_->currentRow(); }

private:
	void build_ui()
	{
		auto *root = new QVBoxLayout(this);

		auto *header = new QLabel(
			"Configure the buttons shown in the dock. Each button sends one key "
			"(with modifiers) or one mouse click to the selected source. Drag "
			"rows to reorder them.",
			this);
		header->setWordWrap(true);
		root->addWidget(header);

		auto *body = new QHBoxLayout;

		/* left: list + list buttons */
		auto *left = new QVBoxLayout;
		list_ = new ActionListWidget(this);
		list_->setSelectionMode(QAbstractItemView::SingleSelection);
		list_->setDragDropMode(QAbstractItemView::InternalMove);
		list_->setDefaultDropAction(Qt::MoveAction);
		list_->setDragEnabled(true);
		list_->setAcceptDrops(true);
		list_->setDropIndicatorShown(true);
		list_->afterDrop = [this] { sync_order_from_list(); };
		left->addWidget(list_, 1);

		auto *listButtons = new QHBoxLayout;
		addButton_ = new QPushButton("Add", this);
		removeButton_ = new QPushButton("Remove", this);
		auto *upButton = new QPushButton("Up", this);
		auto *downButton = new QPushButton("Down", this);
		listButtons->addWidget(addButton_);
		listButtons->addWidget(removeButton_);
		listButtons->addWidget(upButton);
		listButtons->addWidget(downButton);
		left->addLayout(listButtons);

		auto *ioButtons = new QHBoxLayout;
		importButton_ = new QPushButton("Import...", this);
		exportButton_ = new QPushButton("Export...", this);
		ioButtons->addWidget(importButton_);
		ioButtons->addWidget(exportButton_);
		left->addLayout(ioButtons);
		body->addLayout(left, 1);

		/* right: editor */
		auto *right = new QVBoxLayout;
		auto *form = new QFormLayout;

		nameEdit_ = new QLineEdit(this);
		nameEdit_->setMaxLength(64);
		form->addRow("Button name", nameEdit_);

		typeCombo_ = new QComboBox(this);
		typeCombo_->addItem("Keyboard key", ActionKey);
		typeCombo_->addItem("Mouse click", ActionMouse);
		form->addRow("Type", typeCombo_);

		modifiers_ = new ModifierBox(this);
		form->addRow(QString(), modifiers_);

		stack_ = new QStackedWidget(this);

		/* key page */
		auto *keyPage = new QWidget;
		auto *keyForm = new QFormLayout(keyPage);
		keyForm->setContentsMargins(0, 0, 0, 0);
		keyEdit_ = new QLineEdit(keyPage);
		keyEdit_->setMaxLength(128);
		keyEdit_->setPlaceholderText("Return, Escape, F5, a, Left, space, ...");
		keyForm->addRow("Key", keyEdit_);
		textEdit_ = new QLineEdit(keyPage);
		textEdit_->setPlaceholderText("Optional: single character to type instead");
		keyForm->addRow("Text override", textEdit_);
		stack_->addWidget(keyPage);

		/* mouse page */
		auto *mousePage = new QWidget;
		auto *mouseForm = new QFormLayout(mousePage);
		mouseForm->setContentsMargins(0, 0, 0, 0);
		xSpin_ = new QSpinBox(mousePage);
		xSpin_->setRange(-1000000, 1000000);
		ySpin_ = new QSpinBox(mousePage);
		ySpin_->setRange(-1000000, 1000000);
		mouseForm->addRow("X position", xSpin_);
		mouseForm->addRow("Y position", ySpin_);
		captureButton_ = new QPushButton("Capture from interact window...", mousePage);
		captureButton_->setToolTip(
			"Temporarily closes this dialog: click inside the source's interact "
			"window to fill X/Y");
		mouseForm->addRow(QString(), captureButton_);
		buttonCombo_ = make_button_combo();
		mouseForm->addRow("Button", buttonCombo_);
		clickSpin_ = new QSpinBox(mousePage);
		clickSpin_->setRange(1, 16);
		clickSpin_->setToolTip("2 = double click");
		mouseForm->addRow("Click count", clickSpin_);
		stack_->addWidget(mousePage);

		form->addRow(stack_);

		modeCombo_ = make_mode_combo(OBS_BROWSER_HID_BOTH);
		form->addRow("Emit", modeCombo_);

		auto *hint = new QLabel(
			"Coordinates are in the source's own canvas, (0,0) = top-left. Use "
			"\"Capture from interact window...\" to fill them from a real click.",
			this);
		hint->setWordWrap(true);
		form->addRow(QString(), hint);

		auto *hotkeyHint = new QLabel(
			"Hotkeys: after saving, bind a key for each action under Settings "
			"> Hotkeys (listed as \"Browser HID: <action name>\").",
			this);
		hotkeyHint->setWordWrap(true);
		form->addRow(QString(), hotkeyHint);

		right->addLayout(form);
		right->addStretch(1);
		body->addLayout(right, 2);

		root->addLayout(body, 1);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
						     QDialogButtonBox::Cancel, this);
		root->addWidget(buttons);

		/* wiring */
		connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { load_row(row); });
		connect(addButton_, &QPushButton::clicked, this, [this] { add_action(); });
		connect(removeButton_, &QPushButton::clicked, this, [this] { remove_action(); });
		connect(upButton, &QPushButton::clicked, this, [this] { move_action(-1); });
		connect(downButton, &QPushButton::clicked, this, [this] { move_action(1); });
		connect(importButton_, &QPushButton::clicked, this, [this] { import_actions(); });
		connect(exportButton_, &QPushButton::clicked, this, [this] { export_actions(); });
		connect(captureButton_, &QPushButton::clicked, this, [this] {
			if (loading_)
				return;
			store_current();
			done(ResultCapture);
		});

		auto onEdit = [this] {
			if (loading_)
				return;
			store_current();
			update_row_label(list_->currentRow());
		};
		connect(nameEdit_, &QLineEdit::textChanged, this, [onEdit](const QString &) { onEdit(); });
		connect(keyEdit_, &QLineEdit::textChanged, this, [onEdit](const QString &) { onEdit(); });
		connect(textEdit_, &QLineEdit::textChanged, this, [onEdit](const QString &) { onEdit(); });
		connect(typeCombo_, &QComboBox::currentIndexChanged, this, [this, onEdit](int) {
			update_stack();
			onEdit();
		});
		connect(modeCombo_, &QComboBox::currentIndexChanged, this, [onEdit](int) { onEdit(); });
		connect(buttonCombo_, &QComboBox::currentIndexChanged, this, [onEdit](int) { onEdit(); });
		connect(xSpin_, &QSpinBox::valueChanged, this, [onEdit](int) { onEdit(); });
		connect(ySpin_, &QSpinBox::valueChanged, this, [onEdit](int) { onEdit(); });
		connect(clickSpin_, &QSpinBox::valueChanged, this, [onEdit](int) { onEdit(); });
		modifiers_->onChanged(this, onEdit);

		connect(buttons, &QDialogButtonBox::accepted, this, [this] {
			if (!loading_)
				store_current();
			accept();
		});
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	}

	void update_stack()
	{
		stack_->setCurrentIndex(typeCombo_->currentIndex() == ActionMouse ? 1 : 0);
	}

	void refill_list(int select)
	{
		loading_ = true;
		list_->clear();
		for (const QuickAction &a : actions_) {
			auto *item = new QListWidgetItem(a.name, list_);
			item->setData(Qt::UserRole, a.uuid);
			item->setFlags(item->flags() | Qt::ItemIsDragEnabled |
					Qt::ItemIsDropEnabled);
		}
		loading_ = false;

		const bool hasActions = !actions_.empty();
		set_editors_enabled(hasActions);
		exportButton_->setEnabled(hasActions);

		if (!hasActions) {
			clear_editors();
			list_->setCurrentRow(-1);
			return;
		}

		select = std::clamp(select, 0, (int)actions_.size() - 1);
		list_->setCurrentRow(select);
	}

	void set_editors_enabled(bool enabled)
	{
		nameEdit_->setEnabled(enabled);
		typeCombo_->setEnabled(enabled);
		modifiers_->setEnabled(enabled);
		stack_->setEnabled(enabled);
		modeCombo_->setEnabled(enabled);
		removeButton_->setEnabled(enabled);
		captureButton_->setEnabled(enabled);
	}

	void clear_editors()
	{
		loading_ = true;
		nameEdit_->clear();
		keyEdit_->clear();
		textEdit_->clear();
		modifiers_->set(QString());
		set_mode_combo(modeCombo_, OBS_BROWSER_HID_BOTH);
		buttonCombo_->setCurrentIndex(0);
		xSpin_->setValue(0);
		ySpin_->setValue(0);
		clickSpin_->setValue(1);
		typeCombo_->setCurrentIndex(0);
		update_stack();
		loading_ = false;
	}

	void load_row(int row)
	{
		if (row < 0 || row >= (int)actions_.size()) {
			clear_editors();
			set_editors_enabled(false);
			return;
		}

		loading_ = true;
		const QuickAction &a = actions_[row];
		nameEdit_->setText(a.name);
		typeCombo_->setCurrentIndex(a.type == ActionMouse ? 1 : 0);
		modifiers_->set(a.modifiers);
		set_mode_combo(modeCombo_, a.mode);
		keyEdit_->setText(a.key);
		textEdit_->setText(a.text_override);
		xSpin_->setValue(a.x);
		ySpin_->setValue(a.y);
		buttonCombo_->setCurrentIndex(std::max(0, buttonCombo_->findData(a.button)));
		clickSpin_->setValue(a.click_count);
		update_stack();
		loading_ = false;
	}

	void store_current()
	{
		const int row = list_->currentRow();
		if (row < 0 || row >= (int)actions_.size())
			return;

		QuickAction &a = actions_[row];
		a.name = nameEdit_->text().trimmed();
		a.type = typeCombo_->currentIndex() == 1 ? ActionMouse : ActionKey;
		a.modifiers = modifiers_->value();
		a.mode = modeCombo_->currentData().toInt();
		a.key = keyEdit_->text().trimmed();
		a.text_override = textEdit_->text();
		a.x = xSpin_->value();
		a.y = ySpin_->value();
		a.button = buttonCombo_->currentData().toString();
		a.click_count = clickSpin_->value();
	}

	void update_row_label(int row)
	{
		if (row < 0 || row >= list_->count())
			return;
		QListWidgetItem *item = list_->item(row);
		if (!item)
			return;
		const QString name = nameEdit_->text().trimmed();
		item->setText(name.isEmpty() ? QStringLiteral("(unnamed action)") : name);
	}

	/** Mirror the visual list order into `actions_` after a drag/drop. */
	void sync_order_from_list()
	{
		if (actions_.empty() || list_->count() != (int)actions_.size())
			return;

		std::vector<QuickAction> reordered;
		reordered.reserve(actions_.size());
		for (int i = 0; i < list_->count(); i++) {
			const QString uuid = list_->item(i)->data(Qt::UserRole).toString();
			const auto it = std::find_if(actions_.begin(), actions_.end(),
						     [&uuid](const QuickAction &a) {
							     return a.uuid == uuid;
						     });
			if (it != actions_.end())
				reordered.push_back(*it);
		}
		if (reordered.size() != actions_.size())
			return;

		actions_ = std::move(reordered);
		load_row(std::clamp(list_->currentRow(), 0, (int)actions_.size() - 1));
	}

	void add_action()
	{
		QuickAction a;
		a.uuid = new_uuid();
		a.name = "New action";
		const int row = list_->currentRow();
		const int at = row < 0 ? (int)actions_.size() : row + 1;
		actions_.insert(actions_.begin() + at, a);
		refill_list(at);
	}

	void remove_action()
	{
		const int row = list_->currentRow();
		if (row < 0 || row >= (int)actions_.size())
			return;
		actions_.erase(actions_.begin() + row);
		refill_list(row);
	}

	void move_action(int delta)
	{
		const int row = list_->currentRow();
		const int dst = row + delta;
		if (row < 0 || dst < 0 || dst >= (int)actions_.size())
			return;
		std::swap(actions_[row], actions_[dst]);
		refill_list(dst);
	}

	void import_actions()
	{
		const QString path = QFileDialog::getOpenFileName(
			this, "Import quick actions", QString(),
			"JSON files (*.json);;All files (*)");
		if (path.isEmpty())
			return;

		obs_data_t *root = obs_data_create_from_json_file_safe(
			path.toUtf8().constData(), "bak");
		std::vector<QuickAction> imported;
		QString error;
		const bool ok = read_actions(root, imported, &error);
		obs_data_release(root);

		if (!ok) {
			QMessageBox::warning(this, "Import quick actions", error);
			return;
		}
		if (imported.empty()) {
			QMessageBox::information(this, "Import quick actions",
						 "The file contains no actions.");
			return;
		}

		bool replace = actions_.empty();
		if (!actions_.empty()) {
			QMessageBox box(QMessageBox::Question, "Import quick actions",
					QString("The file contains %1 action(s). Append them "
						"to the current list or replace it?")
						.arg(imported.size()),
					QMessageBox::NoButton, this);
			QAbstractButton *appendButton =
				box.addButton("Append", QMessageBox::AcceptRole);
			QAbstractButton *replaceButton =
				box.addButton("Replace", QMessageBox::DestructiveRole);
			box.addButton(QMessageBox::Cancel);
			box.exec();
			if (box.clickedButton() == replaceButton)
				replace = true;
			else if (box.clickedButton() != appendButton)
				return;
		}

		if (replace) {
			actions_ = std::move(imported);
		} else {
			QSet<QString> seen;
			for (const QuickAction &a : actions_)
				seen.insert(a.uuid);
			for (QuickAction a : imported) {
				while (seen.contains(a.uuid))
					a.uuid = new_uuid();
				seen.insert(a.uuid);
				actions_.push_back(a);
			}
		}

		refill_list(actions_.empty() ? -1 : (int)actions_.size() - 1);
	}

	void export_actions()
	{
		const QString path = QFileDialog::getSaveFileName(
			this, "Export quick actions", "browser-hid-actions.json",
			"JSON files (*.json);;All files (*)");
		if (path.isEmpty())
			return;

		obs_data_t *root = obs_data_create();
		write_actions(root, actions_);
		const bool ok = obs_data_save_json_pretty_safe(
			root, path.toUtf8().constData(), "tmp", "bak");
		obs_data_release(root);

		if (!ok)
			QMessageBox::warning(this, "Export quick actions",
					     "Could not write the selected file.");
	}

	std::vector<QuickAction> actions_;
	bool loading_ = false;

	ActionListWidget *list_ = nullptr;
	QPushButton *addButton_ = nullptr;
	QPushButton *removeButton_ = nullptr;
	QPushButton *importButton_ = nullptr;
	QPushButton *exportButton_ = nullptr;
	QLineEdit *nameEdit_ = nullptr;
	QComboBox *typeCombo_ = nullptr;
	ModifierBox *modifiers_ = nullptr;
	QStackedWidget *stack_ = nullptr;
	QLineEdit *keyEdit_ = nullptr;
	QLineEdit *textEdit_ = nullptr;
	QSpinBox *xSpin_ = nullptr;
	QSpinBox *ySpin_ = nullptr;
	QPushButton *captureButton_ = nullptr;
	QComboBox *buttonCombo_ = nullptr;
	QSpinBox *clickSpin_ = nullptr;
	QComboBox *modeCombo_ = nullptr;
};

/* ------------------------------------------------------------------ */
/* Hotkeys                                                             */
/* ------------------------------------------------------------------ */

class HidDock;

struct HotkeyReg {
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	QString uuid;
	HidDock *dock = nullptr;
};

void hotkey_callback(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed);

/** Frontend hotkey bindings live in the active profile config, keyed by the
 *  registered hotkey name; OBS only auto-loads its own hotkeys, so plugins
 *  have to restore theirs when (re)registering. */
void load_hotkey_bindings(const QString &name, obs_hotkey_id id)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	const char *json = config_get_string(config, "Hotkeys", name.toUtf8().constData());
	if (!json)
		return;

	obs_data_t *data = obs_data_create_from_json(json);
	if (!data)
		return;

	obs_data_array_t *bindings = obs_data_get_array(data, "bindings");
	if (bindings) {
		obs_hotkey_load(id, bindings);
		obs_data_array_release(bindings);
	}
	obs_data_release(data);
}

/* ------------------------------------------------------------------ */
/* Dock                                                                */
/* ------------------------------------------------------------------ */

class HidDock : public QWidget {
public:
	explicit HidDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		build_ui();
		bool settings_changed = false;
		load_settings(actions_, initialTarget_, &settings_changed);
		refresh_sources(initialTarget_);
		rebuild_actions();
		sync_hotkeys();
		/* Persist generated UUIDs (old files have none) so the
		 * hotkey names - and therefore saved bindings - stay stable
		 * across restarts. */
		if (settings_changed)
			save_settings(actions_, initialTarget_);
		set_status(QStringLiteral("Ready."), false);
	}

	~HidDock() override
	{
		stop_capture_ui();
		for (const auto &reg : hotkeys_)
			obs_hotkey_unregister(reg->id);
		hotkeys_.clear();
	}

	void refresh_sources(const QString &want)
	{
		QStringList names;
		obs_enum_sources(enum_browser_sources, &names);

		std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
			return QString::compare(a, b, Qt::CaseInsensitive) < 0;
		});

		const QString keep = want.isEmpty() ? selected_target() : want;

		const QSignalBlocker block(target_);
		target_->clear();
		target_->addItem("current  -  first browser source in the programme scene",
				 "current");
		target_->addItem("interact  -  browser source with an open interact window",
				 "interact");
		target_->insertSeparator(target_->count());
		for (const QString &name : names)
			target_->addItem(name, name);

		const int idx = target_->findData(keep);
		target_->setCurrentIndex(idx < 0 ? 0 : idx);
	}

	void refresh_from_frontend()
	{
		refresh_sources(QString());
	}

	/** Runs the action whose stable UUID matches (call on the GUI thread). */
	void trigger_hotkey(const QString &uuid)
	{
		if (captureActive_)
			return;
		for (int i = 0; i < (int)actions_.size(); i++) {
			if (actions_[i].uuid == uuid) {
				run_action(i);
				return;
			}
		}
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		/* the dock is created hidden; recompute the button columns
		 * once it has a real viewport width */
		QTimer::singleShot(0, this, [this] { relayout_actions(); });
	}

private:
	static bool enum_browser_sources(void *param, obs_source_t *source)
	{
		auto *names = static_cast<QStringList *>(param);
		const char *id = obs_source_get_unversioned_id(source);
		if (!id || strcmp(id, "browser_source") != 0)
			return true;
		const char *name = obs_source_get_name(source);
		if (name && *name) {
			const QString qname = QString::fromUtf8(name);
			if (!names->contains(qname))
				names->push_back(qname);
		}
		return true;
	}

	QString selected_target() const
	{
		const QString target = target_->currentData().toString();
		return target.isEmpty() ? QStringLiteral("current") : target;
	}

	void build_ui()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);

		auto *targetGroup = new QGroupBox("Target browser source", this);
		auto *targetLayout = new QVBoxLayout(targetGroup);

		target_ = new QComboBox(targetGroup);
		target_->setMinimumContentsLength(24);
		target_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		targetLayout->addWidget(target_);

		auto *targetButtons = new QHBoxLayout;
		auto *currentButton = new QPushButton("Use current", targetGroup);
		auto *interactButton = new QPushButton("Use interact", targetGroup);
		auto *rescanButton = new QPushButton("Rescan", targetGroup);
		currentButton->setToolTip("First browser source in the programme scene");
		interactButton->setToolTip("Browser source with an open interact window");
		rescanButton->setToolTip("Rescan browser sources");
		targetButtons->addWidget(currentButton);
		targetButtons->addWidget(interactButton);
		targetButtons->addStretch(1);
		targetButtons->addWidget(rescanButton);
		targetLayout->addLayout(targetButtons);
		root->addWidget(targetGroup);

		auto *actionsGroup = new QGroupBox("Quick actions", this);
		auto *actionsLayout = new QVBoxLayout(actionsGroup);

		actionsScroll_ = new QScrollArea(actionsGroup);
		actionsScroll_->setWidgetResizable(true);
		actionsScroll_->setFrameShape(QFrame::NoFrame);
		actionsHost_ = new QWidget;
		actionsGrid_ = new QGridLayout(actionsHost_);
		actionsGrid_->setContentsMargins(0, 0, 0, 0);
		actionsGrid_->setSpacing(6);
		actionsGrid_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
		actionsScroll_->setWidget(actionsHost_);
		actionsScroll_->viewport()->installEventFilter(this);
		actionsLayout->addWidget(actionsScroll_, 1);

		auto *bottom = new QHBoxLayout;
		settingsButton_ = new QPushButton("Settings...", actionsGroup);
		bottom->addWidget(settingsButton_);
		bottom->addStretch(1);
		actionsLayout->addLayout(bottom);
		root->addWidget(actionsGroup, 1);

		status_ = new QLabel(this);
		status_->setWordWrap(true);
		status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		root->addWidget(status_);

		/* wiring */
		connect(currentButton, &QPushButton::clicked, this, [this] {
			const int idx = target_->findData("current");
			if (idx >= 0)
				target_->setCurrentIndex(idx);
		});
		connect(interactButton, &QPushButton::clicked, this, [this] {
			const int idx = target_->findData("interact");
			if (idx >= 0)
				target_->setCurrentIndex(idx);
		});
		connect(rescanButton, &QPushButton::clicked, this,
			[this] { refresh_sources(selected_target()); });
		connect(settingsButton_, &QPushButton::clicked, this, [this] { open_settings(); });
		connect(target_, &QComboBox::currentIndexChanged, this, [this](int) {
			save_settings(actions_, selected_target());
		});
	}

	void clear_actions_layout()
	{
		while (QLayoutItem *item = actionsGrid_->takeAt(0)) {
			if (QWidget *widget = item->widget())
				widget->deleteLater();
			delete item;
		}
	}

	/** Packs the fixed-width buttons into as many columns as the dock width
	 *  allows; all buttons use the widest label so the grid stays tidy. */
	void relayout_actions()
	{
		if (actionButtons_.empty() || !actionsScroll_)
			return;

		int available = actionsScroll_->viewport()->width();
		if (available <= 0)
			available = width();
		const int spacing = std::max(actionsGrid_->spacing(), 1);
		const int cell = std::max(actionButtonWidth_, 1);
		int columns = (available + spacing) / (cell + spacing);
		columns = std::clamp(columns, 1, (int)actionButtons_.size());
		if (columns == actionColumns_)
			return;
		actionColumns_ = columns;

		for (QPushButton *button : actionButtons_)
			actionsGrid_->removeWidget(button);
		for (int i = 0; i < (int)actionButtons_.size(); i++)
			actionsGrid_->addWidget(actionButtons_[i], i / columns, i % columns);
	}

	void rebuild_actions()
	{
		clear_actions_layout();
		actionButtons_.clear();
		actionButtonWidth_ = 0;
		actionColumns_ = -1;

		if (actions_.empty()) {
			auto *hint = new QLabel(
				"No quick actions yet - use \"Settings...\" to add keys or "
				"clicks.",
				actionsHost_);
			hint->setWordWrap(true);
			hint->setEnabled(false);
			actionsGrid_->addWidget(hint, 0, 0, 1, 2);
			return;
		}

		actionButtons_.reserve(actions_.size());
		for (int i = 0; i < (int)actions_.size(); i++) {
			const QuickAction &action = actions_[i];
			auto *button = new QPushButton(action.name, actionsHost_);
			button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
			button->setToolTip(action.type == ActionKey
						   ? QString("Key: %1 %2")
							     .arg(action.key, action.modifiers)
						   : QString("Click %1 at %2,%3")
							     .arg(action.button)
							     .arg(action.x)
							     .arg(action.y));
			connect(button, &QPushButton::clicked, this, [this, i] { run_action(i); });
			actionButtons_.push_back(button);
			actionButtonWidth_ = std::max(actionButtonWidth_, button->sizeHint().width());
		}

		for (QPushButton *button : actionButtons_)
			button->setFixedWidth(actionButtonWidth_);

		relayout_actions();
	}

	void run_action(int index)
	{
		if (index < 0 || index >= (int)actions_.size())
			return;

		const QString target = selected_target();
		const QuickAction &action = actions_[index];
		const QByteArray targetUtf8 = target.toUtf8();
		const QByteArray modifiersUtf8 = action.modifiers.toUtf8();
		char err[512] = {0};

		bool ok;
		if (action.type == ActionMouse) {
			const QByteArray buttonUtf8 = action.button.toUtf8();
			ok = obs_browser_hid_send_mouse_click(targetUtf8.constData(), nullptr,
							      action.x, action.y,
							      buttonUtf8.constData(),
							      action.click_count,
							      modifiersUtf8.constData(),
							      action.mode, false, err, sizeof(err));
		} else {
			const QByteArray keyUtf8 = action.key.toUtf8();
			const QByteArray textUtf8 = action.text_override.toUtf8();
			ok = obs_browser_hid_send_key(targetUtf8.constData(), nullptr,
						      keyUtf8.constData(),
						      modifiersUtf8.constData(), action.mode,
						      action.text_override.isEmpty()
							      ? nullptr
							      : textUtf8.constData(),
						      err, sizeof(err));
		}

		if (ok)
			set_status(QString("Sent '%1' to %2.").arg(action.name, target), false);
		else
			set_status(QString::fromUtf8(err), true);
	}

	void apply_actions(std::vector<QuickAction> next)
	{
		for (QuickAction &action : next)
			sanitize_action(action);
		actions_ = std::move(next);
		rebuild_actions();
		sync_hotkeys();
		if (!save_settings(actions_, selected_target()))
			set_status("Could not save settings (see OBS log).", true);
	}

	void open_settings(int select_row = 0)
	{
		open_settings_with(actions_, select_row);
	}

	void open_settings_with(std::vector<QuickAction> working, int select_row)
	{
		SettingsDialog dialog(std::move(working), this, select_row);
		const int result = dialog.exec();

		if (result == SettingsDialog::ResultCapture) {
			pendingActions_ = dialog.actions();
			pendingRow_ = dialog.current_row();
			start_capture();
			return;
		}
		if (result != QDialog::Accepted) {
			pendingActions_.clear();
			return;
		}

		apply_actions(dialog.actions());
		pendingActions_.clear();
	}

	/* ---- click position capture ---- */

	void reopen_settings_soon(int row)
	{
		const std::vector<QuickAction> next = pendingActions_;
		QTimer::singleShot(0, this,
				   [this, next, row] { open_settings_with(next, row); });
	}

	void start_capture()
	{
		if (pendingRow_ < 0 || pendingRow_ >= (int)pendingActions_.size())
			return;
		if (pendingActions_[pendingRow_].type != ActionMouse) {
			set_status("Position capture is only available for mouse actions.", true);
			reopen_settings_soon(pendingRow_);
			return;
		}

		char *interact_name = obs_browser_hid_interact_source_name();
		const bool has_interact = interact_name != nullptr;
		bfree(interact_name);
		if (!has_interact) {
			set_status("No interact window is open - open one for the source "
				   "first, then capture again.",
				   true);
			reopen_settings_soon(pendingRow_);
			return;
		}

		stop_capture_ui();
		captureActive_ = true;
		const int token = ++captureToken_;
		qApp->installEventFilter(this);

		captureHint_ = new QLabel(
			"Click inside the browser source's interact window to capture the "
			"position.\nPress Esc to cancel. The click is not sent to the page.",
			nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
		captureHint_->setAttribute(Qt::WA_ShowWithoutActivating);
		captureHint_->setAlignment(Qt::AlignCenter);
		captureHint_->setMargin(12);
		captureHint_->setStyleSheet(
			"background:#202020; color:#ffffff; border:1px solid #777777;");
		captureHint_->adjustSize();
		captureHint_->move(QCursor::pos() + QPoint(24, 24));
		captureHint_->show();

		set_status("Capture mode: click in the interact window (Esc cancels).", false);

		QTimer::singleShot(30000, this, [this, token] {
			if (captureActive_ && token == captureToken_)
				cancel_capture();
		});
	}

	void stop_capture_ui()
	{
		++captureToken_;
		captureActive_ = false;
		capturePressValid_ = false;
		captureSourceName_.clear();
		if (qApp)
			qApp->removeEventFilter(this);
		if (captureHint_) {
			captureHint_->hide();
			delete captureHint_;
			captureHint_ = nullptr;
		}
	}

	void finish_capture(int x, int y, const QString &source_name)
	{
		stop_capture_ui();
		if (pendingRow_ >= 0 && pendingRow_ < (int)pendingActions_.size()) {
			pendingActions_[pendingRow_].x = x;
			pendingActions_[pendingRow_].y = y;
		}
		set_status(QString("Captured %1,%2 from '%3'.")
				   .arg(x)
				   .arg(y)
				   .arg(source_name),
			   false);

		reopen_settings_soon(pendingRow_);
	}

	void cancel_capture()
	{
		stop_capture_ui();
		set_status("Position capture cancelled.", false);
		reopen_settings_soon(pendingRow_);
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (actionsScroll_ && watched == actionsScroll_->viewport() &&
		    event->type() == QEvent::Resize) {
			relayout_actions();
			return false;
		}

		if (!captureActive_)
			return QWidget::eventFilter(watched, event);

		if (event->type() == QEvent::KeyPress) {
			auto *key = static_cast<QKeyEvent *>(event);
			if (key->key() == Qt::Key_Escape) {
				cancel_capture();
				return true;
			}
		} else if (event->type() == QEvent::MouseButtonPress) {
			auto *mouse = static_cast<QMouseEvent *>(event);
			const QPoint global = mouse->globalPosition().toPoint();
			int x = 0, y = 0;
			char source_name[256] = {0};
			if (obs_browser_hid_capture_interact_click(
				    global.x(), global.y(), &x, &y, source_name,
				    sizeof(source_name))) {
				capturePressValid_ = true;
				captureX_ = x;
				captureY_ = y;
				captureSourceName_ = QString::fromUtf8(source_name);
				/* swallow the press so page controls do not
				 * activate; the matching release is swallowed
				 * below and completes the capture */
				return true;
			}
		} else if (event->type() == QEvent::MouseButtonRelease) {
			if (capturePressValid_) {
				capturePressValid_ = false;
				finish_capture(captureX_, captureY_, captureSourceName_);
				return true;
			}
		}

		return QWidget::eventFilter(watched, event);
	}

	/* ---- hotkeys ---- */

	void sync_hotkeys()
	{
		for (auto it = hotkeys_.begin(); it != hotkeys_.end();) {
			const QString uuid = (*it)->uuid;
			const bool present = std::any_of(
				actions_.begin(), actions_.end(),
				[&uuid](const QuickAction &a) { return a.uuid == uuid; });
			if (!present) {
				obs_hotkey_unregister((*it)->id);
				it = hotkeys_.erase(it);
			} else {
				++it;
			}
		}

		for (const QuickAction &action : actions_) {
			const auto found = std::find_if(
				hotkeys_.begin(), hotkeys_.end(),
				[&action](const std::unique_ptr<HotkeyReg> &reg) {
					return reg->uuid == action.uuid;
				});
			const QString desc =
				QString("Browser HID: %1")
					.arg(action.name.isEmpty()
						     ? QStringLiteral("Untitled action")
						     : action.name);
			if (found != hotkeys_.end()) {
				/* renaming only changes the description; the
				 * hotkey name (UUID) and bindings stay. */
				obs_hotkey_set_description((*found)->id,
							   desc.toUtf8().constData());
				continue;
			}

			auto reg = std::make_unique<HotkeyReg>();
			reg->uuid = action.uuid;
			reg->dock = this;
			const QString name = QString("obs-browser-hid.action.%1").arg(action.uuid);
			reg->id = obs_hotkey_register_frontend(
				name.toUtf8().constData(), desc.toUtf8().constData(),
				hotkey_callback, reg.get());
			if (reg->id == OBS_INVALID_HOTKEY_ID) {
				blog(LOG_WARNING,
				     "[obs-browser-hid] could not register hotkey '%s'",
				     qUtf8Printable(name));
				continue;
			}
			load_hotkey_bindings(name, reg->id);
			hotkeys_.push_back(std::move(reg));
		}
	}

	void set_status(const QString &text, bool error)
	{
		status_->setText(text);
		status_->setStyleSheet(error ? "color: #c0392b;" : QString());
	}

	QString initialTarget_;

	QComboBox *target_ = nullptr;
	QWidget *actionsHost_ = nullptr;
	QScrollArea *actionsScroll_ = nullptr;
	QGridLayout *actionsGrid_ = nullptr;
	std::vector<QPushButton *> actionButtons_;
	int actionButtonWidth_ = 0;
	int actionColumns_ = -1;
	QPushButton *settingsButton_ = nullptr;
	QLabel *status_ = nullptr;

	std::vector<QuickAction> actions_;

	/* per-action hotkey registrations, keyed by action UUID */
	std::vector<std::unique_ptr<HotkeyReg>> hotkeys_;

	/* position-capture state */
	bool captureActive_ = false;
	int captureToken_ = 0;
	QLabel *captureHint_ = nullptr;
	bool capturePressValid_ = false;
	int captureX_ = 0;
	int captureY_ = 0;
	QString captureSourceName_;
	std::vector<QuickAction> pendingActions_;
	int pendingRow_ = 0;
};

void hotkey_callback(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;

	auto *reg = static_cast<HotkeyReg *>(data);
	if (!reg || !reg->dock)
		return;

	HidDock *dock = reg->dock;
	const QString uuid = reg->uuid;

	/* OBS routes hotkeys to the GUI thread via OBSBasic, but keep this
	 * robust for hosts/configurations without rerouting. */
	if (QThread::currentThread() == dock->thread()) {
		dock->trigger_hotkey(uuid);
	} else {
		QMetaObject::invokeMethod(
			dock, [dock, uuid] { dock->trigger_hotkey(uuid); },
			Qt::QueuedConnection);
	}
}

HidDock *g_dock = nullptr;

void frontend_event(enum obs_frontend_event event, void *data)
{
	auto *dock = static_cast<HidDock *>(data);
	if (!dock)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		dock->refresh_from_frontend();
		break;
	default:
		break;
	}
}

} /* namespace */

extern "C" void obs_browser_hid_ui_init(void)
{
	if (g_dock)
		return;

	if (!QApplication::instance()) {
		blog(LOG_DEBUG, "[obs-browser-hid] no Qt application; dock not created");
		return;
	}

	g_dock = new HidDock(nullptr);
	if (!obs_frontend_add_dock_by_id(kDockId, "Browser HID", g_dock)) {
		blog(LOG_WARNING, "[obs-browser-hid] could not register the dock");
		delete g_dock;
		g_dock = nullptr;
		return;
	}

	obs_frontend_add_event_callback(frontend_event, g_dock);
	blog(LOG_INFO,
	     "[obs-browser-hid] dock registered (View -> Docks -> Browser HID)");
}
