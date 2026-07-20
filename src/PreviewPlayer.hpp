/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QTimer>
#include <QVector>
#include <QWidget>

#include <obs.h>

#include "FFPlayerCore.hpp"

/* Registers the hidden async source type the preview pushes frames into.
 * Call once from obs_module_load. */
void register_preview_source();

/* Native window hosting an obs_display that renders one source (projector
 * technique). Owned by PreviewPlayer. */
class ObsDisplayWidget : public QWidget {
	Q_OBJECT

public:
	explicit ObsDisplayWidget(QWidget *parent = nullptr);
	~ObsDisplayWidget() override;

	void setSource(obs_source_t *s) { source = s; } /* not owned */
	void shutdown();

	QPaintEngine *paintEngine() const override { return nullptr; }

signals:
	void clicked();

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	bool event(QEvent *event) override;

private:
	void createDisplay();
	void destroyDisplay();
	static void drawCallback(void *data, uint32_t cx, uint32_t cy);

	obs_source_t *source = nullptr;
	obs_display_t *display = nullptr;
	void *boundHwnd = nullptr; /* HWND the display is attached to; Qt can
				      recreate the platform window on dialog
				      close/reopen */
};

/* Frame-accurate preview: FFPlayerCore decodes and pushes into a private
 * async OBS source; OBS renders it in the obs_display and monitors audio.
 * Seeks land on exact frames, playback resumes exactly at the shown frame,
 * and only the enabled audio tracks are audible. */
class PreviewPlayer : public QWidget {
	Q_OBJECT

public:
	explicit PreviewPlayer(QWidget *parent = nullptr);
	~PreviewPlayer() override;

	void openFile(const QString &path, double fps, qint64 durationMs, const QVector<int> &enabledAudioStreams);
	void closeFile();
	/* Destroy OBS objects; must be called while libobs is still alive. */
	void shutdown();

	void play();
	void pause();
	void togglePlayPause();
	bool isPlaying() const { return core.isPlaying(); }

	qint64 durationMs() const { return probedDurationMs; }
	qint64 currentMs() const { return core.positionMs(); }

	void scrubTo(qint64 ms);   /* pauses on the exact frame */
	void seekTo(qint64 ms);    /* jumps without changing play/pause state */
	void stepFrames(int count);
	void setEnabledTracks(const QVector<int> &streams);
	void setPreviewQuality(int level); /* 0 full, 1 half, 2 quarter */

signals:
	void positionChanged(qint64 ms);
	void playStateChanged(bool playing);

private:
	obs_source_t *source = nullptr;
	ObsDisplayWidget *displayWidget = nullptr;
	FFPlayerCore core;
	QTimer pollTimer;

	double fps = 60.0;
	qint64 probedDurationMs = 0;
	bool lastPlaying = false;
	int previewQuality = 0;
};
