/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <atomic>

struct ExportSettings {
	QString encoderName = QStringLiteral("libx264"); /* FFmpeg encoder id */
	QString rateControl = QStringLiteral("CQP");     /* CQP | CBR | VBR */
	int bitrateKbps = 16000;
	int cq = 23;
	QString container = QStringLiteral("mp4"); /* mp4 | mkv | mov */
};

struct EncoderChoice {
	QString id;   /* FFmpeg name, e.g. h264_nvenc */
	QString name; /* Display name */
};

/* Renders [inMs, outMs] of the source file to a new file. Video is re-encoded
 * with the chosen encoder; the enabled audio tracks are mixed down to one
 * stereo AAC track. */
class ExportWorker : public QThread {
	Q_OBJECT

public:
	ExportWorker(QString inputPath, QString outputPath, qint64 inMs, qint64 outMs,
		     QVector<int> audioStreamIndices, ExportSettings settings, QObject *parent = nullptr);

	void cancel() { cancelled = true; }

	/* Encoders that actually open on this machine (cached after first call). */
	static QVector<EncoderChoice> availableEncoders();

signals:
	void progressChanged(int percent);
	void exportFinished(bool success, const QString &messageOrPath);

protected:
	void run() override;

private:
	QString inputPath;
	QString outputPath;
	qint64 inMs;
	qint64 outMs;
	QVector<int> audioStreamIndices;
	ExportSettings settings;
	std::atomic<bool> cancelled{false};
};
