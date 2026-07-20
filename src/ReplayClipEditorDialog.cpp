/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ReplayClipEditorDialog.hpp"

#include "PreviewPlayer.hpp"
#include "TimelineWidget.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QThreadPool>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

const char *kVideoExtensions[] = {"mp4", "mkv", "mov", "flv", "ts", "m4v", "avi"};
constexpr int kThumbWidth = 256;

QString msToTimestamp(qint64 ms, bool withMillis = false)
{
	qint64 totalSec = ms / 1000;
	qint64 h = totalSec / 3600;
	qint64 m = (totalSec % 3600) / 60;
	qint64 s = totalSec % 60;
	QString base = h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
			     : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
	if (withMillis)
		base += QStringLiteral(".%1").arg((ms % 1000) / 100);
	return base;
}

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString settingsFilePath()
{
	char *path = obs_module_config_path("settings.json");
	QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

QString humanSize(double bytes)
{
	if (bytes >= 1024.0 * 1024.0 * 1024.0)
		return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
	if (bytes >= 1024.0 * 1024.0)
		return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
	return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

QString mapObsFormatToContainer(const QString &fmt)
{
	if (fmt.contains(QStringLiteral("mkv")) || fmt.contains(QStringLiteral("matroska")))
		return QStringLiteral("mkv");
	if (fmt.contains(QStringLiteral("mov")))
		return QStringLiteral("mov");
	return QStringLiteral("mp4");
}

/* OBS advanced-output encoder ids → FFmpeg encoder names. */
QString mapAdvancedEncoderId(const QString &id)
{
	struct {
		const char *obs;
		const char *ffmpeg;
	} map[] = {
		{"obs_x264", "libx264"},
		{"jim_nvenc", "h264_nvenc"},
		{"obs_nvenc_h264", "h264_nvenc"},
		{"jim_hevc_nvenc", "hevc_nvenc"},
		{"obs_nvenc_hevc", "hevc_nvenc"},
		{"jim_av1_nvenc", "av1_nvenc"},
		{"obs_nvenc_av1", "av1_nvenc"},
		{"h264_texture_amf", "h264_amf"},
		{"h265_texture_amf", "hevc_amf"},
		{"av1_texture_amf", "av1_amf"},
		{"obs_qsv11", "h264_qsv"},
		{"obs_qsv11_v2", "h264_qsv"},
		{"obs_qsv11_hevc", "hevc_qsv"},
		{"obs_qsv11_av1", "av1_qsv"},
	};
	for (const auto &m : map) {
		if (id == QLatin1String(m.obs))
			return QString::fromUtf8(m.ffmpeg);
	}
	return QString();
}

/* OBS simple-output encoder ids → FFmpeg encoder names. */
QString mapSimpleEncoderId(const QString &id)
{
	struct {
		const char *obs;
		const char *ffmpeg;
	} map[] = {
		{"x264", "libx264"},
		{"x264_lowcpu", "libx264"},
		{"nvenc", "h264_nvenc"},
		{"nvenc_hevc", "hevc_nvenc"},
		{"nvenc_av1", "av1_nvenc"},
		{"amd", "h264_amf"},
		{"amd_hevc", "hevc_amf"},
		{"amd_av1", "av1_amf"},
		{"qsv", "h264_qsv"},
		{"qsv_av1", "av1_qsv"},
	};
	for (const auto &m : map) {
		if (id == QLatin1String(m.obs))
			return QString::fromUtf8(m.ffmpeg);
	}
	return QString();
}

/* Trim-marker icon: a bar with a triangle pointing into the kept region,
 * in the same accent color as the timeline handles. */
QIcon makeTrimIcon(bool in)
{
	QPixmap pm(24, 24);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	QColor accent(60, 160, 255);
	p.setPen(Qt::NoPen);
	p.setBrush(accent);
	if (in) {
		p.drawRect(QRectF(4, 4, 3, 16));
		QPolygonF tri;
		tri << QPointF(10, 6) << QPointF(10, 18) << QPointF(19, 12);
		p.drawPolygon(tri);
	} else {
		p.drawRect(QRectF(17, 4, 3, 16));
		QPolygonF tri;
		tri << QPointF(14, 6) << QPointF(14, 18) << QPointF(5, 12);
		p.drawPolygon(tri);
	}
	return QIcon(pm);
}

QString fallbackEncoder()
{
	const auto avail = ExportWorker::availableEncoders();
	const char *preferred[] = {"h264_nvenc", "h264_amf", "h264_qsv", "libx264", "libopenh264"};
	for (const char *p : preferred) {
		for (const auto &e : avail) {
			if (e.id == QLatin1String(p))
				return e.id;
		}
	}
	return avail.isEmpty() ? QString() : avail.first().id;
}

bool encoderAvailable(const QString &id)
{
	for (const auto &e : ExportWorker::availableEncoders()) {
		if (e.id == id)
			return true;
	}
	return false;
}

} // namespace

ReplayClipEditorDialog::ReplayClipEditorDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(text("ReplayClipEditor.Title"));
	setWindowFlags(windowFlags() | Qt::WindowMaximizeButtonHint | Qt::WindowMinimizeButtonHint);
	resize(1060, 760);

	stack = new QStackedWidget(this);
	stack->addWidget(buildBrowserPage());
	stack->addWidget(buildEditorPage());

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(stack);

	/* Keep keyboard focus on the dialog itself so Space and the other
	 * shortcuts always control playback; text fields keep focus so
	 * typing still works. */
	for (QWidget *w : stack->findChildren<QWidget *>()) {
		if (qobject_cast<QPushButton *>(w) || qobject_cast<QToolButton *>(w) ||
		    qobject_cast<QCheckBox *>(w) || qobject_cast<QComboBox *>(w))
			w->setFocusPolicy(Qt::NoFocus);
	}

	loadPersistedSettings();
	updateSettingsModeUi();
}

ReplayClipEditorDialog::~ReplayClipEditorDialog()
{
	shutdown();
}

