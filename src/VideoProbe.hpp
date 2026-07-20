/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include <functional>

struct AudioTrackInfo {
	int streamIndex = -1;    /* absolute stream index in the file */
	QString title;           /* from stream metadata, or "Track N" */
	int channels = 0;
	QString codecName;
};

struct ClipInfo {
	bool valid = false;
	QString path;
	qint64 durationMs = 0;
	int width = 0;
	int height = 0;
	double fps = 0.0;
	QVector<AudioTrackInfo> audioTracks;
};

namespace VideoProbe {

/* Read container/stream metadata without decoding. */
ClipInfo probe(const QString &path);

/* Decode one frame near timeMs, scaled down to at most maxWidth wide.
 * Returns a null image on failure. */
QImage extractFrame(const QString &path, qint64 timeMs, int maxWidth);

/* Decode `count` evenly spaced frames covering [startMs, startMs+spanMs],
 * sharing one demuxer/decoder with fast-decode shortcuts — much faster than
 * repeated extractFrame calls. `cancelled` is polled between frames; when it
 * returns true the job aborts and the result is empty. */
QVector<QImage> extractStrip(const QString &path, qint64 startMs, qint64 spanMs, int count, int maxWidth,
			     const std::function<bool()> &cancelled);

} // namespace VideoProbe
