/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "properties-form.hpp"

#include "common.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace dm {

namespace {

void destroyProperties(obs_properties_t *props)
{
	obs_properties_destroy(props);
}

QColor colorFromObs(long long value)
{
	const auto v = uint32_t(value);
	return QColor(int(v & 0xff), int((v >> 8) & 0xff), int((v >> 16) & 0xff), int((v >> 24) & 0xff));
}

long long colorToObs(const QColor &color)
{
	return (long long)(uint32_t(color.red()) | (uint32_t(color.green()) << 8) | (uint32_t(color.blue()) << 16) |
			   (uint32_t(color.alpha()) << 24));
}

} // namespace

PropertiesForm::PropertiesForm(Loader loader, obs_data_t *settings, QWidget *parent)
	: QWidget(parent),
	  loader_(std::move(loader)),
	  settings_(settings),
	  properties_(nullptr, destroyProperties)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	rebuild();
}

PropertiesForm::~PropertiesForm() = default;

void PropertiesForm::scheduleRebuild()
{
	if (rebuildPending_)
		return;
	rebuildPending_ = true;
	QTimer::singleShot(0, this, [this]() {
		rebuildPending_ = false;
		rebuild();
	});
}

void PropertiesForm::rebuild()
{
	delete content_;
	content_ = new QWidget(this);
	auto *form = new QFormLayout(content_);
	form->setContentsMargins(0, 0, 0, 0);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	properties_.reset(loader_ ? loader_() : nullptr);
	if (properties_) {
		obs_properties_apply_settings(properties_.get(), settings_);
		addProperties(properties_.get(), form);
	}
	if (form->rowCount() == 0)
		form->addRow(new QLabel(T("Properties.None"), content_));

	layout()->addWidget(content_);
}

void PropertiesForm::addProperties(obs_properties_t *props, QFormLayout *layout)
{
	for (obs_property_t *property = obs_properties_first(props); property; obs_property_next(&property)) {
		if (obs_property_visible(property))
			addProperty(property, layout);
	}
}

void PropertiesForm::notifyChanged(obs_property_t *property)
{
	if (obs_property_modified(property, settings_))
		scheduleRebuild();
	emit changed();
}