void ReplayClipEditorDialog::shutdown()
{
	filmstripGeneration++;
	if (exportWorker) {
		exportWorker->cancel();
		exportWorker->wait(10000);
	}
	if (player) {
		cleanupTemporaryReplay(); /* closes the file via the player */
		player->shutdown();
	}
}

/* ---------------------------------------------------------------- browser */

QWidget *ReplayClipEditorDialog::buildBrowserPage()
{
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(16, 12, 16, 12);

	auto *header = new QHBoxLayout;
	auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(text("ReplayClipEditor.Browser.Heading")));
	title->setStyleSheet(QStringLiteral("font-size: 15px;"));
	folderLabel = new QLabel;
	folderLabel->setStyleSheet(QStringLiteral("color: gray;"));

	auto *changeFolderBtn = new QPushButton(text("ReplayClipEditor.Browser.ChangeFolder"));
	changeFolderBtn->setToolTip(text("ReplayClipEditor.Browser.ChangeFolderTip"));
	connect(changeFolderBtn, &QPushButton::clicked, this, [this]() {
		QString dir = QFileDialog::getExistingDirectory(this, text("ReplayClipEditor.Browser.ChangeFolder"),
								effectiveBrowseFolder());
		if (dir.isEmpty())
			return;
		/* Picking the OBS folder again means "back to default". */
		customBrowseFolder = QDir(dir) == QDir(replayFolder()) ? QString() : dir;
		savePersistedSettings();
		refreshClipList();
	});

	resetFolderBtn = new QPushButton(text("ReplayClipEditor.Browser.ResetFolder"));
	resetFolderBtn->setToolTip(text("ReplayClipEditor.Browser.ResetFolderTip"));
	resetFolderBtn->setVisible(false);
	connect(resetFolderBtn, &QPushButton::clicked, this, [this]() {
		customBrowseFolder.clear();
		savePersistedSettings();
		refreshClipList();
	});

	auto *refreshBtn = new QPushButton(text("ReplayClipEditor.Browser.Refresh"));
	refreshBtn->setToolTip(text("ReplayClipEditor.Browser.RefreshTip"));
	connect(refreshBtn, &QPushButton::clicked, this, &ReplayClipEditorDialog::refreshClipList);
	header->addWidget(title);
	header->addSpacing(12);
	header->addWidget(folderLabel, 1);
	header->addWidget(resetFolderBtn);
	header->addWidget(changeFolderBtn);
	header->addWidget(refreshBtn);
	layout->addLayout(header);

	clipList = new QListWidget;
	clipList->setViewMode(QListWidget::IconMode);
	clipList->setResizeMode(QListWidget::Adjust);
	clipList->setMovement(QListWidget::Static);
	clipList->setIconSize(QSize(224, 126));
	clipList->setGridSize(QSize(248, 178));
	clipList->setWordWrap(true);
	clipList->setSpacing(8);
	clipList->setUniformItemSizes(true);
	connect(clipList, &QListWidget::itemActivated, this,
		[this](QListWidgetItem *item) { openClip(item->data(Qt::UserRole).toString()); });
	layout->addWidget(clipList, 1);

	auto *bottom = new QHBoxLayout;
	autoStartCheck = new QCheckBox(text("ReplayClipEditor.Browser.AutoStart"));
	autoStartCheck->setToolTip(text("ReplayClipEditor.Browser.AutoStartTip"));
	connect(autoStartCheck, &QCheckBox::toggled, this, [this](bool on) {
		savePersistedSettings();
		if (on && !obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_start();
	});
	auto *hint = new QLabel(text("ReplayClipEditor.Browser.Hint"));
	hint->setStyleSheet(QStringLiteral("color: gray;"));
	bottom->addWidget(autoStartCheck);
	bottom->addStretch(1);
	bottom->addWidget(hint);
	bottom->addStretch(1);
	layout->addLayout(bottom);

	return page;
}

QString ReplayClipEditorDialog::replayFolder() const
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return QString();

	const char *mode = config_get_string(cfg, "Output", "Mode");
	const char *path = nullptr;
	if (mode && strcmp(mode, "Advanced") == 0)
		path = config_get_string(cfg, "AdvOut", "RecFilePath");
	else
		path = config_get_string(cfg, "SimpleOutput", "FilePath");

	return path ? QString::fromUtf8(path) : QString();
}

QString ReplayClipEditorDialog::effectiveBrowseFolder() const
{
	if (!customBrowseFolder.isEmpty() && QDir(customBrowseFolder).exists())
		return customBrowseFolder;
	return replayFolder();
}

void ReplayClipEditorDialog::refreshClipList()
{
	clipList->clear();

	QString folder = effectiveBrowseFolder();
	folderLabel->setText(folder.isEmpty() ? text("ReplayClipEditor.Browser.NoFolder")
					      : QDir::toNativeSeparators(folder));
	resetFolderBtn->setVisible(!customBrowseFolder.isEmpty());
	if (folder.isEmpty())
		return;

	QDir dir(folder);
	QStringList filters;
	for (const char *ext : kVideoExtensions)
		filters << QStringLiteral("*.") + QString::fromUtf8(ext);

	QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

	QPixmap placeholder(224, 126);
	placeholder.fill(QColor(40, 40, 46));

	int generation = ++filmstripGeneration; /* also invalidates stale thumb jobs */
	for (const QFileInfo &fi : files) {
		auto *item = new QListWidgetItem(QIcon(placeholder), QString());
		double sizeMb = fi.size() / (1024.0 * 1024.0);
		item->setText(QStringLiteral("%1\n%2  -  %3 MB")
				      .arg(fi.fileName(), fi.lastModified().toString(QStringLiteral("MMM d, h:mm AP")))
				      .arg(sizeMb, 0, 'f', 1));
		item->setData(Qt::UserRole, fi.absoluteFilePath());
		item->setToolTip(fi.absoluteFilePath());
		item->setSizeHint(QSize(240, 170));
		clipList->addItem(item);

		/* Thumbnail in the background */
		QString path = fi.absoluteFilePath();
		QPointer<ReplayClipEditorDialog> self(this);
		int row = clipList->count() - 1;
		QThreadPool::globalInstance()->start([self, path, row, generation]() {
			QImage img = VideoProbe::extractFrame(path, 500, kThumbWidth);
			if (img.isNull() || !self)
				return;
			QMetaObject::invokeMethod(
				self,
				[self, img, row, generation]() {
					if (!self || self->filmstripGeneration != generation)
						return;
					if (row < self->clipList->count()) {
						QListWidgetItem *it = self->clipList->item(row);
						it->setIcon(QIcon(QPixmap::fromImage(img)));
					}
				},
				Qt::QueuedConnection);
		});
	}
}

