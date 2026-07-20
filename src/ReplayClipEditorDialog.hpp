/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QDialog>
#include <QPointer>
#include <QTimer>

#include <atomic>

#include "VideoProbe.hpp"
#include "ExportWorker.hpp"

class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QComboBox;
class QSpinBox;
class QProgressBar;
class QCheckBox;

class PreviewPlayer;
class TimelineWidget;

/* Defined in plugin-main.cpp: saves the replay buffer and opens the editor
 * on the result (the Clip That flow). */
void request_clip_that();

class ReplayClipEditorDialog : public QDialog {
	Q_OBJECT

public:
	explicit ReplayClipEditorDialog(QWidget *parent = nullptr);
	~ReplayClipEditorDialog() override;

	void showBrowser();
	/* Bring the window up in a "saving replay" state while OBS muxes the
	 * buffer to disk — long buffers take a while. */
	void showSavingReplay();
	/* Bring the window up and load a clip straight into the editor
	 * (used by the Clip That flow). */
	void openClipExternal(const QString &path);
	/* Release OBS resources; must run while libobs is alive. */
	void shutdown();

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private slots:
	void refreshClipList();
	void openClip(const QString &path);
	void backToBrowser();
	void startExport();
	void cancelExport();

private:
	QWidget *buildBrowserPage();
	QWidget *buildEditorPage();
	QString replayFolder() const;
	QString effectiveBrowseFolder() const;
	void loadFilmstrip(const QString &path, qint64 startMs, qint64 endMs);
	void rebuildAudioTrackToggles();
	void updateTimeLabels();
	void updateSettingsModeUi();
	void updateExtensionLabel();
	void updateSizeEstimate();
	QString effectiveContainer() const;
	ExportSettings deriveObsSettings() const;
	ExportSettings customSettings() const;
	QString buildOutputPath() const;
	QVector<int> enabledAudioStreams() const;
	void seekKeepingState(qint64 ms);
	/* Delete a Clip That replay once a trim has been exported from it. */
	void cleanupTemporaryReplay();
	/* The clip's folder, except for temp replays where it is the real
	 * clips folder (the temp dir's parent) — exports must never land in
	 * the purged temp dir. */
	QString clipDefaultOutputFolder() const;
	void setExportUiBusy(bool busy);
	void loadPersistedSettings();
	void savePersistedSettings();

	/* pages */
	QStackedWidget *stack = nullptr;

	/* browser */
	QListWidget *clipList = nullptr;
	QLabel *folderLabel = nullptr;
	QPushButton *resetFolderBtn = nullptr;
	QCheckBox *autoStartCheck = nullptr;
	QString customBrowseFolder; /* empty = OBS recording folder */

	/* editor */
	QLabel *clipNameLabel = nullptr;
	PreviewPlayer *player = nullptr;
	TimelineWidget *timeline = nullptr;
	QToolButton *playButton = nullptr;
	QComboBox *previewQualityCombo = nullptr;
	QLabel *timeLabel = nullptr;
	QLabel *rangeLabel = nullptr;
	QWidget *audioTracksRow = nullptr;
	QVector<QCheckBox *> trackChecks;

	/* export controls */
	QComboBox *settingsModeCombo = nullptr;
	QWidget *customSettingsRow = nullptr;
	QComboBox *encoderCombo = nullptr;
	QComboBox *rateControlCombo = nullptr;
	QSpinBox *bitrateSpin = nullptr;
	QSpinBox *cqSpin = nullptr;
	QComboBox *containerCombo = nullptr;
	QLineEdit *folderEdit = nullptr;
	QLineEdit *nameEdit = nullptr;
	QLabel *extLabel = nullptr;
	QLabel *estSizeLabel = nullptr;
	QPushButton *exportButton = nullptr;
	QPushButton *cancelButton = nullptr;
	QProgressBar *progressBar = nullptr;
	QLabel *exportStatusLabel = nullptr;
	QPushButton *openFolderButton = nullptr;

	ClipInfo currentClip;
	bool currentClipIsTemporaryReplay = false;
	QTimer *viewDebounce = nullptr;
	QPointer<ExportWorker> exportWorker;
	std::atomic<int> filmstripGeneration{0};
	bool encodersLoaded = false;
	int enabledTracksMask = ~0; /* bit i = audio track i+1 enabled */
	QString persistedOutputFolder; /* empty = same folder as the clip */
	QString lastExportedFile;
};
