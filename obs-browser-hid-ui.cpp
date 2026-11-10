/*
 * obs-browser-hid - "Browser HID" dock and quick-action settings dialog
 *
 * The dock targets a browser source (literal name, or the virtual names
 * "current" / "interact") and runs a user-configured list of keys/clicks
 * directly against it - no websocket round-trip. The same input functions
 * back the obs-websocket vendor, so behaviour is identical.
 *
 * Settings live in obs-browser-hid/settings.json under the OBS
 * plugin_config directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs-browser-hid.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

namespace {

/* ------------------------------------------------------------------ */
/* Settings model                                                      */
/* ------------------------------------------------------------------ */

enum ActionType { ActionKey = 0, ActionMouse = 1 };

struct QuickAction {
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

void load_settings(std::vector<QuickAction> &actions, QString &target)
{
	actions.clear();
	target = "current";

	const QString path = config_path();
	if (path.isEmpty())
		return;

	obs_data_t *root = obs_data_create_from_json_file_safe(
		path.toUtf8().constData(), "bak");
	if (!root)
		return;

	obs_data_array_t *arr = obs_data_get_array(root, "actions");
	if (arr) {
		const size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			QuickAction a;
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

			/* sanitize on load: settings are a local file, but a
			 * corrupt one should not create unusable buttons */
			if (a.name.isEmpty())
				continue;
			if (a.type != ActionKey && a.type != ActionMouse)
				a.type = ActionKey;
			if (a.mode < OBS_BROWSER_HID_BOTH || a.mode > OBS_BROWSER_HID_RELEASE)
				a.mode = OBS_BROWSER_HID_BOTH;
			if (a.click_count < 1)
				a.click_count = 1;
			if (a.button.isEmpty())
				a.button = "left";
			if (a.key.isEmpty())
				a.key = "Return";

			actions.push_back(a);
		}
		obs_data_array_release(arr);
	}

	const char *t = obs_data_get_string(root, "target");
	if (t && *t)
		target = QString::fromUtf8(t);

	obs_data_release(root);
}

bool save_settings(const std::vector<QuickAction> &actions, const QString &target)
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();

	for (const QuickAction &a : actions) {
		obs_data_t *item = obs_data_create();
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

/* ------------------------------------------------------------------ */
/* Quick-action settings dialog                                        */
/* ------------------------------------------------------------------ */

class SettingsDialog : public QDialog {
public:
	explicit SettingsDialog(std::vector<QuickAction> actions, QWidget *parent)
		: QDialog(parent), actions_(std::move(actions))
	{
		setWindowTitle("Browser HID quick actions");
		setModal(true);
		resize(760, 440);
		build_ui();
		refill_list(actions_.empty() ? -1 : 0);
	}

	const std::vector<QuickAction> &actions() const { return actions_; }

private:
	void build_ui()
	{
		auto *root = new QVBoxLayout(this);

		auto *header = new QLabel(
			"Configure the buttons shown in the dock. Each button sends one key "
			"(with modifiers) or one mouse click to the selected source.",
			this);
		header->setWordWrap(true);
		root->addWidget(header);

		auto *body = new QHBoxLayout;

		/* left: list + list buttons */
		auto *left = new QVBoxLayout;
		list_ = new QListWidget(this);
		list_->setSelectionMode(QAbstractItemView::SingleSelection);
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
			"Coordinates are in the source's own canvas, (0,0) = top-left.",
			this);
		hint->setWordWrap(true);
		form->addRow(QString(), hint);

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
		for (const QuickAction &a : actions_)
			list_->addItem(a.name);
		loading_ = false;

		const bool hasActions = !actions_.empty();
		set_editors_enabled(hasActions);

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

	void add_action()
	{
		QuickAction a;
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

	std::vector<QuickAction> actions_;
	bool loading_ = false;

	QListWidget *list_ = nullptr;
	QPushButton *addButton_ = nullptr;
	QPushButton *removeButton_ = nullptr;
	QLineEdit *nameEdit_ = nullptr;
	QComboBox *typeCombo_ = nullptr;
	ModifierBox *modifiers_ = nullptr;
	QStackedWidget *stack_ = nullptr;
	QLineEdit *keyEdit_ = nullptr;
	QLineEdit *textEdit_ = nullptr;
	QSpinBox *xSpin_ = nullptr;
	QSpinBox *ySpin_ = nullptr;
	QComboBox *buttonCombo_ = nullptr;
	QSpinBox *clickSpin_ = nullptr;
	QComboBox *modeCombo_ = nullptr;
};

/* ------------------------------------------------------------------ */
/* Dock                                                                */
/* ------------------------------------------------------------------ */

class HidDock : public QWidget {
public:
	explicit HidDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		build_ui();
		load_settings(actions_, initialTarget_);
		refresh_sources(initialTarget_);
		rebuild_actions();
		set_status(QStringLiteral("Ready."), false);
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

		auto *scroll = new QScrollArea(actionsGroup);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		actionsHost_ = new QWidget;
		actionsGrid_ = new QGridLayout(actionsHost_);
		actionsGrid_->setContentsMargins(0, 0, 0, 0);
		actionsGrid_->setSpacing(6);
		scroll->setWidget(actionsHost_);
		actionsLayout->addWidget(scroll, 1);

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

	void rebuild_actions()
	{
		while (QLayoutItem *item = actionsGrid_->takeAt(0)) {
			if (QWidget *widget = item->widget())
				widget->deleteLater();
			delete item;
		}

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

		for (int i = 0; i < (int)actions_.size(); i++) {
			const QuickAction &action = actions_[i];
			auto *button = new QPushButton(action.name, actionsHost_);
			button->setToolTip(action.type == ActionKey
						   ? QString("Key: %1 %2")
							     .arg(action.key, action.modifiers)
						   : QString("Click %1 at %2,%3")
							     .arg(action.button)
							     .arg(action.x)
							     .arg(action.y));
			connect(button, &QPushButton::clicked, this, [this, i] { run_action(i); });
			actionsGrid_->addWidget(button, i / 2, i % 2);
		}

		actionsGrid_->setRowStretch(((int)actions_.size() + 1) / 2, 1);
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

	void open_settings()
	{
		SettingsDialog dialog(actions_, this);
		if (dialog.exec() != QDialog::Accepted)
			return;

		actions_ = dialog.actions();
		rebuild_actions();
		if (!save_settings(actions_, selected_target()))
			set_status("Could not save settings (see OBS log).", true);
	}

	void set_status(const QString &text, bool error)
	{
		status_->setText(text);
		status_->setStyleSheet(error ? "color: #c0392b;" : QString());
	}

	QString initialTarget_;

	QComboBox *target_ = nullptr;
	QWidget *actionsHost_ = nullptr;
	QGridLayout *actionsGrid_ = nullptr;
	QPushButton *settingsButton_ = nullptr;
	QLabel *status_ = nullptr;

	std::vector<QuickAction> actions_;
};

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