void ReplayClipEditorDialog::showBrowser()
{
	stack->setCurrentIndex(0);
	refreshClipList();
	showNormal();
	raise();
	activateWindow();
}

void ReplayClipEditorDialog::openClipExternal(const QString &path)
{
	showNormal();
	raise();
	activateWindow();
	openClip(path);
	if (currentClip.valid && currentClip.path == path) {
		/* Clip That replays are working copies: once a trim is
		 * exported, the full-length replay is deleted on leaving. */
		currentClipIsTemporaryReplay = true;
		currentClipWasExported = false;
	}
}

void ReplayClipEditorDialog::cleanupTemporaryReplay()
{
	if (currentClipIsTemporaryReplay && currentClipWasExported && !currentClip.path.isEmpty()) {
		player->closeFile(); /* release file handles first */
		if (QFile::remove(currentClip.path))
			blog(LOG_INFO, "[replay-clip-editor] removed temporary replay '%s'",
			     currentClip.path.toUtf8().constData());
		else
			blog(LOG_WARNING, "[replay-clip-editor] could not remove temporary replay '%s'",
			     currentClip.path.toUtf8().constData());
	}
	currentClipIsTemporaryReplay = false;
	currentClipWasExported = false;
}

/* ----------------------------------------------------------------- editor */

