/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QWidget>
#include <QImage>
#include <QVector>

/* Filmstrip timeline with draggable in/out trim handles, a playhead, a time
 * ruler, and zoom/scroll (wheel zooms at the cursor, Shift+wheel or
 * middle-drag pans), styled after the Steam Replay editor. */
class TimelineWidget : public QWidget {
	Q_OBJECT

public:
	explicit TimelineWidget(QWidget *parent = nullptr);

	void setDuration(qint64 ms);
	/* frames evenly cover [coverStartMs, coverEndMs] */
	void setFilmstrip(const QVector<QImage> &frames, qint64 coverStartMs, qint64 coverEndMs);
	void setFilmstripLoading(bool loading);
	void clear();

	qint64 inPointMs() const { return inMs; }
	qint64 outPointMs() const { return outMs; }
	qint64 playheadMs() const { return playMs; }
	qint64 viewStartMs() const { return viewStart; }
	qint64 viewEndMs() const { return viewEnd; }

	/* How many full-aspect tiles fit across the strip without cropping. */
	int desiredTileCount(double aspect) const;

	void setInPoint(qint64 ms);
	void setOutPoint(qint64 ms);
	void setPlayhead(qint64 ms);

	QSize sizeHint() const override { return QSize(640, 100); }
	QSize minimumSizeHint() const override { return QSize(320, 80); }

signals:
	void inPointChanged(qint64 ms);
	void outPointChanged(qint64 ms);
	void seekRequested(qint64 ms);
	/* Emitted while the user drags a trim handle, with the handle's
	 * position — lets the preview show the frame being trimmed to. */
	void handleScrubbed(qint64 ms);
	/* Visible window changed (zoom or pan). */
	void viewChanged(qint64 startMs, qint64 endMs);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void leaveEvent(QEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	enum class Drag { None, InHandle, OutHandle, Scrub, Pan, Minimap };

	QRectF stripRect() const;
	QRectF rulerRect() const;
	/* Bottom band holding the zoom indicator; grabbing it pans the view. */
	QRectF minimapBandRect() const;
	/* Map an x in the minimap band to a time over the full duration. */
	qint64 minimapXToMs(qreal x) const;
	qint64 xToMs(qreal x) const;
	qreal msToX(qint64 ms) const;
	qint64 minViewSpan() const;
	void clampView();
	void panBy(qint64 deltaMs);
	Drag hitTest(const QPointF &pos) const;
	void updateCursor(const QPointF &pos);

	qint64 durationMs = 0;
	qint64 inMs = 0;
	qint64 outMs = 0;
	qint64 playMs = 0;
	qint64 viewStart = 0;
	qint64 viewEnd = 0;
	QVector<QImage> filmstrip;
	qint64 filmstripStart = 0;
	qint64 filmstripEnd = 0;
	bool filmstripLoading = false;
	Drag dragging = Drag::None;
	qreal panLastX = 0;
	/* while dragging the minimap thumb, the grabbed point's offset from
	 * viewStart (keeps the thumb under the cursor) */
	qint64 minimapGrabMs = 0;
};
