/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QString>
#include <QVector>

#include <obs.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

/* Frame-accurate preview playback engine. Decodes with FFmpeg on a worker
 * thread and pushes frames/audio into an async OBS source, which handles
 * rendering (via obs_display) and audio monitoring. Unlike the stock media
 * source this seeks to exact frames, resumes playback exactly at the shown
 * frame, and live-mixes only the enabled audio tracks. */
class FFPlayerCore {
public:
	FFPlayerCore() = default;
	~FFPlayerCore();

	/* source is not owned; it must outlive close(). Playback starts
	 * immediately from startMs. */
	void open(const QString &path, obs_source_t *outputSource, const QVector<int> &enabledAudioStreams,
		  qint64 startMs, bool startPlaying);
	void close();

	void play();
	void pause();
	/* Decodes and shows the exact frame at ms; keeps play/pause state. */
	void seekMs(qint64 ms);
	void setEnabledTracks(const QVector<int> &streams);
	/* 0 = full, 1 = 1/2 (skip loop filter), 2 = 1/4 (also skip non-ref
	 * frames). Preview only — exports always decode fully. */
	void setQuality(int level) { quality = level; }

	bool isPlaying() const { return state.load() == 1; }
	bool isEnded() const { return state.load() == 2; }
	qint64 positionMs() const { return posMs.load(); }

private:
	void threadLoop();

	std::thread worker;
	mutable std::mutex cmdMutex;
	std::condition_variable cmdCv;

	/* command state, guarded by cmdMutex */
	bool quitFlag = false;
	bool wantPlaying = false;
	int64_t pendingSeekMs = -1;
	bool tracksDirty = false;
	QVector<int> enabledStreams;

	std::atomic<int64_t> posMs{0};
	std::atomic<int> state{0}; /* 0 = paused, 1 = playing, 2 = ended */
	std::atomic<int> quality{0};

	QString path;
	obs_source_t *source = nullptr;
};