QWidget *ReplayClipEditorDialog::buildEditorPage()
{
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(16, 12, 16, 12);
	layout->setSpacing(8);

	/* header */
	auto *header = new QHBoxLayout;
	auto *backBtn = new QPushButton(text("ReplayClipEditor.Editor.Back"));
	connect(backBtn, &QPushButton::clicked, this, &ReplayClipEditorDialog::backToBrowser);
	clipNameLabel = new QLabel;
	clipNameLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
	clipNameLabel->setAlignment(Qt::AlignCenter);
	/* phantom spacer mirrors the back button so the title centers on the
	 * window, not on the leftover space */
	auto *headerSpacer = new QWidget;
	headerSpacer->setFixedWidth(backBtn->sizeHint().width());
	header->addWidget(backBtn);
	header->addWidget(clipNameLabel, 1);
	header->addWidget(headerSpacer);
	layout->addLayout(header);

	/* preview */
	player = new PreviewPlayer;
	player->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	layout->addWidget(player, 1);
	connect(player, &PreviewPlayer::positionChanged, this, [this](qint64 ms) {
		timeline->setPlayhead(ms);
		updateTimeLabels();
	});
	connect(player, &PreviewPlayer::playStateChanged, this, [this](bool playing) {
		playButton->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
	});

	/* transport row */
	auto *transport = new QHBoxLayout;
	playButton = new QToolButton;
	playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
	playButton->setToolTip(text("ReplayClipEditor.Editor.PlayPause"));
	playButton->setFixedSize(40, 32);
	connect(playButton, &QToolButton::clicked, this, [this]() { player->togglePlayPause(); });

	auto *stepBack = new QToolButton;
	stepBack->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
	stepBack->setToolTip(text("ReplayClipEditor.Editor.FrameBack"));
	stepBack->setFixedSize(32, 32);
	connect(stepBack, &QToolButton::clicked, this, [this]() { player->stepFrames(-1); });
	auto *stepFwd = new QToolButton;
	stepFwd->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
	stepFwd->setToolTip(text("ReplayClipEditor.Editor.FrameForward"));
	stepFwd->setFixedSize(32, 32);
	connect(stepFwd, &QToolButton::clicked, this, [this]() { player->stepFrames(1); });

	auto *jumpInBtn = new QToolButton;
	jumpInBtn->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
	jumpInBtn->setToolTip(text("ReplayClipEditor.Editor.JumpToIn"));
	jumpInBtn->setFixedSize(32, 32);
	connect(jumpInBtn, &QToolButton::clicked, this, [this]() { seekKeepingState(timeline->inPointMs()); });
	auto *jumpOutBtn = new QToolButton;
	jumpOutBtn->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
	jumpOutBtn->setToolTip(text("ReplayClipEditor.Editor.JumpToOut"));
	jumpOutBtn->setFixedSize(32, 32);
	connect(jumpOutBtn, &QToolButton::clicked, this, [this]() { seekKeepingState(timeline->outPointMs()); });

	timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"));
	rangeLabel = new QLabel;
	rangeLabel->setStyleSheet(QStringLiteral("color: #3ca0ff; font-weight: bold;"));

	auto *setInBtn = new QToolButton;
	setInBtn->setIcon(makeTrimIcon(true));
	setInBtn->setToolTip(text("ReplayClipEditor.Editor.SetInTip"));
	setInBtn->setFixedSize(32, 32);
	connect(setInBtn, &QToolButton::clicked, this, [this]() { timeline->setInPoint(player->currentMs()); });
	auto *setOutBtn = new QToolButton;
	setOutBtn->setIcon(makeTrimIcon(false));
	setOutBtn->setToolTip(text("ReplayClipEditor.Editor.SetOutTip"));
	setOutBtn->setFixedSize(32, 32);
	connect(setOutBtn, &QToolButton::clicked, this, [this]() { timeline->setOutPoint(player->currentMs()); });

	auto *qualityLabel = new QLabel(text("ReplayClipEditor.Editor.PreviewQuality") + QStringLiteral(":"));
	qualityLabel->setStyleSheet(QStringLiteral("color: gray;"));
	previewQualityCombo = new QComboBox;
	previewQualityCombo->addItem(text("ReplayClipEditor.Editor.PreviewFull"), 0);
	previewQualityCombo->addItem(text("ReplayClipEditor.Editor.PreviewFast"), 1);
	previewQualityCombo->addItem(text("ReplayClipEditor.Editor.PreviewFastest"), 2);
	previewQualityCombo->setToolTip(text("ReplayClipEditor.Editor.PreviewQualityTip"));
	connect(previewQualityCombo, &QComboBox::currentIndexChanged, this, [this](int) {
		player->setPreviewQuality(previewQualityCombo->currentData().toInt());
		savePersistedSettings();
	});

	transport->addWidget(jumpInBtn);
	transport->addWidget(stepBack);
	transport->addWidget(playButton);
	transport->addWidget(stepFwd);
	transport->addWidget(jumpOutBtn);
	transport->addSpacing(10);
	transport->addWidget(timeLabel);
	transport->addStretch(1);
	transport->addWidget(rangeLabel);
	transport->addStretch(1);
	transport->addWidget(qualityLabel);
	transport->addWidget(previewQualityCombo);
	transport->addSpacing(8);
	transport->addWidget(setInBtn);
	transport->addWidget(setOutBtn);
	layout->addLayout(transport);

	/* timeline */
	timeline = new TimelineWidget;
	/* Moving the playhead during playback keeps playing from the new
	 * spot; while paused it shows the exact frame. */
	connect(timeline, &TimelineWidget::seekRequested, this, [this](qint64 ms) {
		if (player->isPlaying())
			player->seekTo(ms);
		else
			player->scrubTo(ms);
	});
	connect(timeline, &TimelineWidget::handleScrubbed, this, [this](qint64 ms) { player->scrubTo(ms); });
	/* Regenerate the filmstrip for the visible window shortly after the
	 * user stops zooming/panning — zoomed views get sharper previews. */
	viewDebounce = new QTimer(this);
	viewDebounce->setSingleShot(true);
	viewDebounce->setInterval(350);
	connect(viewDebounce, &QTimer::timeout, this, [this]() {
		if (currentClip.valid && stack->currentIndex() == 1)
			loadFilmstrip(currentClip.path, timeline->viewStartMs(), timeline->viewEndMs());
	});
	connect(timeline, &TimelineWidget::viewChanged, this, [this](qint64, qint64) { viewDebounce->start(); });

	connect(timeline, &TimelineWidget::inPointChanged, this, [this](qint64) {
		updateTimeLabels();
		updateSizeEstimate();
	});
	connect(timeline, &TimelineWidget::outPointChanged, this, [this](qint64) {
		updateTimeLabels();
		updateSizeEstimate();
	});
	layout->addWidget(timeline);

	/* audio tracks */
	audioTracksRow = new QWidget;
	auto *tracksLayout = new QHBoxLayout(audioTracksRow);
	tracksLayout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(audioTracksRow);

	/* export settings row */
	auto *settingsRow = new QHBoxLayout;
	settingsRow->addWidget(new QLabel(text("ReplayClipEditor.Export.Settings") + QStringLiteral(":")));

	settingsModeCombo = new QComboBox;
	settingsModeCombo->addItem(text("ReplayClipEditor.Export.UseObsSettings"), QStringLiteral("obs"));
	settingsModeCombo->addItem(text("ReplayClipEditor.Export.CustomSettings"), QStringLiteral("custom"));
	settingsModeCombo->setToolTip(text("ReplayClipEditor.Export.SettingsTip"));
	connect(settingsModeCombo, &QComboBox::currentIndexChanged, this, [this](int) { updateSettingsModeUi(); });
	settingsRow->addWidget(settingsModeCombo);

	customSettingsRow = new QWidget;
	auto *customLayout = new QHBoxLayout(customSettingsRow);
	customLayout->setContentsMargins(0, 0, 0, 0);

	encoderCombo = new QComboBox;
	encoderCombo->setToolTip(text("ReplayClipEditor.Export.EncoderTip"));
	connect(encoderCombo, &QComboBox::currentIndexChanged, this, [this](int) { updateSizeEstimate(); });

	rateControlCombo = new QComboBox;
	rateControlCombo->addItems({QStringLiteral("CQP"), QStringLiteral("CBR"), QStringLiteral("VBR")});
	rateControlCombo->setToolTip(text("ReplayClipEditor.Export.RateControlTip"));
	connect(rateControlCombo, &QComboBox::currentTextChanged, this,
		[this](const QString &) { updateSettingsModeUi(); });

	cqSpin = new QSpinBox;
	cqSpin->setRange(0, 51);
	cqSpin->setValue(23);
	cqSpin->setPrefix(text("ReplayClipEditor.Export.CqPrefix"));
	cqSpin->setToolTip(text("ReplayClipEditor.Export.CqTip"));
	connect(cqSpin, &QSpinBox::valueChanged, this, [this](int) { updateSizeEstimate(); });

	bitrateSpin = new QSpinBox;
	bitrateSpin->setRange(500, 300000);
	bitrateSpin->setValue(16000);
	bitrateSpin->setSingleStep(500);
	bitrateSpin->setSuffix(QStringLiteral(" kbps"));
	connect(bitrateSpin, &QSpinBox::valueChanged, this, [this](int) { updateSizeEstimate(); });

	containerCombo = new QComboBox;
	containerCombo->addItems({QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov")});
	connect(containerCombo, &QComboBox::currentTextChanged, this,
		[this](const QString &) { updateExtensionLabel(); });

	customLayout->addWidget(encoderCombo);
	customLayout->addWidget(rateControlCombo);
	customLayout->addWidget(cqSpin);
	customLayout->addWidget(bitrateSpin);
	customLayout->addWidget(containerCombo);
	settingsRow->addWidget(customSettingsRow);
	settingsRow->addStretch(1);
	layout->addLayout(settingsRow);

	/* destination + export row */
	auto *destRow = new QHBoxLayout;
	destRow->addWidget(new QLabel(text("ReplayClipEditor.Export.SaveTo") + QStringLiteral(":")));
	folderEdit = new QLineEdit;
	folderEdit->setToolTip(text("ReplayClipEditor.Export.SaveToTip"));
	destRow->addWidget(folderEdit, 1);
	auto *browseBtn = new QPushButton(text("ReplayClipEditor.Export.Browse"));
	browseBtn->setToolTip(text("ReplayClipEditor.Export.BrowseTip"));
	connect(browseBtn, &QPushButton::clicked, this, [this]() {
		QString dir = QFileDialog::getExistingDirectory(this, text("ReplayClipEditor.Export.SaveTo"),
								folderEdit->text());
		if (!dir.isEmpty())
			folderEdit->setText(QDir::toNativeSeparators(dir));
	});
	destRow->addWidget(browseBtn);
	destRow->addSpacing(8);
	destRow->addWidget(new QLabel(text("ReplayClipEditor.Export.FileName") + QStringLiteral(":")));
	nameEdit = new QLineEdit;
	nameEdit->setToolTip(text("ReplayClipEditor.Export.FileNameTip"));
	destRow->addWidget(nameEdit, 2);
	extLabel = new QLabel(QStringLiteral(".mp4"));
	destRow->addWidget(extLabel);
	destRow->addSpacing(8);

	estSizeLabel = new QLabel;
	estSizeLabel->setStyleSheet(QStringLiteral("color: gray;"));
	estSizeLabel->setToolTip(text("ReplayClipEditor.Export.EstSizeTip"));
	destRow->addWidget(estSizeLabel);

	cancelButton = new QPushButton(text("ReplayClipEditor.Export.Cancel"));
	cancelButton->setVisible(false);
	connect(cancelButton, &QPushButton::clicked, this, &ReplayClipEditorDialog::cancelExport);
	destRow->addWidget(cancelButton);

	exportButton = new QPushButton(text("ReplayClipEditor.Export.Button"));
	exportButton->setStyleSheet(QStringLiteral("font-weight: bold; padding: 4px 18px;"));
	connect(exportButton, &QPushButton::clicked, this, &ReplayClipEditorDialog::startExport);
	destRow->addWidget(exportButton);
	layout->addLayout(destRow);

	/* progress / status */
	progressBar = new QProgressBar;
	progressBar->setRange(0, 100);
	progressBar->setVisible(false);
	layout->addWidget(progressBar); /* full row width */

	auto *statusRow = new QHBoxLayout;
	exportStatusLabel = new QLabel;
	openFolderButton = new QPushButton(text("ReplayClipEditor.Export.OpenFolder"));
	openFolderButton->setVisible(false);
	connect(openFolderButton, &QPushButton::clicked, this, [this]() {
		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(lastExportedFile).absolutePath()));
	});
	statusRow->addWidget(exportStatusLabel, 1);
	statusRow->addWidget(openFolderButton);
	layout->addLayout(statusRow);

	return page;
}