void PropertiesForm::addProperty(obs_property_t *property, QFormLayout *layout)
{
	const char *name = obs_property_name(property);
	const QString label = qstr(obs_property_description(property));
	const QString tooltip = qstr(obs_property_long_description(property));
	QWidget *parent = layout->parentWidget();
	QWidget *field = nullptr;
	bool labelInField = false;

	switch (obs_property_get_type(property)) {
	case OBS_PROPERTY_BOOL: {
		auto *box = new QCheckBox(label, parent);
		box->setChecked(obs_data_get_bool(settings_, name));
		connect(box, &QCheckBox::toggled, this, [this, property, name](bool checked) {
			obs_data_set_bool(settings_, name, checked);
			notifyChanged(property);
		});
		field = box;
		labelInField = true;
		break;
	}
	case OBS_PROPERTY_INT: {
		auto *spin = new QSpinBox(parent);
		spin->setRange(obs_property_int_min(property), obs_property_int_max(property));
		spin->setSingleStep(std::max(1, obs_property_int_step(property)));
		spin->setSuffix(qstr(obs_property_int_suffix(property)));
		spin->setValue(int(obs_data_get_int(settings_, name)));
		connect(spin, &QSpinBox::valueChanged, this, [this, property, name](int value) {
			obs_data_set_int(settings_, name, value);
			notifyChanged(property);
		});
		field = spin;
		break;
	}
	case OBS_PROPERTY_FLOAT: {
		auto *spin = new QDoubleSpinBox(parent);
		spin->setRange(obs_property_float_min(property), obs_property_float_max(property));
		spin->setSingleStep(obs_property_float_step(property));
		spin->setDecimals(3);
		spin->setSuffix(qstr(obs_property_float_suffix(property)));
		spin->setValue(obs_data_get_double(settings_, name));
		connect(spin, &QDoubleSpinBox::valueChanged, this, [this, property, name](double value) {
			obs_data_set_double(settings_, name, value);
			notifyChanged(property);
		});
		field = spin;
		break;
	}
	case OBS_PROPERTY_TEXT: {
		const obs_text_type type = obs_property_text_type(property);
		if (type == OBS_TEXT_INFO) {
			auto *info = new QLabel(label, parent);
			info->setWordWrap(true);
			info->setTextFormat(Qt::RichText);
			info->setOpenExternalLinks(true);
			field = info;
			labelInField = true;
		} else if (type == OBS_TEXT_MULTILINE) {
			auto *edit = new QPlainTextEdit(qstr(obs_data_get_string(settings_, name)), parent);
			edit->setMaximumHeight(90);
			connect(edit, &QPlainTextEdit::textChanged, this, [this, property, name, edit]() {
				obs_data_set_string(settings_, name, utf8(edit->toPlainText()).c_str());
				notifyChanged(property);
			});
			field = edit;
		} else {
			auto *edit = new QLineEdit(qstr(obs_data_get_string(settings_, name)), parent);
			if (type == OBS_TEXT_PASSWORD)
				edit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
			connect(edit, &QLineEdit::textEdited, this, [this, property, name](const QString &text) {
				obs_data_set_string(settings_, name, utf8(text).c_str());
				notifyChanged(property);
			});
			field = edit;
		}
		break;
	}
	case OBS_PROPERTY_PATH: {
		auto *row = new QWidget(parent);
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 0, 0, 0);
		auto *edit = new QLineEdit(qstr(obs_data_get_string(settings_, name)), row);
		auto *browse = new QPushButton(T("Browse"), row);
		h->addWidget(edit, 1);
		h->addWidget(browse);
		connect(edit, &QLineEdit::textEdited, this, [this, property, name](const QString &text) {
			obs_data_set_string(settings_, name, utf8(text).c_str());
			notifyChanged(property);
		});
		connect(browse, &QPushButton::clicked, this, [this, property, name, edit, label]() {
			const obs_path_type type = obs_property_path_type(property);
			const QString start = edit->text().isEmpty() ? qstr(obs_property_path_default_path(property))
								     : edit->text();
			const QString filter = qstr(obs_property_path_filter(property));
			QString path;
			if (type == OBS_PATH_DIRECTORY)
				path = QFileDialog::getExistingDirectory(this, label, start);
			else if (type == OBS_PATH_FILE_SAVE)
				path = QFileDialog::getSaveFileName(this, label, start, filter);
			else
				path = QFileDialog::getOpenFileName(this, label, start, filter);
			if (path.isEmpty())
				return;
			edit->setText(path);
			obs_data_set_string(settings_, name, utf8(path).c_str());
			notifyChanged(property);
		});
		field = row;
		break;
	}
	case OBS_PROPERTY_LIST: {
		auto *combo = new QComboBox(parent);
		const obs_combo_format format = obs_property_list_format(property);
		const bool editable = obs_property_list_type(property) == OBS_COMBO_TYPE_EDITABLE;
		combo->setEditable(editable);
		int selected = -1;
		for (size_t i = 0, n = obs_property_list_item_count(property); i < n; i++) {
			QVariant value;
			bool match = false;
			switch (format) {
			case OBS_COMBO_FORMAT_INT:
				value = QVariant::fromValue(obs_property_list_item_int(property, i));
				match = obs_data_get_int(settings_, name) == obs_property_list_item_int(property, i);
				break;
			case OBS_COMBO_FORMAT_FLOAT:
				value = obs_property_list_item_float(property, i);
				match = obs_data_get_double(settings_, name) ==
					obs_property_list_item_float(property, i);
				break;
			case OBS_COMBO_FORMAT_BOOL:
				value = obs_property_list_item_bool(property, i);
				match = obs_data_get_bool(settings_, name) == obs_property_list_item_bool(property, i);
				break;
			case OBS_COMBO_FORMAT_STRING:
			default:
				value = qstr(obs_property_list_item_string(property, i));
				match = value.toString() == qstr(obs_data_get_string(settings_, name));
				break;
			}
			combo->addItem(qstr(obs_property_list_item_name(property, i)), value);
			if (obs_property_list_item_disabled(property, i)) {
				if (auto *model = qobject_cast<QStandardItemModel *>(combo->model()))
					model->item(int(i))->setEnabled(false);
			}
			if (match && selected < 0)
				selected = int(i);
		}
		if (editable && format == OBS_COMBO_FORMAT_STRING)
			combo->setEditText(qstr(obs_data_get_string(settings_, name)));
		else
			combo->setCurrentIndex(selected);

		connect(combo, &QComboBox::currentIndexChanged, this, [this, property, name, combo, format](int index) {
			if (index < 0)
				return;
			const QVariant value = combo->itemData(index);
			switch (format) {
			case OBS_COMBO_FORMAT_INT:
				obs_data_set_int(settings_, name, value.toLongLong());
				break;
			case OBS_COMBO_FORMAT_FLOAT:
				obs_data_set_double(settings_, name, value.toDouble());
				break;
			case OBS_COMBO_FORMAT_BOOL:
				obs_data_set_bool(settings_, name, value.toBool());
				break;
			case OBS_COMBO_FORMAT_STRING:
			default:
				obs_data_set_string(settings_, name, utf8(value.toString()).c_str());
				break;
			}
			notifyChanged(property);
		});
		if (editable && format == OBS_COMBO_FORMAT_STRING) {
			connect(combo, &QComboBox::editTextChanged, this, [this, property, name](const QString &text) {
				obs_data_set_string(settings_, name, utf8(text).c_str());
				notifyChanged(property);
			});
		}
		field = combo;
		break;
	}
	case OBS_PROPERTY_COLOR:
	case OBS_PROPERTY_COLOR_ALPHA: {
		const bool alpha = obs_property_get_type(property) == OBS_PROPERTY_COLOR_ALPHA;
		auto *button = new QPushButton(parent);
		auto paint = [button](const QColor &color) {
			button->setText(color.name(QColor::HexArgb));
			button->setStyleSheet(QStringLiteral("background-color: %1;").arg(color.name()));
		};
		paint(colorFromObs(obs_data_get_int(settings_, name)));
		connect(button, &QPushButton::clicked, this, [this, property, name, alpha, paint, label]() {
			QColorDialog::ColorDialogOptions options;
			if (alpha)
				options |= QColorDialog::ShowAlphaChannel;
			const QColor color = QColorDialog::getColor(colorFromObs(obs_data_get_int(settings_, name)),
								    this, label, options);
			if (!color.isValid())
				return;
			obs_data_set_int(settings_, name, colorToObs(color));
			paint(color);
			notifyChanged(property);
		});
		field = button;
		break;
	}
	case OBS_PROPERTY_GROUP: {
		auto *group = new QGroupBox(label, parent);
		auto *inner = new QFormLayout(group);
		if (obs_property_group_type(property) == OBS_GROUP_CHECKABLE) {
			group->setCheckable(true);
			group->setChecked(obs_data_get_bool(settings_, name));
			connect(group, &QGroupBox::toggled, this, [this, property, name](bool checked) {
				obs_data_set_bool(settings_, name, checked);
				notifyChanged(property);
			});
		}
		addProperties(obs_property_group_content(property), inner);
		field = group;
		labelInField = true;
		break;
	}
	default:
		/* Buttons, fonts, frame rates and editable lists are not used by encoders or services. */
		return;
	}

	field->setEnabled(obs_property_enabled(property));
	if (!tooltip.isEmpty())
		field->setToolTip(tooltip);
	if (labelInField)
		layout->addRow(field);
	else
		layout->addRow(label, field);
}

} // namespace dm