void ReplayClipEditorDialog::openClip(const QString &path)
{
	cleanupTemporaryReplay();
	currentClip = VideoProbe::probe(path);
	if (!currentClip.valid) {
		exportStatusLabel->setText(text("ReplayClipEditor.Error.OpenFailed"));
		return;
	}

	if (!encodersLoaded) {
		encodersLoaded = true;
		const auto encoders = ExportWorker::availableEncoders();
		QString preferred = encoderCombo->property("preferredEncoder").toString();
		for (const auto &e : encoders)
			encoderCombo->addItem(e.name, e.id);
		int idx = encoderCombo->findData(preferred);
		if (idx >= 0)
			encoderCombo->setCurrentIndex(idx);
	}

	QFileInfo fi(path);
	clipNameLabel->setText(fi.fileName());
	exportStatusLabel->clear();
	openFolderButton->setVisible(false);
	progressBar->setVisible(false);

	/* default destination: clip's folder (or the user's saved choice) */
	QString defaultFolder = persistedOutputFolder.isEmpty() ? fi.absolutePath() : persistedOutputFolder;
	folderEdit->setText(QDir::toNativeSeparators(defaultFolder));
	nameEdit->setText(fi.completeBaseName() + QStringLiteral("_clip"));
	updateExtensionLabel();

	timeline->setDuration(currentClip.durationMs);
	rebuildAudioTrackToggles();
	updateTimeLabels();
	updateSizeEstimate();

	player->openFile(path, currentClip.fps, currentClip.durationMs, enabledAudioStreams());
	loadFilmstrip(path, 0, currentClip.durationMs);

	stack->setCurrentIndex(1);
	setFocus();
}

void ReplayClipEditorDialog::backToBrowser()
{
	if (exportWorker && exportWorker->isRunning())
		return; /* don't abandon a running export */
	filmstripGeneration++;
	player->closeFile();
	cleanupTemporaryReplay();
	timeline->clear();
	stack->setCurrentIndex(0);
	refreshClipList();
}

void ReplayClipEditorDialog::loadFilmstrip(const QString &path, qint64 startMs, qint64 endMs)
{
	int generation = ++filmstripGeneration;
	qint64 span = std::max<qint64>(endMs - startMs, 1);
	/* Only as many tiles as fit at the clip's aspect ratio — no cropping,
	 * and fewer frames to decode. */
	double aspect = currentClip.height > 0 ? (double)currentClip.width / currentClip.height : 16.0 / 9.0;
	int tileCount = timeline->desiredTileCount(aspect);
	timeline->setFilmstripLoading(true);
	QPointer<ReplayClipEditorDialog> self(this);

	QThreadPool::globalInstance()->start([self, path, startMs, span, tileCount, generation]() {
		/* Stale jobs (superseded by a newer zoom/pan) abort quickly so
		 * they don't queue up in front of the current one. */
		auto cancelled = [self, generation]() {
			return !self || self->filmstripGeneration.load() != generation;
		};
		QVector<QImage> frames = VideoProbe::extractStrip(path, startMs, span, tileCount, 160, cancelled);
		if (!self || frames.isEmpty())
			return;
		QMetaObject::invokeMethod(
			self,
			[self, frames, generation, startMs, span]() {
				if (!self || self->filmstripGeneration.load() != generation)
					return;
				self->timeline->setFilmstrip(frames, startMs, startMs + span);
				self->timeline->setFilmstripLoading(false);
			},
			Qt::QueuedConnection);
	});
}

void ReplayClipEditorDialog::rebuildAudioTrackToggles()
{
	trackChecks.clear();

	auto *tracksLayout = static_cast<QHBoxLayout *>(audioTracksRow->layout());
	while (QLayoutItem *item = tracksLayout->takeAt(0)) {
		if (QWidget *w = item->widget())
			w->deleteLater();
		delete item;
	}

	tracksLayout->addWidget(new QLabel(text("ReplayClipEditor.Editor.AudioTracks") + QStringLiteral(":")));
	if (currentClip.audioTracks.isEmpty()) {
		tracksLayout->addWidget(new QLabel(text("ReplayClipEditor.Editor.NoAudio")));
	} else {
		for (int i = 0; i < currentClip.audioTracks.size(); i++) {
			const AudioTrackInfo &track = currentClip.audioTracks[i];
			auto *check = new QCheckBox(track.title);
			check->setFocusPolicy(Qt::NoFocus);
			check->setChecked(enabledTracksMask & (1 << i));
			check->setToolTip(QStringLiteral("%1 ch, %2").arg(track.channels).arg(track.codecName));
			connect(check, &QCheckBox::toggled, this, [this](bool) {
				/* Remember the selection, preserving bits of tracks
				 * this clip doesn't have. */
				int low = 0;
				for (int j = 0; j < trackChecks.size(); j++) {
					if (trackChecks[j]->isChecked())
						low |= 1 << j;
				}
				enabledTracksMask = (enabledTracksMask & ~((1 << trackChecks.size()) - 1)) | low;
				savePersistedSettings();
				updateSizeEstimate();
				/* The preview mutes/unmutes live. */
				player->setEnabledTracks(enabledAudioStreams());
			});
			trackChecks.push_back(check);
			tracksLayout->addWidget(check);
		}
	}
	tracksLayout->addStretch(1);
}

void ReplayClipEditorDialog::updateTimeLabels()
{
	timeLabel->setText(QStringLiteral("%1 / %2").arg(msToTimestamp(player->currentMs()),
							 msToTimestamp(currentClip.durationMs)));
	qint64 len = timeline->outPointMs() - timeline->inPointMs();
	rangeLabel->setText(QStringLiteral("%1 - %2   (%3)")
				    .arg(msToTimestamp(timeline->inPointMs(), true),
					 msToTimestamp(timeline->outPointMs(), true), msToTimestamp(len, true)));
}

void ReplayClipEditorDialog::updateSettingsModeUi()
{
	bool custom = settingsModeCombo->currentData().toString() == QStringLiteral("custom");
	customSettingsRow->setVisible(custom);
	if (custom) {
		bool cqp = rateControlCombo->currentText() == QStringLiteral("CQP");
		cqSpin->setVisible(cqp);
		bitrateSpin->setVisible(!cqp);
	}
	updateExtensionLabel();
	updateSizeEstimate();
}

QString ReplayClipEditorDialog::effectiveContainer() const
{
	if (settingsModeCombo->currentData().toString() == QStringLiteral("custom"))
		return containerCombo->currentText();
	return deriveObsSettings().container;
}

void ReplayClipEditorDialog::updateExtensionLabel()
{
	extLabel->setText(QStringLiteral(".") + effectiveContainer());
}

void ReplayClipEditorDialog::updateSizeEstimate()
{
	if (!estSizeLabel)
		return;
	if (!currentClip.valid || currentClip.durationMs <= 0) {
		estSizeLabel->clear();
		return;
	}

	double durSec = (timeline->outPointMs() - timeline->inPointMs()) / 1000.0;
	if (durSec <= 0) {
		estSizeLabel->clear();
		return;
	}

	bool useObs = settingsModeCombo->currentData().toString() != QStringLiteral("custom");
	ExportSettings s = useObs ? deriveObsSettings() : customSettings();

	double videoBps;
	if (s.rateControl == QStringLiteral("CBR") || s.rateControl == QStringLiteral("VBR")) {
		videoBps = s.bitrateKbps * 1000.0;
	} else {
		/* Constant quality: size depends on content, so anchor the guess
		 * to the source file's own bitrate, scaled by the quality offset
		 * (bitrate roughly halves per +6 CQ from a CQ 23 baseline). */
		double srcBps = QFileInfo(currentClip.path).size() * 8000.0 / (double)currentClip.durationMs;
		double factor = std::clamp(std::pow(2.0, (23.0 - s.cq) / 6.0), 0.25, 4.0);
		if (s.encoderName.contains(QStringLiteral("hevc")) || s.encoderName.contains(QStringLiteral("av1")))
			factor *= 0.65;
		videoBps = srcBps * factor;
	}

	double audioBps = enabledAudioStreams().isEmpty() ? 0.0 : 192000.0;
	double bytes = (videoBps + audioBps) / 8.0 * durSec;
	estSizeLabel->setText(QStringLiteral("~%1").arg(humanSize(bytes)));
}

/* Derive export settings from the user's OBS recording configuration. */
ExportSettings ReplayClipEditorDialog::deriveObsSettings() const
{
	ExportSettings s;
	s.encoderName = fallbackEncoder();
	s.rateControl = QStringLiteral("CQP");
	s.cq = 20;

	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return s;

	const char *modeStr = config_get_string(cfg, "Output", "Mode");
	bool advanced = modeStr && strcmp(modeStr, "Advanced") == 0;

	if (advanced) {
		const char *fmt = config_get_string(cfg, "AdvOut", "RecFormat2");
		if (fmt)
			s.container = mapObsFormatToContainer(QString::fromUtf8(fmt));

		const char *enc = config_get_string(cfg, "AdvOut", "RecEncoder");
		QString mapped = enc ? mapAdvancedEncoderId(QString::fromUtf8(enc)) : QString();
		if (!mapped.isEmpty() && encoderAvailable(mapped))
			s.encoderName = mapped;

		/* The recording encoder's detailed settings live in
		 * recordEncoder.json inside the profile folder. */
		config_t *user = obs_frontend_get_user_config();
		const char *profileDir = user ? config_get_string(user, "Basic", "ProfileDir") : nullptr;
		if (profileDir) {
			QString rel = QStringLiteral("obs-studio/basic/profiles/%1/recordEncoder.json")
					      .arg(QString::fromUtf8(profileDir));
			char *full = os_get_config_path_ptr(rel.toUtf8().constData());
			obs_data_t *encData = full ? obs_data_create_from_json_file(full) : nullptr;
			bfree(full);
			if (encData) {
				const char *rc = obs_data_get_string(encData, "rate_control");
				QString rcs = QString::fromUtf8(rc ? rc : "");
				if (rcs == QStringLiteral("CBR") || rcs == QStringLiteral("VBR"))
					s.rateControl = rcs;
				else
					s.rateControl = QStringLiteral("CQP");

				long long bitrate = obs_data_get_int(encData, "bitrate");
				if (bitrate > 0)
					s.bitrateKbps = (int)bitrate;

				for (const char *key : {"cqp", "crf", "cq", "cq_value"}) {
					long long v = obs_data_get_int(encData, key);
					if (v > 0) {
						s.cq = (int)v;
						break;
					}
				}
				obs_data_release(encData);
			}
		}
	} else {
		const char *fmt = config_get_string(cfg, "SimpleOutput", "RecFormat2");
		if (fmt)
			s.container = mapObsFormatToContainer(QString::fromUtf8(fmt));

		const char *enc = config_get_string(cfg, "SimpleOutput", "RecEncoder");
		QString mapped = enc ? mapSimpleEncoderId(QString::fromUtf8(enc)) : QString();
		if (!mapped.isEmpty() && encoderAvailable(mapped))
			s.encoderName = mapped;

		const char *quality = config_get_string(cfg, "SimpleOutput", "RecQuality");
		QString q = QString::fromUtf8(quality ? quality : "");
		if (q == QStringLiteral("Stream")) {
			s.rateControl = QStringLiteral("CBR");
			long long vbitrate = config_get_int(cfg, "SimpleOutput", "VBitrate");
			s.bitrateKbps = vbitrate > 0 ? (int)vbitrate : 6000;
		} else if (q == QStringLiteral("Small")) {
			s.cq = 23;
		} else if (q == QStringLiteral("HQ")) {
			s.cq = 16;
		} else if (q == QStringLiteral("Lossless")) {
			s.cq = 10;
		}
	}

	return s;
}

ExportSettings ReplayClipEditorDialog::customSettings() const
{
	ExportSettings s;
	s.encoderName = encoderCombo->currentData().toString();
	s.rateControl = rateControlCombo->currentText();
	s.bitrateKbps = bitrateSpin->value();
	s.cq = cqSpin->value();
	s.container = containerCombo->currentText();
	return s;
}

/* ----------------------------------------------------------------- export */

QVector<int> ReplayClipEditorDialog::enabledAudioStreams() const
{
	QVector<int> result;
	for (int i = 0; i < trackChecks.size() && i < currentClip.audioTracks.size(); i++) {
		if (trackChecks[i]->isChecked())
			result.push_back(currentClip.audioTracks[i].streamIndex);
	}
	return result;
}

QString ReplayClipEditorDialog::buildOutputPath() const
{
	QString folder = QDir::fromNativeSeparators(folderEdit->text().trimmed());
	if (folder.isEmpty())
		folder = QFileInfo(currentClip.path).absolutePath();
	QString name = nameEdit->text().trimmed();
	if (name.isEmpty())
		name = QFileInfo(currentClip.path).completeBaseName() + QStringLiteral("_clip");
	QString ext = effectiveContainer();

	QString base = folder + QStringLiteral("/") + name;
	QString candidate = base + QStringLiteral(".") + ext;
	int n = 2;
	while (QFileInfo::exists(candidate))
		candidate = base + QStringLiteral(" (%1).").arg(n++) + ext;
	return candidate;
}

void ReplayClipEditorDialog::startExport()
{
	if (!currentClip.valid || (exportWorker && exportWorker->isRunning()))
		return;

	player->pause();

	bool useObs = settingsModeCombo->currentData().toString() != QStringLiteral("custom");
	ExportSettings s = useObs ? deriveObsSettings() : customSettings();
	if (s.encoderName.isEmpty()) {
		exportStatusLabel->setText(text("ReplayClipEditor.Error.NoEncoder"));
		return;
	}

	QString folder = QDir::fromNativeSeparators(folderEdit->text().trimmed());
	QDir dir(folder);
	if (folder.isEmpty() || !dir.exists()) {
		exportStatusLabel->setText(text("ReplayClipEditor.Error.BadFolder"));
		return;
	}
	savePersistedSettings();

	QString outPath = buildOutputPath();

	exportWorker = new ExportWorker(currentClip.path, outPath, timeline->inPointMs(), timeline->outPointMs(),
					enabledAudioStreams(), s, this);
	/* Only safe to delete once run() has fully returned. */
	connect(exportWorker, &QThread::finished, exportWorker, &QObject::deleteLater);
	connect(exportWorker, &ExportWorker::progressChanged, this, [this](int pct) { progressBar->setValue(pct); },
		Qt::QueuedConnection);
	connect(exportWorker, &ExportWorker::exportFinished, this,
		[this](bool ok, const QString &msg) {
			setExportUiBusy(false);
			if (ok) {
				lastExportedFile = msg;
				currentClipWasExported = true;
				exportStatusLabel->setText(text("ReplayClipEditor.Export.Done") + QStringLiteral(" ") +
							   QFileInfo(msg).fileName());
				openFolderButton->setVisible(true);
			} else if (msg == QStringLiteral("cancelled")) {
				exportStatusLabel->setText(text("ReplayClipEditor.Export.Cancelled"));
			} else {
				exportStatusLabel->setText(text("ReplayClipEditor.Error.ExportFailed") +
							   QStringLiteral(" ") + msg);
			}
		},
		Qt::QueuedConnection);

	setExportUiBusy(true);
	exportStatusLabel->clear();
	openFolderButton->setVisible(false);
	progressBar->setValue(0);
	exportWorker->start();
}

void ReplayClipEditorDialog::cancelExport()
{
	if (exportWorker)
		exportWorker->cancel();
}

void ReplayClipEditorDialog::setExportUiBusy(bool busy)
{
	exportButton->setEnabled(!busy);
	cancelButton->setVisible(busy);
	progressBar->setVisible(busy);
	settingsModeCombo->setEnabled(!busy);
	customSettingsRow->setEnabled(!busy);
	folderEdit->setEnabled(!busy);
	nameEdit->setEnabled(!busy);
}

/* ------------------------------------------------------------- persistence */

void ReplayClipEditorDialog::loadPersistedSettings()
{
	obs_data_t *data = obs_data_create_from_json_file(settingsFilePath().toUtf8().constData());
	if (!data)
		return;
	/* Encoder list isn't populated yet; remember the preference. */
	encoderCombo->setProperty("preferredEncoder", QString::fromUtf8(obs_data_get_string(data, "encoder")));
	const char *mode = obs_data_get_string(data, "settings_mode");
	if (mode && strcmp(mode, "custom") == 0)
		settingsModeCombo->setCurrentIndex(settingsModeCombo->findData(QStringLiteral("custom")));
	const char *rc = obs_data_get_string(data, "rate_control");
	if (rc && *rc)
		rateControlCombo->setCurrentText(QString::fromUtf8(rc));
	if (obs_data_has_user_value(data, "bitrate"))
		bitrateSpin->setValue((int)obs_data_get_int(data, "bitrate"));
	if (obs_data_has_user_value(data, "cq"))
		cqSpin->setValue((int)obs_data_get_int(data, "cq"));
	const char *container = obs_data_get_string(data, "container");
	if (container && *container)
		containerCombo->setCurrentText(QString::fromUtf8(container));
	persistedOutputFolder = QString::fromUtf8(obs_data_get_string(data, "output_folder"));
	customBrowseFolder = QString::fromUtf8(obs_data_get_string(data, "browse_folder"));
	/* blockSignals: the toggled handler saves settings, which would
	 * clobber values not yet loaded into the UI */
	autoStartCheck->blockSignals(true);
	autoStartCheck->setChecked(obs_data_get_bool(data, "auto_start_replay_buffer"));
	autoStartCheck->blockSignals(false);
	if (obs_data_has_user_value(data, "enabled_tracks_mask"))
		enabledTracksMask = (int)obs_data_get_int(data, "enabled_tracks_mask");
	if (obs_data_has_user_value(data, "preview_quality")) {
		int idx = previewQualityCombo->findData((int)obs_data_get_int(data, "preview_quality"));
		if (idx >= 0)
			previewQualityCombo->setCurrentIndex(idx);
	}
	obs_data_release(data);
}

void ReplayClipEditorDialog::savePersistedSettings()
{
	char *dir = obs_module_config_path(nullptr);
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	/* Remember a custom output folder only when it differs from the
	 * clip's own folder (which is the per-clip default). */
	if (currentClip.valid) {
		QString folder = QDir::fromNativeSeparators(folderEdit->text().trimmed());
		if (folder == QFileInfo(currentClip.path).absolutePath())
			persistedOutputFolder.clear();
		else
			persistedOutputFolder = folder;
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "settings_mode",
			    settingsModeCombo->currentData().toString().toUtf8().constData());
	obs_data_set_string(data, "encoder", encoderCombo->currentData().toString().toUtf8().constData());
	obs_data_set_string(data, "rate_control", rateControlCombo->currentText().toUtf8().constData());
	obs_data_set_int(data, "bitrate", bitrateSpin->value());
	obs_data_set_int(data, "cq", cqSpin->value());
	obs_data_set_string(data, "container", containerCombo->currentText().toUtf8().constData());
	obs_data_set_string(data, "output_folder", persistedOutputFolder.toUtf8().constData());
	obs_data_set_string(data, "browse_folder", customBrowseFolder.toUtf8().constData());
	obs_data_set_bool(data, "auto_start_replay_buffer", autoStartCheck->isChecked());
	obs_data_set_int(data, "enabled_tracks_mask", enabledTracksMask);
	obs_data_set_int(data, "preview_quality", previewQualityCombo->currentData().toInt());
	obs_data_save_json(data, settingsFilePath().toUtf8().constData());
	obs_data_release(data);
}

/* ------------------------------------------------------------------ misc */

void ReplayClipEditorDialog::seekKeepingState(qint64 ms)
{
	if (player->isPlaying())
		player->seekTo(ms);
	else
		player->scrubTo(ms);
}

void ReplayClipEditorDialog::keyPressEvent(QKeyEvent *event)
{
	if (stack->currentIndex() != 1) {
		QDialog::keyPressEvent(event);
		return;
	}

	/* Don't steal keys while the user types in a text or number field. */
	QWidget *focus = focusWidget();
	if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus)) {
		QDialog::keyPressEvent(event);
		return;
	}

	switch (event->key()) {
	case Qt::Key_Space:
		player->togglePlayPause();
		break;
	case Qt::Key_I:
		timeline->setInPoint(player->currentMs());
		break;
	case Qt::Key_O:
		timeline->setOutPoint(player->currentMs());
		break;
	case Qt::Key_Left:
		if (event->modifiers() & Qt::ShiftModifier)
			seekKeepingState(player->currentMs() - 1000);
		else
			player->stepFrames(-1);
		break;
	case Qt::Key_Right:
		if (event->modifiers() & Qt::ShiftModifier)
			seekKeepingState(player->currentMs() + 1000);
		else
			player->stepFrames(1);
		break;
	case Qt::Key_Home:
		seekKeepingState(timeline->inPointMs());
		break;
	case Qt::Key_End:
		seekKeepingState(timeline->outPointMs());
		break;
	case Qt::Key_Escape:
		backToBrowser();
		break;
	default:
		QDialog::keyPressEvent(event);
	}
}

void ReplayClipEditorDialog::closeEvent(QCloseEvent *event)
{
	if (exportWorker && exportWorker->isRunning()) {
		/* Keep exporting; just hide the window. */
		hide();
		event->ignore();
		return;
	}
	filmstripGeneration++;
	player->closeFile();
	cleanupTemporaryReplay();
	timeline->clear();
	QDialog::closeEvent(event);
}
